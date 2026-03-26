#include <jni.h>
#include <string>
#include <vector>
#include <array>
#include <cmath>
#include <chrono>
#include <atomic>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include "OboeCapture.h"

#define LOG_TAG "BelkaRx-Native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

std::mutex g_mutex;
int surfaceWidth = 0;
int surfaceHeight = 0;
constexpr int kTopMargin = 15;
std::vector<uint32_t> waterfallBuffer;
int waterfallHeadRow = 0;
std::atomic<int> g_renderHeadSnapshot{0};  // atomic snapshot for render thread
static std::vector<double> g_lineMagnitudes; // spectrum data for render thread
static std::mutex g_spectrumMutex;           // protects g_lineMagnitudes
std::vector<double> spectrumDecayBuffer;
int sensitivityValue = 100;
int contrastValue = 100;
int currentSampleRate = 96000;
bool swapIQEnabled = false;
bool zoomEnabled = false;
bool fastWaterfallEnabled = false;
bool showSpectrumEnabled = false;
bool isSpectrumFilled = false;  // true: filled bars, false: lines (default)
bool isSpectrumConstantColor = false;  // true: use constant middle color, false: use dynamic color (default)
int colorScale = 0;  // 0: Rainbow, 1: Light Blue, 2: Grayscale, 3: Cool-Hot, 4: Green Phosphor, 5: LCD

// Spectrum gain smoothing
double smoothedMinMag = 110.0;
double smoothedMaxMag = 170.0;

// Oboe capture state
OboeCapture* g_audioCapture = nullptr;
std::thread* g_audioThread = nullptr;
bool g_isCapturing = false;

// Global reference for calling back to Java processAndDraw
static JavaVM* g_vm = nullptr;
static jobject g_mainActivityRef = nullptr;
static jobject g_surfaceRef = nullptr;
static ANativeWindow* g_nativeWindow = nullptr;

void fft(std::vector<float>& real, std::vector<float>& imag) {
    size_t n = real.size();
    if (n <= 1) return;

    // Bit-reversal permutation
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }

    // Iterative Cooley-Tukey butterfly (no heap allocation)
    for (size_t len = 2; len <= n; len <<= 1) {
        float ang = static_cast<float>(-2.0 * M_PI / len);
        float wRe = cosf(ang);
        float wIm = sinf(ang);
        for (size_t i = 0; i < n; i += len) {
            float curRe = 1.0f, curIm = 0.0f;
            for (size_t j = 0; j < len / 2; j++) {
                float uRe = real[i + j];
                float uIm = imag[i + j];
                float tRe = curRe * real[i + j + len/2] - curIm * imag[i + j + len/2];
                float tIm = curRe * imag[i + j + len/2] + curIm * real[i + j + len/2];
                real[i + j]          = uRe + tRe;
                imag[i + j]          = uIm + tIm;
                real[i + j + len/2]  = uRe - tRe;
                imag[i + j + len/2]  = uIm - tIm;
                float nextRe = curRe * wRe - curIm * wIm;
                curIm         = curRe * wIm + curIm * wRe;
                curRe         = nextRe;
            }
        }
    }
}



extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setSurfaceSize(JNIEnv* env, jobject /* this */, jint width, jint height) {
    std::lock_guard<std::mutex> lock(g_mutex);
    surfaceWidth = width;
    surfaceHeight = height;
    int waterfallRows = height > kTopMargin ? (height - kTopMargin) : 0;
    waterfallBuffer.assign(width * waterfallRows, 0xFF000000);
    waterfallHeadRow = 0;
    g_renderHeadSnapshot.store(0, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setSensitivity(JNIEnv* env, jobject /* this */, jint value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    sensitivityValue = value;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setContrast(JNIEnv* env, jobject /* this */, jint value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    contrastValue = value;
    LOGI("setContrast: %d", value);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setZoom(JNIEnv* env, jobject /* this */, jboolean enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    zoomEnabled = (enabled == JNI_TRUE);
    LOGI("setZoom: %s", zoomEnabled ? "true (centered @+8kHz)" : "false (±24kHz @DC)");
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setFastWaterfall(JNIEnv* env, jobject /* this */, jboolean enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    fastWaterfallEnabled = (enabled == JNI_TRUE);
    LOGI("setFastWaterfall: %s", fastWaterfallEnabled ? "true" : "false");
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setShowSpectrum(JNIEnv* env, jobject /* this */, jboolean enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    showSpectrumEnabled = (enabled == JNI_TRUE);
    LOGI("setShowSpectrum: %s", showSpectrumEnabled ? "true" : "false");
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setColorScale(JNIEnv* env, jobject /* this */, jint scale) {
    std::lock_guard<std::mutex> lock(g_mutex);
    colorScale = scale;
    const char* scaleNames[] = {
        "Classic Rainbow", "Light Blue", "Grayscale", "Cool-Hot", "Green Phosphor", "LCD"
    };
    if (scale >= 0 && scale < 6) {
        LOGI("setColorScale: %d (%s)", scale, scaleNames[scale]);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setSpectrumFilled(JNIEnv* env, jobject /* this */, jboolean filled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    isSpectrumFilled = (filled == JNI_TRUE);
    LOGI("setSpectrumFilled: %s", isSpectrumFilled ? "true (filled bars)" : "false (lines)");
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setSpectrumConstantColor(JNIEnv* env, jobject /* this */, jboolean constant) {
    std::lock_guard<std::mutex> lock(g_mutex);
    isSpectrumConstantColor = (constant == JNI_TRUE);
    LOGI("setSpectrumConstantColor: %s", isSpectrumConstantColor ? "true (constant color)" : "false (dynamic color)");
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setNativeSampleRate(JNIEnv* env, jobject /* this */, jint rate) {
    std::lock_guard<std::mutex> lock(g_mutex);
    currentSampleRate = rate;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setSwapIQ(JNIEnv* env, jobject /* this */, jboolean swap) {
    std::lock_guard<std::mutex> lock(g_mutex);
    swapIQEnabled = swap;
    LOGI("setSwapIQ: %d", (int)swap);
}

static void drawArrowHead(uint32_t* dest, int stride, int centerX, int topY) {
    const uint32_t YELLOW = 0xFF00FFFF;
    const int ARROW_SIZE = 15;
    for (int i = 0; i <= ARROW_SIZE; i++) {
        int y = topY + i;
        if (y < surfaceHeight) {
            int width = ARROW_SIZE - i;
            for (int x = centerX - width; x <= centerX + width; x++) {
                if (x >= 0 && x < surfaceWidth) dest[y * stride + x] = YELLOW;
            }
        }
    }
}

static void drawFrequencyGridLines(uint32_t* dest, int stride, int markerX, int markerY, float Hz_per_pixel) {
    const uint32_t YELLOW = 0xFF00FFFF;
    const uint8_t font[12][5] = {
        {0x7, 0x5, 0x5, 0x5, 0x7}, {0x2, 0x6, 0x2, 0x2, 0x7}, {0x7, 0x1, 0x7, 0x4, 0x7},
        {0x7, 0x1, 0x7, 0x1, 0x7}, {0x5, 0x5, 0x7, 0x1, 0x1}, {0x7, 0x4, 0x7, 0x1, 0x7},
        {0x7, 0x4, 0x7, 0x5, 0x7}, {0x7, 0x1, 0x2, 0x2, 0x2}, {0x7, 0x5, 0x7, 0x5, 0x7},
        {0x7, 0x5, 0x7, 0x1, 0x7}, {0x0, 0x2, 0x7, 0x2, 0x0}, {0x0, 0x0, 0x7, 0x0, 0x0}
    };

    auto drawNum = [&](int x, int y, int num) {
        int scale = 3, cx = x;
        auto drawChar = [&](int charIdx) {
            for (int r = 0; r < 5; ++r) {
                for (int c = 0; c < 3; ++c) {
                    if ((font[charIdx][r] >> (2 - c)) & 1) {
                        for (int sy = 0; sy < scale; ++sy) {
                            for (int sx = 0; sx < scale; ++sx) {
                                int px = cx + c * scale + sx, py = y + r * scale + sy;
                                if (px >= 0 && px < surfaceWidth && py >= 0 && py < surfaceHeight)
                                    dest[py * stride + px] = YELLOW;
                            }
                        }
                    }
                }
            }
            cx += 4 * scale;
        };
        int numChars = 2;
        if (std::abs(num) >= 10) numChars = 3;
        if (std::abs(num) >= 100) numChars = 4;
        cx -= ((numChars * 4 - 1) * scale) / 2;
        if (num > 0) drawChar(10);
        else if (num < 0) { drawChar(11); num = -num; }
        if (num >= 100) {
            drawChar(num / 100); drawChar((num / 10) % 10); drawChar(num % 10);
        } else if (num >= 10) {
            drawChar(num / 10); drawChar(num % 10);
        } else {
            drawChar(num % 10);
        }
    };

    for (int offsetKHz = -50; offsetKHz <= 50; offsetKHz += 5) {
        float pixelOffset = (static_cast<float>(offsetKHz) * 1000.0f) / Hz_per_pixel;
        int pixelX = markerX + static_cast<int>(std::lround(pixelOffset));
        if (pixelX < 0 || pixelX >= surfaceWidth || offsetKHz == 0) continue;
        int lineHeight = (offsetKHz % 10 == 0) ? 20 : 10;
        for (int y = markerY; y < markerY + lineHeight && y < surfaceHeight; y++) {
            if (y >= 0) dest[y * stride + pixelX] = YELLOW;
        }
        if (offsetKHz % 10 == 0) drawNum(pixelX, markerY + lineHeight + 2, offsetKHz);
    }
}

void drawArrowMarkerOnWindow(uint32_t* dest, int stride, int markerPixelX, int markerPixelY) {
    if (markerPixelX < 0 || markerPixelX >= surfaceWidth) return;
    const int FFT_SIZE = 4096;
    const int BASE_BINS = FFT_SIZE / 4;
    const float HZ_PER_BIN = static_cast<float>(currentSampleRate) / FFT_SIZE;
    int visibleBins = zoomEnabled ? BASE_BINS / 2 : BASE_BINS;
    float Hz_per_pixel = static_cast<float>(visibleBins) * HZ_PER_BIN / static_cast<float>(surfaceWidth);
    drawArrowHead(dest, stride, markerPixelX, markerPixelY);
    drawFrequencyGridLines(dest, stride, markerPixelX, markerPixelY, Hz_per_pixel);
}

// --- Color Scale Helper Functions ---

static void applyRainbowScale(int intensityInt, uint8_t& r, uint8_t& g, uint8_t& b) {
    double norm = intensityInt / 255.0;
    if (norm < 0.25) {
        double t = norm * 4.0;
        r = 0; g = 0; b = static_cast<uint8_t>(t * 255);
    } else if (norm < 0.5) {
        double t = (norm - 0.25) * 4.0;
        r = 0; g = static_cast<uint8_t>(t * 255);
        b = static_cast<uint8_t>((1.0 - t) * 255 + t * 150);
    } else if (norm < 0.75) {
        double t = (norm - 0.5) * 4.0;
        r = static_cast<uint8_t>(t * 255); g = 255;
        b = static_cast<uint8_t>((1.0 - t) * 150);
    } else {
        double t = (norm - 0.75) * 4.0;
        r = 255; g = static_cast<uint8_t>((1.0 - t) * 255); b = 0;
    }
}

static void applyLightBlueScale(int intensityInt, uint8_t& r, uint8_t& g, uint8_t& b) {
    double norm = intensityInt / 255.0;
    if (norm < 0.33) {
        r = 0; g = 0; b = static_cast<uint8_t>(norm * 3.0 * 150);
    } else if (norm < 0.66) {
        double t = (norm - 0.33) * 3.0;
        r = 0; g = static_cast<uint8_t>(t * 206);
        b = static_cast<uint8_t>(150 + t * 105);
    } else {
        double t = (norm - 0.66) * 3.0;
        r = static_cast<uint8_t>(t * 255);
        g = static_cast<uint8_t>(206 + t * 49); b = 255;
    }
}

static void applyGrayscaleScale(int intensityInt, uint8_t& r, uint8_t& g, uint8_t& b) {
    auto gray = static_cast<uint8_t>(intensityInt);
    r = gray; g = gray; b = gray;
}

static uint8_t lerpChannel(uint8_t start, uint8_t end, double t) {
    return static_cast<uint8_t>(start + (end - start) * t);
}

static void applyGradientScale(
    int intensityInt,
    uint8_t& r,
    uint8_t& g,
    uint8_t& b,
    const double* positions,
    const uint8_t colors[][3],
    int stopCount) {
    double norm = intensityInt / 255.0;
    if (norm <= positions[0]) {
        r = colors[0][0];
        g = colors[0][1];
        b = colors[0][2];
        return;
    }
    if (norm >= positions[stopCount - 1]) {
        r = colors[stopCount - 1][0];
        g = colors[stopCount - 1][1];
        b = colors[stopCount - 1][2];
        return;
    }

    for (int i = 0; i < stopCount - 1; i++) {
        if (norm <= positions[i + 1]) {
            double localT = (norm - positions[i]) / (positions[i + 1] - positions[i]);
            r = lerpChannel(colors[i][0], colors[i + 1][0], localT);
            g = lerpChannel(colors[i][1], colors[i + 1][1], localT);
            b = lerpChannel(colors[i][2], colors[i + 1][2], localT);
            return;
        }
    }
}

static void applyGreenPhosphorScale(int intensityInt, uint8_t& r, uint8_t& g, uint8_t& b) {
    static const double positions[] = {0.0, 0.35, 0.65, 0.85, 1.0};
    static const uint8_t colors[][3] = {
        {6, 18, 6},
        {18, 78, 18},
        {32, 190, 32},
        {140, 255, 140},
        {228, 255, 196}
    };
    applyGradientScale(intensityInt, r, g, b, positions, colors, 5);
}

static void applyCoolHotScale(int intensityInt, uint8_t& r, uint8_t& g, uint8_t& b) {
    double norm = intensityInt / 255.0;
    if (norm < 0.25) {
        double t = norm * 4.0;
        r = static_cast<uint8_t>(t * 100); g = 0; b = static_cast<uint8_t>(t * 150);
    } else if (norm < 0.5) {
        double t = (norm - 0.25) * 4.0;
        r = static_cast<uint8_t>(100 + t * 155); g = 0;
        b = static_cast<uint8_t>((1.0 - t) * 150);
    } else if (norm < 0.75) {
        double t = (norm - 0.5) * 4.0;
        r = 255; g = static_cast<uint8_t>(t * 200); b = 0;
    } else {
        double t = (norm - 0.75) * 4.0;
        r = 255; g = static_cast<uint8_t>(200 + t * 55);
        b = static_cast<uint8_t>(t * 255);
    }
}

static void applyLcdScale(int intensityInt, uint8_t& r, uint8_t& g, uint8_t& b) {
    // LCD background color: #799aa4 (RGB). Stronger signal quickly shifts to dark gray then black.
    const uint8_t baseR = 0x79;
    const uint8_t baseG = 0x9A;
    const uint8_t baseB = 0xA4;
    double norm = intensityInt / 255.0;

    if (norm < 0.45) {
        double t = norm / 0.45;
        t = pow(t, 0.75);  // darker response earlier
        const uint8_t gray = 0x38;
        r = lerpChannel(baseR, gray, t);
        g = lerpChannel(baseG, gray, t);
        b = lerpChannel(baseB, gray, t);
    } else {
        double t = (norm - 0.45) / 0.55;
        t = pow(t, 0.70);  // pull into black faster
        const uint8_t gray = 0x38;
        r = lerpChannel(gray, 0x00, t);
        g = lerpChannel(gray, 0x00, t);
        b = lerpChannel(gray, 0x00, t);
    }
}

static uint32_t getColorWithParams(double intensity, int sensitivity, int contrast, int scale) {
    // intensity is already normalized to 0-255 range from the caller
    
    if (intensity < 0) intensity = 0;
    if (intensity > 255) intensity = 255;

    // Apply sensitivity adjustment
    intensity = intensity + (sensitivity - 100) * 0.8;
    if (intensity < 0) intensity = 0;
    if (intensity > 255) intensity = 255;

    // Calculate contrast exponent: 100 = normal (1.833), lower/higher values adjust curve sharpness
    // contrast ranges 0-300: 0 = 0.5 (soft), 100 = 1.833 (normal), 300 = 4.5 (very sharp)
    double contrastExponent = 0.5 + (contrast / 300.0) * 4.0;
    
    // Apply logarithmic curve with contrast adjustment
    intensity = intensity / 255.0;  // Normalize to 0-1
    intensity = pow(intensity, contrastExponent);  // Exponential curve: controls signal/noise ratio
    intensity = intensity * 255.0;  // Scale back to 0-255

    int intensityInt = (int)intensity;
    if (intensityInt < 0) intensityInt = 0;
    if (intensityInt > 255) intensityInt = 255;
    
    // Apply color scale
    uint8_t r, g, b;
    
    if (scale == 0) {
        applyRainbowScale(intensityInt, r, g, b);
    } else if (scale == 1) {
        applyLightBlueScale(intensityInt, r, g, b);
    } else if (scale == 2) {
        applyGrayscaleScale(intensityInt, r, g, b);
    } else if (scale == 3) {
        applyCoolHotScale(intensityInt, r, g, b);
    } else if (scale == 4) {
        applyGreenPhosphorScale(intensityInt, r, g, b);
    } else {
        applyLcdScale(intensityInt, r, g, b);
    }
    
    return 0xFF000000 | (b << 16) | (g << 8) | r;
}

uint32_t getColor(double intensity) {
    return getColorWithParams(intensity, sensitivityValue, contrastValue, colorScale);
}

static void buildColorLut(int sensitivity, int contrast, int scale, std::array<uint32_t, 256>& lut) {
    for (int i = 0; i < 256; i++) {
        double intensity = static_cast<double>(i);

        intensity = intensity + (sensitivity - 100) * 0.8;
        if (intensity < 0) intensity = 0;
        if (intensity > 255) intensity = 255;

        double contrastExponent = 0.5 + (contrast / 300.0) * 4.0;
        intensity = intensity / 255.0;
        intensity = pow(intensity, contrastExponent);
        intensity = intensity * 255.0;

        int intensityInt = static_cast<int>(intensity);
        if (intensityInt < 0) intensityInt = 0;
        if (intensityInt > 255) intensityInt = 255;

        uint8_t r, g, b;
        if (scale == 0) {
            applyRainbowScale(intensityInt, r, g, b);
        } else if (scale == 1) {
            applyLightBlueScale(intensityInt, r, g, b);
        } else if (scale == 2) {
            applyGrayscaleScale(intensityInt, r, g, b);
        } else if (scale == 3) {
            applyCoolHotScale(intensityInt, r, g, b);
        } else if (scale == 4) {
            applyGreenPhosphorScale(intensityInt, r, g, b);
        } else {
            applyLcdScale(intensityInt, r, g, b);
        }

        lut[i] = 0xFF000000 | (b << 16) | (g << 8) | r;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_processAndDraw(JNIEnv* env, jobject /* this */, jshortArray data, jint size, jobject surface) {
    if (surface == nullptr && g_nativeWindow == nullptr) return;

    using Clock = std::chrono::steady_clock;
    auto frameStart = Clock::now();
    int64_t unpackNs = 0;
    int64_t fftNs = 0;
    int64_t magPassNs = 0;
    int64_t waterfallUpdateNs = 0;
    int64_t windowBlitNs = 0;

    jshort* buffer = env->GetShortArrayElements(data, nullptr);
    if (buffer == nullptr) return;
    
    static bool loggedRate = false;
    if (!loggedRate) {
        loggedRate = true;
        LOGI("processAndDraw first call: currentSampleRate=%d, size=%d", currentSampleRate, size);
    }

    int fftSize = 4096;  // Larger FFT for better frequency resolution
    if (size < 2 * fftSize) {
        env->ReleaseShortArrayElements(data, buffer, JNI_ABORT);
        return;
    }

    // Use thread_local to avoid memory allocation on every frame
    thread_local std::vector<float> real;
    thread_local std::vector<float> imag;
    if (real.size() != fftSize) {
        real.resize(fftSize);
        imag.resize(fftSize);
    }

    {
        auto t0 = Clock::now();
        std::lock_guard<std::mutex> lock(g_mutex);
        if (swapIQEnabled) {
            for (int i = 0; i < fftSize; i++) {
                // I/Q handling: real = Q, imag = I
                real[i] = buffer[2 * i + 1] / 32768.0f;
                imag[i] = buffer[2 * i] / 32768.0f;
            }
        } else {
            for (int i = 0; i < fftSize; i++) {
                // Stereo interpretation: left = I (Real), right = Q (Imaginary)
                float leftSample = buffer[2 * i] / 32768.0f;
                float rightSample = buffer[2 * i + 1] / 32768.0f;
                
                // For SDR: treat left as I and right as Q (quadrature)
                real[i] = leftSample;
                imag[i] = rightSample;
            }
        }
        auto t1 = Clock::now();
        unpackNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        
    }
    env->ReleaseShortArrayElements(data, buffer, JNI_ABORT);

    auto fftStart = Clock::now();
    fft(real, imag);
    auto fftEnd = Clock::now();
    fftNs += std::chrono::duration_cast<std::chrono::nanoseconds>(fftEnd - fftStart).count();
    
    std::lock_guard<std::mutex> lock(g_mutex);
    int TOP_MARGIN = kTopMargin;
    int waterfallRows = surfaceHeight - TOP_MARGIN;
    
    // Copy volatile settings to avoid race conditions during rendering
    bool spectrum_filled = isSpectrumFilled;
    bool show_spectrum = showSpectrumEnabled;
    bool spectrum_constant_color = isSpectrumConstantColor;
    
    size_t expectedWaterfallSize = waterfallRows > 0 ? static_cast<size_t>(surfaceWidth * waterfallRows) : 0;
    if (surfaceWidth <= 0 || surfaceHeight <= 0 || waterfallBuffer.size() != expectedWaterfallSize) {
        return;
    }

    int shiftRows = fastWaterfallEnabled ? 3 : 2;

    if (!show_spectrum && waterfallRows > 0) {
        auto t0 = Clock::now();
        int rowAdvance = shiftRows < waterfallRows ? shiftRows : waterfallRows;
        waterfallHeadRow -= rowAdvance;
        while (waterfallHeadRow < 0) {
            waterfallHeadRow += waterfallRows;
        }
        auto t1 = Clock::now();
        waterfallUpdateNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    }

    // I/Q positive frequencies (0 Hz to Nyquist)
    // For I/Q signals, positive frequencies occupy bins 0..fftSize/2
    // At 96 kHz sampling: Nyquist = 48 kHz, so fftSize/2 = 1024 bins covering 0..48 kHz
    int baseBins = fftSize / 4;  // For 2048 FFT: 1024 bins = ±24 kHz centered at DC
    int visibleBins;
    int startBin;
    
    if (zoomEnabled) {
        // Zoom: centered at +8 kHz with ±6 kHz span (total 12 kHz width)
        visibleBins = baseBins / 2;  // 512 bins = 12 kHz
        int binFreqResolution = currentSampleRate / fftSize;  // Hz per bin (46.88 Hz at 96kHz)
        int targetBin = (8000 / binFreqResolution);  // +8 kHz position in FFT
        startBin = targetBin - (visibleBins / 2);  // Center at +8 kHz
    } else {
        // Normal mode: ±24 kHz centered at DC
        visibleBins = baseBins;
        startBin = -(visibleBins / 2);
    }
    
    // Draw new line
    double minMag = 1e9, maxMag = -1e9;
    
    // First pass: calculate min/max for current frame
    // Use thread_local to avoid memory allocation every frame
    thread_local std::vector<double> lineMagnitudes;
    if (lineMagnitudes.size() != surfaceWidth) {
        lineMagnitudes.resize(surfaceWidth);
    }
    
    // Scale precalculations to avoid excessive divisions inside loop
    double invSurfaceWidth = 1.0 / surfaceWidth;
    
    auto magStart = Clock::now();
    for (int x = 0; x < surfaceWidth; x++) {
        double logMag;
        
        if (zoomEnabled) {
            // ZOOM MODE: use linear interpolation
            double binPosReal = (x * (double)visibleBins) * invSurfaceWidth;
            int bin1 = (int)binPosReal;
            int bin2 = bin1 + 1;
            double frac = binPosReal - bin1;  // 0.0 to 1.0 interpolation factor
            double invFrac = 1.0 - frac;
            
            // Get bin indices with wrapping
            int shiftedBin1 = startBin + bin1;
            int shiftedBin2 = startBin + bin2;
            
            // Wrap negative bins
            if (shiftedBin1 < 0) shiftedBin1 += fftSize;
            else if (shiftedBin1 >= fftSize) shiftedBin1 -= fftSize;
            if (shiftedBin2 < 0) shiftedBin2 += fftSize;
            else if (shiftedBin2 >= fftSize) shiftedBin2 -= fftSize;
            
            // Calculate squared magnitudes to avoid sqrt
            double magSq1 = real[shiftedBin1] * real[shiftedBin1] + imag[shiftedBin1] * imag[shiftedBin1];
            double magSq2 = real[shiftedBin2] * real[shiftedBin2] + imag[shiftedBin2] * imag[shiftedBin2];
            
            // Linear interpolation of squared magnitude
            double interpMagSq = magSq1 * invFrac + magSq2 * frac;
            logMag = 10.0 * log10(interpMagSq + 1e-12) + 120.0;
        } else {
            // NORMAL MODE: direct bin lookup (as before)
            int binIdxInSpan = static_cast<int>((x * visibleBins) * invSurfaceWidth);
            int binIdx = startBin + binIdxInSpan;
            
            // Wrap bin index to handle negative frequencies
            int shiftedBin = binIdx >= 0 ? binIdx : binIdx + fftSize;
            if (shiftedBin >= fftSize) shiftedBin -= fftSize;

            // I/Q squared magnitude: I^2 + Q^2
            double magSq = real[shiftedBin] * real[shiftedBin] + imag[shiftedBin] * imag[shiftedBin];
            logMag = 10.0 * log10(magSq + 1e-12) + 120.0;
        }
        
        lineMagnitudes[x] = logMag;
        if (logMag < minMag) minMag = logMag;
        if (logMag > maxMag) maxMag = logMag;
    }
    auto magEnd = Clock::now();
    magPassNs += std::chrono::duration_cast<std::chrono::nanoseconds>(magEnd - magStart).count();
    
    // Process min/max smoothing based on display type
    double alpha = showSpectrumEnabled ? 1.0 : 0.4;  // Smoothing factor: 1.0 = instant for spectrum, 0.4 = smooth for waterfall
    smoothedMinMag = smoothedMinMag * (1.0 - alpha) + minMag * alpha;
    smoothedMaxMag = smoothedMaxMag * (1.0 - alpha) + maxMag * alpha;
    
    // Second pass: draw with normalized intensity using smoothed min/max
    if (!showSpectrumEnabled && waterfallRows > 0) {
        auto t0 = Clock::now();
        thread_local std::array<uint32_t, 256> colorLut;
        thread_local int lutSensitivity = -1;
        thread_local int lutContrast = -1;
        thread_local int lutScale = -1;

        if (lutSensitivity != sensitivityValue || lutContrast != contrastValue || lutScale != colorScale) {
            buildColorLut(sensitivityValue, contrastValue, colorScale, colorLut);
            lutSensitivity = sensitivityValue;
            lutContrast = contrastValue;
            lutScale = colorScale;
        }

        double range = smoothedMaxMag - smoothedMinMag + 1e-6;  // Avoid division by zero
        double invRange = 255.0 / range;
        
        // Fill the newest ring-buffer row sequentially
        uint32_t* firstRowBuffer = &waterfallBuffer[waterfallHeadRow * surfaceWidth];
        for (int x = 0; x < surfaceWidth; x++) {
            double logMag = lineMagnitudes[x];
            
            // Normalize intensity to 0-255 based on smoothed min/max
            double normalizedIntensity = (logMag - smoothedMinMag) * invRange;
            // Fast clamp
            normalizedIntensity = normalizedIntensity < 0.0 ? 0.0 : (normalizedIntensity > 255.0 ? 255.0 : normalizedIntensity);

            firstRowBuffer[x] = colorLut[static_cast<int>(normalizedIntensity)];
        }
        
        // Duplicate newest rows for fast waterfall mode.
        int duplicateRows = shiftRows < waterfallRows ? shiftRows : waterfallRows;
        for (int r = 1; r < duplicateRows; r++) {
            int physicalRow = waterfallHeadRow + r;
            if (physicalRow >= waterfallRows) {
                physicalRow -= waterfallRows;
            }
            memcpy(&waterfallBuffer[physicalRow * surfaceWidth], firstRowBuffer, surfaceWidth * sizeof(uint32_t));
        }
        auto t1 = Clock::now();
        waterfallUpdateNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    }
   
    auto blitStart = Clock::now();
    ANativeWindow* window = g_nativeWindow;
    bool temporaryWindow = false;
    if (window == nullptr && surface != nullptr) {
        window = ANativeWindow_fromSurface(env, surface);
        temporaryWindow = (window != nullptr);
    }
    if (window) {
        ANativeWindow_Buffer buffer_out;
        if (ANativeWindow_lock(window, &buffer_out, nullptr) == 0) {
            if (buffer_out.width >= surfaceWidth && buffer_out.height >= surfaceHeight) {
                auto* dest = static_cast<uint32_t*>(buffer_out.bits);
                
                if (show_spectrum) {
                    if (spectrumDecayBuffer.size() != (size_t)surfaceWidth) {
                        spectrumDecayBuffer.assign(surfaceWidth, -1000.0);
                    }
                    // LCD scale uses LCD-colored background; other scales use black.
                    const uint32_t spectrumBackgroundColor = (colorScale == 5) ? 0xFFA49A79 : 0xFF000000;

                    // Clear full background (including area above spectrum)
                    if (buffer_out.stride == surfaceWidth) {
                        std::fill_n(dest, surfaceWidth * surfaceHeight, spectrumBackgroundColor);
                    } else {
                        for (int y = 0; y < surfaceHeight; y++) {
                            std::fill_n(&dest[y * buffer_out.stride], surfaceWidth, spectrumBackgroundColor);
                        }
                    }
                    // Draw spectrum chart
                    int prevY = -1;
                    
                    // Manual Offset and Range based on sliders for Spectrum Mode
                    // Sensitivity (0-200) -> Reference level (top of display). Invert logic so higher sensitivity = lower refLevel (zooms into noise floor).
                    double refLevel = 250.0 - sensitivityValue; 
                    // Contrast (0-300) -> Range (display height, e.g., high contrast = 20 dB, low contrast = 160 dB)
                    double displayRange = 160.0 - (contrastValue / 300.0) * 140.0;
                    double minLevel = refLevel - displayRange;
                    double invDisplayRange = 1.0 / displayRange; // Pre-calc for optimization
                    
                    double decayRate = fastWaterfallEnabled ? 1.5 : 0.75;
                    int plotHeight = surfaceHeight - TOP_MARGIN;
                    
                    for (int x = 0; x < surfaceWidth; x++) {
                        double logMag = lineMagnitudes[x];
                        
                        // Apply decay
                        if (logMag > spectrumDecayBuffer[x]) {
                            spectrumDecayBuffer[x] = logMag;
                        } else {
                            spectrumDecayBuffer[x] -= decayRate;
                        }
                        
                        double normalized = (spectrumDecayBuffer[x] - minLevel) * invDisplayRange;
                        normalized = normalized < 0.0 ? 0.0 : (normalized > 1.0 ? 1.0 : normalized); // Fast clamp
                        
                        // Pick color based on the same color scale logic as the waterfall,
                        // but floor the normalized value for color at e.g. 0.2 (~50 out of 255) 
                        // so that deeply dipped signals don't turn completely black and disappear 
                        // against the black background.
                        double colorNormalized = normalized < 0.2 ? 0.2 : normalized;
                        
                        // If constant color mode, use 3/4 scale value instead of normalized value
                        if (spectrum_constant_color) {
                            colorNormalized = 0.75;  // 3/4 of scale
                        }
                        
                        uint32_t traceColor = getColor(colorNormalized * 255.0);
                        
                        int yPos = surfaceHeight - 1 - (int)(normalized * plotHeight);
                        yPos = yPos < TOP_MARGIN ? TOP_MARGIN : (yPos >= surfaceHeight ? surfaceHeight - 1 : yPos);
                        
                        if (spectrum_filled) {
                            // Draw filled bar from bottom (plotHeight) down to yPos
                            int bottomY = surfaceHeight - 1;
                            for (int y = yPos; y <= bottomY; y++) {
                                dest[y * buffer_out.stride + x] = traceColor;
                            }
                        } else {
                            // Draw continuous line between prevY and yPos
                            if (prevY != -1) {
                                int step = (yPos > prevY) ? 1 : -1;
                                int y = prevY;
                                while (y != yPos) {
                                    dest[y * buffer_out.stride + x] = traceColor;
                                    y += step;
                                }
                                dest[yPos * buffer_out.stride + x] = traceColor;
                            } else {
                                dest[yPos * buffer_out.stride + x] = traceColor;
                            }
                            prevY = yPos;
                        }
                    }
                } else {
                    // Draw waterfall data
                    int visibleTopMargin = TOP_MARGIN < surfaceHeight ? TOP_MARGIN : surfaceHeight;
                    if (buffer_out.stride == surfaceWidth) {
                        if (visibleTopMargin > 0) {
                            std::fill_n(dest, visibleTopMargin * surfaceWidth, 0xFF000000);
                        }
                        if (waterfallRows > 0) {
                            int firstChunkRows = waterfallRows - waterfallHeadRow;
                            memcpy(
                                &dest[TOP_MARGIN * surfaceWidth],
                                &waterfallBuffer[waterfallHeadRow * surfaceWidth],
                                firstChunkRows * surfaceWidth * sizeof(uint32_t)
                            );
                            if (waterfallHeadRow > 0) {
                                memcpy(
                                    &dest[(TOP_MARGIN + firstChunkRows) * surfaceWidth],
                                    waterfallBuffer.data(),
                                    waterfallHeadRow * surfaceWidth * sizeof(uint32_t)
                                );
                            }
                        }
                    } else {
                        for (int y = 0; y < visibleTopMargin; y++) {
                            std::fill_n(&dest[y * buffer_out.stride], surfaceWidth, 0xFF000000);
                        }
                        if (waterfallRows > 0) {
                            for (int y = TOP_MARGIN; y < surfaceHeight; y++) {
                                int dataRow = y - TOP_MARGIN;
                                int physicalRow = waterfallHeadRow + dataRow;
                                if (physicalRow >= waterfallRows) {
                                    physicalRow -= waterfallRows;
                                }
                                memcpy(&dest[y * buffer_out.stride], &waterfallBuffer[physicalRow * surfaceWidth], surfaceWidth * sizeof(uint32_t));
                            }
                        }
                    }
                }
                
                // Draw +8kHz marker arrow above the spectrum
                // The marker position depends on the current zoom mode
                int markerX;
                int markerYTop = 0;
                if (zoomEnabled) {
                    // Zoom mode: +8 kHz is the center of display
                    markerX = static_cast<int>(std::lround(surfaceWidth / 2.325));  // Center of screen = +8 kHz
                } else {
                    // Normal mode: ±24 kHz centered at DC, +8kHz is at 7/12 position  
                    markerX = static_cast<int>(std::lround((surfaceWidth * 7) / 11.05));
                }
                drawArrowMarkerOnWindow(dest, buffer_out.stride, markerX, markerYTop);
            }
            ANativeWindow_unlockAndPost(window);
        }
        if (temporaryWindow) {
            ANativeWindow_release(window);
        }
    }
    auto blitEnd = Clock::now();
    windowBlitNs += std::chrono::duration_cast<std::chrono::nanoseconds>(blitEnd - blitStart).count();

    auto frameEnd = Clock::now();
    int64_t frameNs = std::chrono::duration_cast<std::chrono::nanoseconds>(frameEnd - frameStart).count();

    thread_local int64_t profileWindowStartNs = 0;
    thread_local int profileFrames = 0;
    thread_local int64_t sumUnpackNs = 0;
    thread_local int64_t sumFftNs = 0;
    thread_local int64_t sumMagPassNs = 0;
    thread_local int64_t sumWaterfallUpdateNs = 0;
    thread_local int64_t sumWindowBlitNs = 0;
    thread_local int64_t sumFrameNs = 0;

    int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(frameEnd.time_since_epoch()).count();
    if (profileWindowStartNs == 0) {
        profileWindowStartNs = nowNs;
    }

    profileFrames++;
    sumUnpackNs += unpackNs;
    sumFftNs += fftNs;
    sumMagPassNs += magPassNs;
    sumWaterfallUpdateNs += waterfallUpdateNs;
    sumWindowBlitNs += windowBlitNs;
    sumFrameNs += frameNs;

    if (nowNs - profileWindowStartNs >= 2000000000LL && profileFrames > 0) {
        double invFrames = 1.0 / profileFrames;
        LOGI(
            "perf avg ms: frame=%.3f unpack=%.3f fft=%.3f mag=%.3f water=%.3f blit=%.3f fps=%.1f mode=%s",
            (sumFrameNs * invFrames) / 1e6,
            (sumUnpackNs * invFrames) / 1e6,
            (sumFftNs * invFrames) / 1e6,
            (sumMagPassNs * invFrames) / 1e6,
            (sumWaterfallUpdateNs * invFrames) / 1e6,
            (sumWindowBlitNs * invFrames) / 1e6,
            (profileFrames * 1e9) / (double)(nowNs - profileWindowStartNs),
            showSpectrumEnabled ? "spectrum" : "waterfall"
        );

        profileWindowStartNs = nowNs;
        profileFrames = 0;
        sumUnpackNs = 0;
        sumFftNs = 0;
        sumMagPassNs = 0;
        sumWaterfallUpdateNs = 0;
        sumWindowBlitNs = 0;
        sumFrameNs = 0;
    }
}

// ============================================================================
// Audio-only processing (FFT + waterfall update, no blit)
// Called from Kotlin AudioRecord thread. Decoupled from rendering so the audio
// thread is never blocked by ANativeWindow_lock waiting for a vsync buffer.
// ============================================================================
extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_processAudioData(JNIEnv* env, jobject, jshortArray data, jint size) {
    using Clock = std::chrono::steady_clock;
    auto frameStart = Clock::now();
    int64_t unpackNs = 0, fftNs = 0, magPassNs = 0, waterfallUpdateNs = 0;

    jshort* buffer = env->GetShortArrayElements(data, nullptr);
    if (buffer == nullptr) return;

    int fftSize = 4096;
    if (size < 2 * fftSize) {
        env->ReleaseShortArrayElements(data, buffer, JNI_ABORT);
        return;
    }

    thread_local std::vector<float> real, imag;
    if (real.size() != (size_t)fftSize) { real.resize(fftSize); imag.resize(fftSize); }

    {
        auto t0 = Clock::now();
        std::lock_guard<std::mutex> lk(g_mutex);
        if (swapIQEnabled) {
            for (int i = 0; i < fftSize; i++) {
                real[i] = buffer[2*i+1] / 32768.0f;
                imag[i] = buffer[2*i]   / 32768.0f;
            }
        } else {
            for (int i = 0; i < fftSize; i++) {
                real[i] = buffer[2*i]   / 32768.0f;
                imag[i] = buffer[2*i+1] / 32768.0f;
            }
        }
        unpackNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
    }
    env->ReleaseShortArrayElements(data, buffer, JNI_ABORT);

    auto fftStart = Clock::now();
    fft(real, imag);
    fftNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - fftStart).count();

    std::lock_guard<std::mutex> lk(g_mutex);
    const int TOP_MARGIN = kTopMargin;
    int waterfallRows = surfaceHeight - TOP_MARGIN;
    int expectedSize = (waterfallRows > 0) ? (surfaceWidth * waterfallRows) : 0;
    if (surfaceWidth <= 0 || surfaceHeight <= 0 || (int)waterfallBuffer.size() != expectedSize) return;

    int shiftRows = fastWaterfallEnabled ? 3 : 2;
    if (!showSpectrumEnabled && waterfallRows > 0) {
        auto t0 = Clock::now();
        int rowAdv = (shiftRows < waterfallRows) ? shiftRows : waterfallRows;
        waterfallHeadRow -= rowAdv;
        while (waterfallHeadRow < 0) waterfallHeadRow += waterfallRows;
        waterfallUpdateNs += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
    }

    int baseBins = fftSize / 4;
    int visibleBins, startBin;
    if (zoomEnabled) {
        visibleBins = baseBins / 2;
        int binHz = currentSampleRate / fftSize;
        startBin = (8000 / binHz) - visibleBins / 2;
    } else {
        visibleBins = baseBins;
        startBin = -(visibleBins / 2);
    }

    double minMag = 1e9, maxMag = -1e9;
    thread_local std::vector<double> lineMags;
    if ((int)lineMags.size() != surfaceWidth) lineMags.resize(surfaceWidth);
    double invW = 1.0 / surfaceWidth;

    auto magTs = Clock::now();
    for (int x = 0; x < surfaceWidth; x++) {
        double logMag;
        if (zoomEnabled) {
            double bpr = (x * (double)visibleBins) * invW;
            int b1 = (int)bpr, b2 = b1 + 1;
            double f = bpr - b1;
            int s1 = startBin + b1, s2 = startBin + b2;
            if (s1 < 0) s1 += fftSize; else if (s1 >= fftSize) s1 -= fftSize;
            if (s2 < 0) s2 += fftSize; else if (s2 >= fftSize) s2 -= fftSize;
            double sq1 = real[s1]*real[s1] + imag[s1]*imag[s1];
            double sq2 = real[s2]*real[s2] + imag[s2]*imag[s2];
            logMag = 10.0 * log10(sq1*(1.0-f) + sq2*f + 1e-12) + 120.0;
        } else {
            int bi = startBin + (int)((x * visibleBins) * invW);
            int sb = (bi >= 0) ? bi : bi + fftSize;
            if (sb >= fftSize) sb -= fftSize;
            double sq = real[sb]*real[sb] + imag[sb]*imag[sb];
            logMag = 10.0 * log10(sq + 1e-12) + 120.0;
        }
        lineMags[x] = logMag;
        if (logMag < minMag) minMag = logMag;
        if (logMag > maxMag) maxMag = logMag;
    }
    magPassNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - magTs).count();

    double alpha = showSpectrumEnabled ? 1.0 : 0.4;
    smoothedMinMag = smoothedMinMag * (1.0 - alpha) + minMag * alpha;
    smoothedMaxMag = smoothedMaxMag * (1.0 - alpha) + maxMag * alpha;

    if (!showSpectrumEnabled && waterfallRows > 0) {
        auto t0 = Clock::now();
        thread_local std::array<uint32_t, 256> colorLut;
        thread_local int lutSens = -1, lutContr = -1, lutSc = -1;
        if (lutSens != sensitivityValue || lutContr != contrastValue || lutSc != colorScale) {
            buildColorLut(sensitivityValue, contrastValue, colorScale, colorLut);
            lutSens = sensitivityValue; lutContr = contrastValue; lutSc = colorScale;
        }
        double range = smoothedMaxMag - smoothedMinMag + 1e-6;
        double invRange = 255.0 / range;
        uint32_t* row = &waterfallBuffer[waterfallHeadRow * surfaceWidth];
        for (int x = 0; x < surfaceWidth; x++) {
            double ni = (lineMags[x] - smoothedMinMag) * invRange;
            ni = ni < 0.0 ? 0.0 : (ni > 255.0 ? 255.0 : ni);
            row[x] = colorLut[(int)ni];
        }
        int dupRows = (shiftRows < waterfallRows) ? shiftRows : waterfallRows;
        for (int r = 1; r < dupRows; r++) {
            int pr = waterfallHeadRow + r;
            if (pr >= waterfallRows) pr -= waterfallRows;
            memcpy(&waterfallBuffer[pr * surfaceWidth], row, surfaceWidth * sizeof(uint32_t));
        }
        waterfallUpdateNs += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
        // Publish head row for render thread with release ordering
        g_renderHeadSnapshot.store(waterfallHeadRow, std::memory_order_release);
    } else if (showSpectrumEnabled) {
        std::lock_guard<std::mutex> specLk(g_spectrumMutex);
        if ((int)g_lineMagnitudes.size() != surfaceWidth) g_lineMagnitudes.resize(surfaceWidth);
        memcpy(g_lineMagnitudes.data(), lineMags.data(), surfaceWidth * sizeof(double));
    }

    auto frameEnd = Clock::now();
    int64_t frameNs = std::chrono::duration_cast<std::chrono::nanoseconds>(frameEnd - frameStart).count();
    thread_local int64_t profWin = 0; thread_local int profN = 0;
    thread_local int64_t sU = 0, sF = 0, sM = 0, sW = 0, sFr = 0;
    int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(frameEnd.time_since_epoch()).count();
    if (profWin == 0) profWin = nowNs;
    profN++; sU += unpackNs; sF += fftNs; sM += magPassNs; sW += waterfallUpdateNs; sFr += frameNs;
    if (nowNs - profWin >= 2000000000LL && profN > 0) {
        double inv = 1.0 / profN;
        LOGI("audio avg ms: frame=%.3f unpack=%.3f fft=%.3f mag=%.3f water=%.3f fps=%.1f",
            (sFr*inv)/1e6, (sU*inv)/1e6, (sF*inv)/1e6, (sM*inv)/1e6, (sW*inv)/1e6,
            (profN * 1e9) / (double)(nowNs - profWin));
        profWin = nowNs; profN = 0; sU = sF = sM = sW = sFr = 0;
    }
}

// ============================================================================
// Vsync-aligned blit (render only) - called from Choreographer on UI thread.
// ANativeWindow_lock fires right after vsync so a buffer is always immediately
// available, eliminating the ~5-6 ms blocking wait that was stalling the audio
// thread and causing stutter.
// ============================================================================
extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_renderFrame(JNIEnv* env, jobject, jobject surface) {
    using Clock = std::chrono::steady_clock;
    auto blitStart = Clock::now();

    ANativeWindow* window = g_nativeWindow;
    bool tempWin = false;
    if (!window && surface) {
        window = ANativeWindow_fromSurface(env, surface);
        tempWin = (window != nullptr);
    }
    if (!window) return;

    // Read atomic head row snapshot (acquire ordering ensures waterfall pixel visibility)
    int headRow = g_renderHeadSnapshot.load(std::memory_order_acquire);

    // Dimensions are written only from the UI thread (surfaceChanged → setSurfaceSize),
    // so reading them here on the UI thread (Choreographer) is race-free.
    int sw = surfaceWidth, sh = surfaceHeight;
    int waterfallRows = sh - kTopMargin;
    
    // Copy volatile settings under lock to prevent race conditions
    bool showSpec;
    bool spectrum_filled_copy;
    bool spectrum_constant_color_copy;
    bool fastWaterfall_copy;
    int sensitivity_copy;
    int contrast_copy;
    int colorScale_copy;
    bool zoom_copy;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        showSpec = showSpectrumEnabled;
        spectrum_filled_copy = isSpectrumFilled;
        spectrum_constant_color_copy = isSpectrumConstantColor;
        fastWaterfall_copy = fastWaterfallEnabled;
        sensitivity_copy = sensitivityValue;
        contrast_copy = contrastValue;
        colorScale_copy = colorScale;
        zoom_copy = zoomEnabled;
    }

    if (sw <= 0 || sh <= 0) { if (tempWin) ANativeWindow_release(window); return; }

    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(window, &buf, nullptr) == 0) {
        if (buf.width >= sw && buf.height >= sh) {
            auto* dest = static_cast<uint32_t*>(buf.bits);
            const int TOP_MARGIN = kTopMargin;

            if (showSpec) {
                std::vector<double> lineMagCopy;
                {
                    std::lock_guard<std::mutex> specLk(g_spectrumMutex);
                    lineMagCopy = g_lineMagnitudes;
                }
                if (!lineMagCopy.empty() && (int)lineMagCopy.size() == sw) {
                    if ((int)spectrumDecayBuffer.size() != sw)
                        spectrumDecayBuffer.assign(sw, -1000.0);
                    const uint32_t spectrumBackgroundColor = (colorScale_copy == 5) ? 0xFFA49A79 : 0xFF000000u;
                    if (buf.stride == sw)
                        std::fill_n(dest, sw * sh, spectrumBackgroundColor);
                    else
                        for (int y = 0; y < sh; y++)
                            std::fill_n(&dest[y * buf.stride], sw, spectrumBackgroundColor);
                    double refLevel = 250.0 - sensitivity_copy;
                    double displayRange = 160.0 - (contrast_copy / 300.0) * 140.0;
                    double minLevel = refLevel - displayRange;
                    double invDR = 1.0 / displayRange;
                    double decayRate = fastWaterfall_copy ? 1.5 : 0.75;
                    int plotH = sh - TOP_MARGIN;
                    int prevY = -1;
                    for (int x = 0; x < sw; x++) {
                        double lm = lineMagCopy[x];
                        if (lm > spectrumDecayBuffer[x]) spectrumDecayBuffer[x] = lm;
                        else spectrumDecayBuffer[x] -= decayRate;
                        double norm = (spectrumDecayBuffer[x] - minLevel) * invDR;
                        norm = norm < 0.0 ? 0.0 : (norm > 1.0 ? 1.0 : norm);
                        double cn = norm < 0.2 ? 0.2 : norm;
                        
                        // If constant color mode, use 3/4 of scale (0.75)
                        if (spectrum_constant_color_copy) {
                            cn = 0.75;
                        }
                        
                        uint32_t tc = getColorWithParams(cn * 255.0, sensitivity_copy, contrast_copy, colorScale_copy);
                        int yPos = sh - 1 - (int)(norm * plotH);
                        yPos = yPos < TOP_MARGIN ? TOP_MARGIN : (yPos >= sh ? sh - 1 : yPos);
                        if (spectrum_filled_copy) {
                            // Draw filled bar from bottom to yPos
                            int bottomY = sh - 1;
                            for (int y = yPos; y <= bottomY; y++) {
                                dest[y * buf.stride + x] = tc;
                            }
                        } else {
                            // Draw continuous line between prevY and yPos
                            if (prevY != -1) {
                                int step = (yPos > prevY) ? 1 : -1;
                                int y = prevY;
                                while (y != yPos) { dest[y * buf.stride + x] = tc; y += step; }
                                dest[yPos * buf.stride + x] = tc;
                            } else dest[yPos * buf.stride + x] = tc;
                            prevY = yPos;
                        }
                    }
                }
            } else {
                int vtm = TOP_MARGIN < sh ? TOP_MARGIN : sh;
                if (buf.stride == sw) {
                    if (vtm > 0) std::fill_n(dest, vtm * sw, 0xFF000000u);
                    if (waterfallRows > 0) {
                        std::lock_guard<std::mutex> wfLock(g_mutex);
                        int safeHeadRow = waterfallHeadRow;
                        int fc = waterfallRows - safeHeadRow;
                        memcpy(&dest[TOP_MARGIN * sw], &waterfallBuffer[safeHeadRow * sw],
                               fc * sw * sizeof(uint32_t));
                        if (safeHeadRow > 0)
                            memcpy(&dest[(TOP_MARGIN + fc) * sw], waterfallBuffer.data(),
                                   safeHeadRow * sw * sizeof(uint32_t));
                    }
                } else {
                    for (int y = 0; y < vtm; y++)
                        std::fill_n(&dest[y * buf.stride], sw, 0xFF000000u);
                    if (waterfallRows > 0) {
                        std::lock_guard<std::mutex> wfLock(g_mutex);
                        int safeHeadRow = waterfallHeadRow;
                        for (int y = TOP_MARGIN; y < sh; y++) {
                            int pr = safeHeadRow + (y - TOP_MARGIN);
                            if (pr >= waterfallRows) pr -= waterfallRows;
                            memcpy(&dest[y * buf.stride], &waterfallBuffer[pr * sw],
                                   sw * sizeof(uint32_t));
                        }
                    }
                }
            }

            int mx = zoom_copy
                ? (int)std::lround(sw / 2.325)
                : (int)std::lround((sw * 7) / 11.05);
            drawArrowMarkerOnWindow(dest, buf.stride, mx, 0);
        }
        ANativeWindow_unlockAndPost(window);
    }
    if (tempWin) ANativeWindow_release(window);

    int64_t blitNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - blitStart).count();
    thread_local int64_t profWin = 0; thread_local int profN = 0; thread_local int64_t sB = 0;
    int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    if (profWin == 0) profWin = nowNs;
    profN++; sB += blitNs;
    if (nowNs - profWin >= 2000000000LL && profN > 0) {
        double inv = 1.0 / profN;
        LOGI("render avg ms: blit=%.3f fps=%.1f", (sB * inv) / 1e6,
             (profN * 1e9) / (double)(nowNs - profWin));
        profWin = nowNs; profN = 0; sB = 0;
    }
}

// ============================================================================
// Oboe JNI Methods
// ============================================================================

// --- Audio Capture Helpers ---

struct AudioThreadEnv {
    JNIEnv* env;
    jshortArray frameArray;
    int16_t* buffer;
    bool attached;
};

static bool setupAudioEnvironment(AudioThreadEnv& audio) {
    audio.env = nullptr;
    audio.attached = false;
    audio.frameArray = nullptr;
    audio.buffer = nullptr;

    if (g_vm->GetEnv((void**)&audio.env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_vm->AttachCurrentThread(&audio.env, nullptr);
        audio.attached = true;
    }
    if (audio.env == nullptr) {
        LOGI("ERROR: Failed to get JNI environment");
        return false;
    }

    const int32_t fftSize = 2048;
    audio.buffer = new int16_t[fftSize * 2 * 4]; // 4x oversized for burst reads

    audio.frameArray = audio.env->NewShortArray(fftSize * 2);
    if (audio.frameArray == nullptr) {
        LOGI("ERROR: Failed to create frame array");
        delete[] audio.buffer;
        if (audio.attached) g_vm->DetachCurrentThread();
        return false;
    }
    return true;
}

// Audio capture thread function
void audioReadingThread() {
    if (g_audioCapture == nullptr || !g_isCapturing) return;

    AudioThreadEnv audio{};
    if (!setupAudioEnvironment(audio)) return;

    const int32_t fftSize = 2048;
    constexpr double kTargetRenderFps = 60.0;
    double renderBudget = 0.0;

    // Accumulator: collects burst-sized reads until we have a full fftSize frame
    std::vector<int16_t> accumulator;
    accumulator.reserve(fftSize * 2);

    while (g_isCapturing && g_audioCapture != nullptr) {
        // Read up to fftSize frames in one call (may return fewer in LOW_LATENCY mode)
        int32_t framesRead = g_audioCapture->readFrames(audio.buffer, fftSize);
        if (framesRead < 0) {
            LOGI("AAudio read error");
            break;
        }
        if (framesRead == 0) {
            usleep(10);
            continue;
        }

        // Append new samples to accumulator (stereo: 2 shorts per frame)
        int32_t newSamples = framesRead * 2;
        accumulator.insert(accumulator.end(), audio.buffer, audio.buffer + newSamples);

        // Only process once we have a full FFT window
        if ((int32_t)accumulator.size() < fftSize * 2) {
            continue;
        }

        audio.env->SetShortArrayRegion(audio.frameArray, 0, fftSize * 2, accumulator.data());

        // Keep any excess samples for the next frame (sliding window)
        if ((int32_t)accumulator.size() > fftSize * 2) {
            accumulator.erase(accumulator.begin(), accumulator.begin() + fftSize * 2);
        } else {
            accumulator.clear();
        }

        bool shouldRender = true;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!showSpectrumEnabled) {
                double sourceWindowsPerSecond = static_cast<double>(currentSampleRate) / fftSize;
                if (sourceWindowsPerSecond > kTargetRenderFps) {
                    renderBudget += kTargetRenderFps;
                    if (renderBudget >= sourceWindowsPerSecond) {
                        renderBudget -= sourceWindowsPerSecond;
                    } else {
                        shouldRender = false;
                    }
                }
            }
        }
        if (!shouldRender) {
            continue;
        }

        jobject surface = g_surfaceRef;
        Java_com_example_belkarx_MainActivity_processAndDraw(audio.env, nullptr, audio.frameArray, fftSize * 2, surface);
    }

    if (audio.frameArray != nullptr) {
        audio.env->DeleteLocalRef(audio.frameArray);
        audio.frameArray = nullptr;
    }
    if (audio.attached) g_vm->DetachCurrentThread();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_belkarx_MainActivity_startOboeCapture(JNIEnv* env, jobject thisObj, jint deviceId, jint sampleRate) {
    if (g_audioCapture != nullptr) {
        LOGI("Oboe capture already running");
        return JNI_FALSE;
    }

    // Save VM and Activity reference for callbacks
    env->GetJavaVM(&g_vm);
    g_mainActivityRef = env->NewGlobalRef(thisObj);

    LOGI("Starting Oboe capture: deviceId=%d, sampleRate=%d", deviceId, sampleRate);
    
    g_audioCapture = new OboeCapture();
    if (!g_audioCapture->start(deviceId, sampleRate)) {
        LOGI("Failed to start Oboe capture");
        delete g_audioCapture;
        g_audioCapture = nullptr;
        if (g_mainActivityRef != nullptr) {
            env->DeleteGlobalRef(g_mainActivityRef);
            g_mainActivityRef = nullptr;
        }
        return JNI_FALSE;
    }

    g_isCapturing = true;
    
    // Spawn audio reading thread
    g_audioThread = new std::thread(audioReadingThread);

    LOGI("Oboe capture started successfully");
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_stopOboeCapture(JNIEnv* env, jobject /* this */) {
    if (g_audioCapture == nullptr) return;

    LOGI("Stopping Oboe capture");
    g_isCapturing = false;
    
    if (g_audioThread != nullptr) {
        g_audioThread->join();
        delete g_audioThread;
        g_audioThread = nullptr;
    }

    g_audioCapture->stop();
    delete g_audioCapture;
    g_audioCapture = nullptr;

    // Clean up Java references
    if (g_mainActivityRef != nullptr) {
        env->DeleteGlobalRef(g_mainActivityRef);
        g_mainActivityRef = nullptr;
    }
    if (g_surfaceRef != nullptr) {
        env->DeleteGlobalRef(g_surfaceRef);
        g_surfaceRef = nullptr;
    }
    if (g_nativeWindow != nullptr) {
        ANativeWindow_release(g_nativeWindow);
        g_nativeWindow = nullptr;
    }

    LOGI("Oboe capture stopped");
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setOboeSurface(JNIEnv* env, jobject /* this */, jobject surface) {
    std::lock_guard<std::mutex> lock(g_mutex);

    // Clean up old surface reference if it exists
    if (g_surfaceRef != nullptr) {
        env->DeleteGlobalRef(g_surfaceRef);
        g_surfaceRef = nullptr;
        LOGI("setOboeSurface: Deleted old surface reference");
    }
    if (g_nativeWindow != nullptr) {
        ANativeWindow_release(g_nativeWindow);
        g_nativeWindow = nullptr;
        LOGI("setOboeSurface: Released old native window");
    }
    
    // Store new surface reference
    if (surface != nullptr) {
        g_surfaceRef = env->NewGlobalRef(surface);
        g_nativeWindow = ANativeWindow_fromSurface(env, surface);
        LOGI("setOboeSurface: Updated to new surface reference %p", surface);
    } else {
        g_surfaceRef = nullptr;
        LOGI("setOboeSurface: Cleared surface reference");
    }
}
