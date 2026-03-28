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
#include "NativeShared.h"
#include "AudioLifecycle.h"
#include "ColorScales.h"
#include "DspHelpers.h"
#include "RenderHelpers.h"

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
JavaVM* g_vm = nullptr;
jobject g_mainActivityRef = nullptr;
jobject g_surfaceRef = nullptr;
ANativeWindow* g_nativeWindow = nullptr;

namespace {

struct RenderConfigSnapshot {
    int sensitivity;
    int contrast;
    int sampleRate;
    int colorScaleValue;
    bool swapIq;
    bool zoom;
    bool fastWaterfall;
    bool showSpectrum;
    bool spectrumFilled;
    bool spectrumConstantColor;
};

int getWaterfallRows(int height) {
    return height > kTopMargin ? (height - kTopMargin) : 0;
}

int getShiftRows(bool fastWaterfall) {
    return fastWaterfall ? 3 : 2;
}

bool hasValidWaterfallBufferLocked(int waterfallRows) {
    size_t expectedWaterfallSize = waterfallRows > 0 ? static_cast<size_t>(surfaceWidth * waterfallRows) : 0;
    return surfaceWidth > 0 && surfaceHeight > 0 && waterfallBuffer.size() == expectedWaterfallSize;
}

void advanceWaterfallHeadLocked(int waterfallRows, int shiftRows) {
    if (waterfallRows <= 0) {
        return;
    }

    int rowAdvance = shiftRows < waterfallRows ? shiftRows : waterfallRows;
    waterfallHeadRow -= rowAdvance;
    while (waterfallHeadRow < 0) {
        waterfallHeadRow += waterfallRows;
    }
}

void updateSmoothedMagnitudeRange(double minMag, double maxMag, bool showSpectrum) {
    double alpha = showSpectrum ? 1.0 : 0.4;
    smoothedMinMag = smoothedMinMag * (1.0 - alpha) + minMag * alpha;
    smoothedMaxMag = smoothedMaxMag * (1.0 - alpha) + maxMag * alpha;
}

RenderConfigSnapshot snapshotRenderConfigLocked() {
    return {
        sensitivityValue,
        contrastValue,
        currentSampleRate,
        colorScale,
        swapIQEnabled,
        zoomEnabled,
        fastWaterfallEnabled,
        showSpectrumEnabled,
        isSpectrumFilled,
        isSpectrumConstantColor};
}

int computeMarkerX(int width, bool zoom) {
    if (zoom) {
        return static_cast<int>(std::lround(width / 2.325));
    }
    return static_cast<int>(std::lround((width * 7) / 11.05));
}

}  // namespace

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
Java_hu_ha8mz_belkarx_MainActivity_setSurfaceSize(JNIEnv* env, jobject /* this */, jint width, jint height) {
    std::lock_guard<std::mutex> lock(g_mutex);
    surfaceWidth = width;
    surfaceHeight = height;
    int waterfallRows = getWaterfallRows(height);
    waterfallBuffer.assign(width * waterfallRows, 0xFF000000);
    waterfallHeadRow = 0;
    g_renderHeadSnapshot.store(0, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setSensitivity(JNIEnv* env, jobject /* this */, jint value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    sensitivityValue = value;
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setContrast(JNIEnv* env, jobject /* this */, jint value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    contrastValue = value;
    LOGI("setContrast: %d", value);
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setZoom(JNIEnv* env, jobject /* this */, jboolean enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    zoomEnabled = (enabled == JNI_TRUE);
    LOGI("setZoom: %s", zoomEnabled ? "true (centered @+8kHz)" : "false (±24kHz @DC)");
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setFastWaterfall(JNIEnv* env, jobject /* this */, jboolean enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    fastWaterfallEnabled = (enabled == JNI_TRUE);
    LOGI("setFastWaterfall: %s", fastWaterfallEnabled ? "true" : "false");
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setShowSpectrum(JNIEnv* env, jobject /* this */, jboolean enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    showSpectrumEnabled = (enabled == JNI_TRUE);
    LOGI("setShowSpectrum: %s", showSpectrumEnabled ? "true" : "false");
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setColorScale(JNIEnv* env, jobject /* this */, jint scale) {
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
Java_hu_ha8mz_belkarx_MainActivity_setSpectrumFilled(JNIEnv* env, jobject /* this */, jboolean filled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    isSpectrumFilled = (filled == JNI_TRUE);
    LOGI("setSpectrumFilled: %s", isSpectrumFilled ? "true (filled bars)" : "false (lines)");
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setSpectrumConstantColor(JNIEnv* env, jobject /* this */, jboolean constant) {
    std::lock_guard<std::mutex> lock(g_mutex);
    isSpectrumConstantColor = (constant == JNI_TRUE);
    LOGI("setSpectrumConstantColor: %s", isSpectrumConstantColor ? "true (constant color)" : "false (dynamic color)");
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setNativeSampleRate(JNIEnv* env, jobject /* this */, jint rate) {
    std::lock_guard<std::mutex> lock(g_mutex);
    currentSampleRate = rate;
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setSwapIQ(JNIEnv* env, jobject /* this */, jboolean swap) {
    std::lock_guard<std::mutex> lock(g_mutex);
    swapIQEnabled = swap;
    LOGI("setSwapIQ: %d", (int)swap);
}

static void drawArrowHead(uint32_t* dest, int stride, int surfaceW, int surfaceH, int centerX, int topY) {
    const uint32_t YELLOW = 0xFF00FFFF;
    const int ARROW_SIZE = 15;
    for (int i = 0; i <= ARROW_SIZE; i++) {
        int y = topY + i;
        if (y < surfaceH) {
            int halfWidth = ARROW_SIZE - i;
            for (int x = centerX - halfWidth; x <= centerX + halfWidth; x++) {
                if (x >= 0 && x < surfaceW) dest[y * stride + x] = YELLOW;
            }
        }
    }
}

static void drawFrequencyGridLines(uint32_t* dest, int stride, int width, int height, int markerX, int markerY, float Hz_per_pixel) {
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
                                if (px >= 0 && px < width && py >= 0 && py < height)
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
        if (pixelX < 0 || pixelX >= width || offsetKHz == 0) continue;
        int lineHeight = (offsetKHz % 10 == 0) ? 20 : 10;
        for (int y = markerY; y < markerY + lineHeight && y < height; y++) {
            if (y >= 0) dest[y * stride + pixelX] = YELLOW;
        }
        if (offsetKHz % 10 == 0) drawNum(pixelX, markerY + lineHeight + 2, offsetKHz);
    }
}

void drawArrowMarkerOnWindow(uint32_t* dest, int stride, int width, int height, int sampleRate, bool zoom, int markerPixelX, int markerPixelY) {
    if (markerPixelX < 0 || markerPixelX >= width) return;
    const int FFT_SIZE = 4096;
    const int BASE_BINS = FFT_SIZE / 4;
    const float HZ_PER_BIN = static_cast<float>(sampleRate) / FFT_SIZE;
    int visibleBins = zoom ? BASE_BINS / 2 : BASE_BINS;
    float Hz_per_pixel = static_cast<float>(visibleBins) * HZ_PER_BIN / static_cast<float>(width);
    drawArrowHead(dest, stride, width, height, markerPixelX, markerPixelY);
    drawFrequencyGridLines(dest, stride, width, height, markerPixelX, markerPixelY, Hz_per_pixel);
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_processAndDraw(JNIEnv* env, jobject /* this */, jshortArray data, jint size, jobject surface) {
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
        unpackIqSamples(buffer, fftSize, swapIQEnabled, real, imag);
        auto t1 = Clock::now();
        unpackNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    }
    env->ReleaseShortArrayElements(data, buffer, JNI_ABORT);

    auto fftStart = Clock::now();
    fft(real, imag);
    auto fftEnd = Clock::now();
    fftNs += std::chrono::duration_cast<std::chrono::nanoseconds>(fftEnd - fftStart).count();
    
    std::lock_guard<std::mutex> lock(g_mutex);
    RenderConfigSnapshot config = snapshotRenderConfigLocked();
    int TOP_MARGIN = kTopMargin;
    int waterfallRows = getWaterfallRows(surfaceHeight);

    if (!hasValidWaterfallBufferLocked(waterfallRows)) {
        return;
    }

    int shiftRows = getShiftRows(config.fastWaterfall);

    if (!config.showSpectrum && waterfallRows > 0) {
        auto t0 = Clock::now();
        advanceWaterfallHeadLocked(waterfallRows, shiftRows);
        auto t1 = Clock::now();
        waterfallUpdateNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    }

    SpectrumSpan span = computeSpectrumSpan(fftSize, config.sampleRate, config.zoom);
    double minMag = 1e9, maxMag = -1e9;
    
    // First pass: calculate min/max for current frame
    // Use thread_local to avoid memory allocation every frame
    thread_local std::vector<double> lineMagnitudes;
    if (lineMagnitudes.size() != surfaceWidth) {
        lineMagnitudes.resize(surfaceWidth);
    }
    
    auto magStart = Clock::now();
    computeLineMagnitudes(real, imag, fftSize, surfaceWidth, config.zoom, span, lineMagnitudes, minMag, maxMag);
    auto magEnd = Clock::now();
    magPassNs += std::chrono::duration_cast<std::chrono::nanoseconds>(magEnd - magStart).count();
    
    updateSmoothedMagnitudeRange(minMag, maxMag, config.showSpectrum);
    
    // Second pass: draw with normalized intensity using smoothed min/max
    if (!config.showSpectrum && waterfallRows > 0) {
        auto t0 = Clock::now();
        thread_local std::array<uint32_t, 256> colorLut;
        thread_local int lutSensitivity = -1;
        thread_local int lutContrast = -1;
        thread_local int lutScale = -1;

        if (lutSensitivity != config.sensitivity || lutContrast != config.contrast || lutScale != config.colorScaleValue) {
            buildColorLut(config.sensitivity, config.contrast, config.colorScaleValue, colorLut);
            lutSensitivity = config.sensitivity;
            lutContrast = config.contrast;
            lutScale = config.colorScaleValue;
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
                
                if (config.showSpectrum) {
                    SpectrumRenderParams spectrumParams{
                        surfaceWidth,
                        surfaceHeight,
                        TOP_MARGIN,
                        config.sensitivity,
                        config.contrast,
                        config.colorScaleValue,
                        config.fastWaterfall,
                        config.spectrumFilled,
                        config.spectrumConstantColor};
                    drawSpectrumFrame(dest, buffer_out.stride, lineMagnitudes, spectrumDecayBuffer, spectrumParams);
                } else {
                    blitWaterfallFrame(
                        dest,
                        buffer_out.stride,
                        waterfallBuffer,
                        surfaceWidth,
                        surfaceHeight,
                        TOP_MARGIN,
                        waterfallRows,
                        waterfallHeadRow,
                        0xFF000000u);
                }
                
                // Draw +8kHz marker arrow above the spectrum
                // The marker position depends on the current zoom mode
                int markerYTop = 0;
                int markerX = computeMarkerX(surfaceWidth, config.zoom);
                drawArrowMarkerOnWindow(dest, buffer_out.stride, surfaceWidth, surfaceHeight, config.sampleRate, config.zoom, markerX, markerYTop);
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
Java_hu_ha8mz_belkarx_MainActivity_processAudioData(JNIEnv* env, jobject, jshortArray data, jint size) {
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
        unpackIqSamples(buffer, fftSize, swapIQEnabled, real, imag);
        unpackNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
    }
    env->ReleaseShortArrayElements(data, buffer, JNI_ABORT);

    auto fftStart = Clock::now();
    fft(real, imag);
    fftNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - fftStart).count();

    std::lock_guard<std::mutex> lk(g_mutex);
    const int TOP_MARGIN = kTopMargin;
    RenderConfigSnapshot config = snapshotRenderConfigLocked();
    int waterfallRows = getWaterfallRows(surfaceHeight);
    if (!hasValidWaterfallBufferLocked(waterfallRows)) return;

    int shiftRows = getShiftRows(config.fastWaterfall);
    if (!config.showSpectrum && waterfallRows > 0) {
        auto t0 = Clock::now();
        advanceWaterfallHeadLocked(waterfallRows, shiftRows);
        waterfallUpdateNs += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
    }

    SpectrumSpan span = computeSpectrumSpan(fftSize, config.sampleRate, config.zoom);

    double minMag = 1e9, maxMag = -1e9;
    thread_local std::vector<double> lineMags;
    if ((int)lineMags.size() != surfaceWidth) lineMags.resize(surfaceWidth);
    auto magTs = Clock::now();
    computeLineMagnitudes(real, imag, fftSize, surfaceWidth, config.zoom, span, lineMags, minMag, maxMag);
    magPassNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - magTs).count();

    updateSmoothedMagnitudeRange(minMag, maxMag, config.showSpectrum);

    if (!config.showSpectrum && waterfallRows > 0) {
        auto t0 = Clock::now();
        thread_local std::array<uint32_t, 256> colorLut;
        thread_local int lutSens = -1, lutContr = -1, lutSc = -1;
        if (lutSens != config.sensitivity || lutContr != config.contrast || lutSc != config.colorScaleValue) {
            buildColorLut(config.sensitivity, config.contrast, config.colorScaleValue, colorLut);
            lutSens = config.sensitivity; lutContr = config.contrast; lutSc = config.colorScaleValue;
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
    } else if (config.showSpectrum) {
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
Java_hu_ha8mz_belkarx_MainActivity_renderFrame(JNIEnv* env, jobject, jobject surface) {
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
    int waterfallRows = getWaterfallRows(sh);
    
    RenderConfigSnapshot config;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        config = snapshotRenderConfigLocked();
    }

    if (sw <= 0 || sh <= 0) { if (tempWin) ANativeWindow_release(window); return; }

    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(window, &buf, nullptr) == 0) {
        if (buf.width >= sw && buf.height >= sh) {
            auto* dest = static_cast<uint32_t*>(buf.bits);
            const int TOP_MARGIN = kTopMargin;

            if (config.showSpectrum) {
                std::vector<double> lineMagCopy;
                {
                    std::lock_guard<std::mutex> specLk(g_spectrumMutex);
                    lineMagCopy = g_lineMagnitudes;
                }
                SpectrumRenderParams spectrumParams{
                    sw,
                    sh,
                    TOP_MARGIN,
                    config.sensitivity,
                    config.contrast,
                    config.colorScaleValue,
                    config.fastWaterfall,
                    config.spectrumFilled,
                    config.spectrumConstantColor};
                drawSpectrumFrame(dest, buf.stride, lineMagCopy, spectrumDecayBuffer, spectrumParams);
            } else {
                std::lock_guard<std::mutex> wfLock(g_mutex);
                int safeHeadRow = waterfallHeadRow;
                blitWaterfallFrame(
                    dest,
                    buf.stride,
                    waterfallBuffer,
                    sw,
                    sh,
                    TOP_MARGIN,
                    waterfallRows,
                    safeHeadRow,
                    0xFF000000u);
            }

            int mx = computeMarkerX(sw, config.zoom);
            drawArrowMarkerOnWindow(dest, buf.stride, sw, sh, config.sampleRate, config.zoom, mx, 0);
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

extern "C" JNIEXPORT jboolean JNICALL
Java_hu_ha8mz_belkarx_MainActivity_startOboeCapture(JNIEnv* env, jobject thisObj, jint deviceId, jint sampleRate) {
    return startOboeCaptureImpl(env, thisObj, deviceId, sampleRate) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_stopOboeCapture(JNIEnv* env, jobject /* this */) {
    stopOboeCaptureImpl(env);
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setOboeSurface(JNIEnv* env, jobject /* this */, jobject surface) {
    setOboeSurfaceImpl(env, surface);
}
