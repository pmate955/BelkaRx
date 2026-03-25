#include <jni.h>
#include <string>
#include <vector>
#include <cmath>
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
std::vector<uint32_t> waterfallBuffer;
std::vector<double> spectrumDecayBuffer;
int sensitivityValue = 100;
int contrastValue = 100;
int currentSampleRate = 96000;
bool swapIQEnabled = false;
bool zoomEnabled = false;
bool fastWaterfallEnabled = false;
bool showSpectrumEnabled = false;
int colorScale = 0;  // 0: Grayscale, 1: Black-Blue, 2: Blue-Green-Red

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

void fft(std::vector<double>& real, std::vector<double>& imag) {
    size_t n = real.size();
    if (n <= 1) return;

    std::vector<double> real_even(n / 2), imag_even(n / 2);
    std::vector<double> real_odd(n / 2), imag_odd(n / 2);

    for (size_t i = 0; i < n / 2; i++) {
        real_even[i] = real[2 * i];
        imag_even[i] = imag[2 * i];
        real_odd[i] = real[2 * i + 1];
        imag_odd[i] = imag[2 * i + 1];
    }

    fft(real_even, imag_even);
    fft(real_odd, imag_odd);

    for (size_t i = 0; i < n / 2; i++) {
        double angle = -2.0 * M_PI * i / n;
        double wr = cos(angle);
        double wi = sin(angle);
        double tr = wr * real_odd[i] - wi * imag_odd[i];
        double ti = wr * imag_odd[i] + wi * real_odd[i];
        real[i] = real_even[i] + tr;
        imag[i] = imag_even[i] + ti;
        real[i + n / 2] = real_even[i] - tr;
        imag[i + n / 2] = imag_even[i] - ti;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setSurfaceSize(JNIEnv* env, jobject /* this */, jint width, jint height) {
    std::lock_guard<std::mutex> lock(g_mutex);
    surfaceWidth = width;
    surfaceHeight = height;
    waterfallBuffer.assign(width * height, 0xFF000000);
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
    const char* scaleNames[] = {"Classic Rainbow", "Light Blue", "Grayscale", "Cool-Hot"};
    if (scale >= 0 && scale < 4) {
        LOGI("setColorScale: %d (%s)", scale, scaleNames[scale]);
    }
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
    const int FFT_SIZE = 2048;
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

uint32_t getColor(double intensity) {
    // intensity is already normalized to 0-255 range from the caller
    
    if (intensity < 0) intensity = 0;
    if (intensity > 255) intensity = 255;

    // Apply sensitivity adjustment
    intensity = intensity + (sensitivityValue - 100) * 0.8;
    if (intensity < 0) intensity = 0;
    if (intensity > 255) intensity = 255;

    // Calculate contrast exponent: 100 = normal (1.833), lower/higher values adjust curve sharpness
    // contrastValue ranges 0-300: 0 = 0.5 (soft), 100 = 1.833 (normal), 300 = 4.5 (very sharp)
    double contrastExponent = 0.5 + (contrastValue / 300.0) * 4.0;
    
    // Apply logarithmic curve with contrast adjustment
    intensity = intensity / 255.0;  // Normalize to 0-1
    intensity = pow(intensity, contrastExponent);  // Exponential curve: controls signal/noise ratio
    intensity = intensity * 255.0;  // Scale back to 0-255

    int intensityInt = (int)intensity;
    if (intensityInt < 0) intensityInt = 0;
    if (intensityInt > 255) intensityInt = 255;
    
    // Apply color scale
    uint8_t r, g, b;
    
    if (colorScale == 0) {
        applyRainbowScale(intensityInt, r, g, b);
    } else if (colorScale == 1) {
        applyLightBlueScale(intensityInt, r, g, b);
    } else if (colorScale == 2) {
        applyGrayscaleScale(intensityInt, r, g, b);
    } else {  // colorScale == 3
        applyCoolHotScale(intensityInt, r, g, b);
    }
    
    return 0xFF000000 | (b << 16) | (g << 8) | r;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_processAndDraw(JNIEnv* env, jobject /* this */, jshortArray data, jint size, jobject surface) {
    if (surface == nullptr) return;

    jshort* buffer = env->GetShortArrayElements(data, nullptr);
    if (buffer == nullptr) return;
    
    static bool loggedRate = false;
    if (!loggedRate) {
        loggedRate = true;
        LOGI("processAndDraw first call: currentSampleRate=%d, size=%d", currentSampleRate, size);
    }

    int fftSize = 2048;  // Larger FFT for better frequency resolution
    if (size < 2 * fftSize) {
        env->ReleaseShortArrayElements(data, buffer, JNI_ABORT);
        return;
    }

    // Use thread_local to avoid memory allocation on every frame
    thread_local std::vector<double> real;
    thread_local std::vector<double> imag;
    if (real.size() != fftSize) {
        real.resize(fftSize);
        imag.resize(fftSize);
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (swapIQEnabled) {
            for (int i = 0; i < fftSize; i++) {
                // I/Q handling: real = Q, imag = I
                real[i] = buffer[2 * i + 1] / 32768.0;
                imag[i] = buffer[2 * i] / 32768.0;
            }
        } else {
            for (int i = 0; i < fftSize; i++) {
                // Stereo interpretation: left = I (Real), right = Q (Imaginary)
                double leftSample = buffer[2 * i] / 32768.0;
                double rightSample = buffer[2 * i + 1] / 32768.0;
                
                // For SDR: treat left as I and right as Q (quadrature)
                real[i] = leftSample;
                imag[i] = rightSample;
            }
        }
        
        // Debug logging for contrast
        static int contrastLogCount = 0;
        if (++contrastLogCount % 50 == 0) {
            LOGI("Contrast setting: contrastValue=%d", contrastValue);
        }
        
        // Debug: log input data samples
        static int inputLogCount = 0;
        if (++inputLogCount % 100 == 0) {
            int16_t minI = 32767, maxI = -32768, minQ = 32767, maxQ = -32768;
            for (int i = 0; i < fftSize; i++) {
                int16_t I_val = buffer[2 * i];  // Raw buffer, not normalized
                int16_t Q_val = buffer[2 * i + 1];
                minI = (I_val < minI) ? I_val : minI;
                maxI = (I_val > maxI) ? I_val : maxI;
                minQ = (Q_val < minQ) ? Q_val : minQ;
                maxQ = (Q_val > maxQ) ? Q_val : maxQ;
            }
            LOGI("FFT Input RAW: I[min=%d, max=%d, range=%d], Q[min=%d, max=%d, range=%d]", 
                 minI, maxI, maxI-minI, minQ, maxQ, maxQ-minQ);
        }
    }
    env->ReleaseShortArrayElements(data, buffer, JNI_ABORT);

    fft(real, imag);
    
    // Check FFT output immediately
    static int fftCheckCount = 0;
    if (++fftCheckCount % 20 == 0) {
        double mag0 = sqrt(real[0]*real[0] + imag[0]*imag[0]);
        double mag128 = sqrt(real[128]*real[128] + imag[128]*imag[128]);
        double mag512 = sqrt(real[512]*real[512] + imag[512]*imag[512]);
        
        LOGI("FFT Check #%d [SWAP=%d]: mag[0]=%.6f, mag[128]=%.6f, mag[512]=%.6f, real[0]=%.6f, imag[0]=%.6f",
             fftCheckCount, (int)swapIQEnabled, mag0, mag128, mag512, real[0], imag[0]);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (surfaceWidth <= 0 || surfaceHeight <= 0 || waterfallBuffer.size() != (size_t)(surfaceWidth * surfaceHeight)) {
        return;
    }

    int TOP_MARGIN = 15;
    int shiftRows = fastWaterfallEnabled ? 2 : 1;

    if (!showSpectrumEnabled) {
        // Shift waterfall down
        for (int y = surfaceHeight - 1; y >= TOP_MARGIN + shiftRows; y--) {
            memcpy(&waterfallBuffer[y * surfaceWidth], &waterfallBuffer[(y - shiftRows) * surfaceWidth], surfaceWidth * sizeof(uint32_t));
        }
        
        // Clear top margin to black (fully opaque)
        for (int y = 0; y < TOP_MARGIN; y++) {
            for (int x = 0; x < surfaceWidth; x++) {
                waterfallBuffer[y * surfaceWidth + x] = 0xFF000000;
            }
        }
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
    
    LOGI("Spectrum: %s, visibleBins=%d, startBin=%d, fftSize=%d, sampleRate=%d, displayBW=%.1f kHz, Hz/pixel=%.1f", 
         zoomEnabled ? "ZOOM +8kHz (interpolated)" : "normal ±24kHz", visibleBins, startBin, fftSize, currentSampleRate, 
         (float)visibleBins * currentSampleRate / fftSize / 1000,
         (float)visibleBins * currentSampleRate / fftSize / surfaceWidth);

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
    
    // Process min/max smoothing based on display type
    double alpha = showSpectrumEnabled ? 1.0 : 0.4;  // Smoothing factor: 1.0 = instant for spectrum, 0.4 = smooth for waterfall
    smoothedMinMag = smoothedMinMag * (1.0 - alpha) + minMag * alpha;
    smoothedMaxMag = smoothedMaxMag * (1.0 - alpha) + maxMag * alpha;
    
    // Second pass: draw with normalized intensity using smoothed min/max
    if (!showSpectrumEnabled) {
        double range = smoothedMaxMag - smoothedMinMag + 1e-6;  // Avoid division by zero
        double invRange = 255.0 / range;
        
        // Fill the first new row sequentially
        uint32_t* firstRowBuffer = &waterfallBuffer[TOP_MARGIN * surfaceWidth];
        for (int x = 0; x < surfaceWidth; x++) {
            double logMag = lineMagnitudes[x];
            
            // Normalize intensity to 0-255 based on smoothed min/max
            double normalizedIntensity = (logMag - smoothedMinMag) * invRange;
            // Fast clamp
            normalizedIntensity = normalizedIntensity < 0.0 ? 0.0 : (normalizedIntensity > 255.0 ? 255.0 : normalizedIntensity);
            
            firstRowBuffer[x] = getColor(normalizedIntensity);
        }
        
        // Copy the first row if shiftRows > 1
        for (int r = 1; r < shiftRows; r++) {
            memcpy(&waterfallBuffer[(TOP_MARGIN + r) * surfaceWidth], firstRowBuffer, surfaceWidth * sizeof(uint32_t));
        }
    }
    static int drawCount = 0;
    if (++drawCount % 10 == 0) {
        // Log first few FFT bins to see actual magnitude values
        const char* mode = swapIQEnabled ? "I/Q_SWAP" : "I/Q";
        LOGI("FFT Draw #%d [%s]: frameMag=[%.1f-%.1f], smoothedMag=[%.1f-%.1f], bins: [%.1f, %.1f, %.1f, %.1f, %.1f]",
             drawCount, mode, minMag, maxMag, smoothedMinMag, smoothedMaxMag,
             20*log10(fabs(sqrt(real[0]*real[0]+imag[0]*imag[0]))+1e-6)+120,
             20*log10(fabs(sqrt(real[1]*real[1]+imag[1]*imag[1]))+1e-6)+120,
             20*log10(fabs(sqrt(real[256]*real[256]+imag[256]*imag[256]))+1e-6)+120,
             20*log10(fabs(sqrt(real[512]*real[512]+imag[512]*imag[512]))+1e-6)+120,
             20*log10(fabs(sqrt(real[768]*real[768]+imag[768]*imag[768]))+1e-6)+120);
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (window) {
        ANativeWindow_Buffer buffer_out;
        if (ANativeWindow_lock(window, &buffer_out, nullptr) == 0) {
            if (buffer_out.width >= surfaceWidth && buffer_out.height >= surfaceHeight) {
                auto* dest = static_cast<uint32_t*>(buffer_out.bits);
                
                if (showSpectrumEnabled) {
                    if (spectrumDecayBuffer.size() != (size_t)surfaceWidth) {
                        spectrumDecayBuffer.assign(surfaceWidth, -1000.0);
                    }
                    // Clear background to black (Optimized 32-bit SIMD fill)
                    if (buffer_out.stride == surfaceWidth) {
                        // Contiguous memory: blast it all at once
                        std::fill_n(dest, surfaceWidth * surfaceHeight, 0xFF000000);
                    } else {
                        // Memory has padding: fill row by row using optimized blocks
                        for (int y = 0; y < surfaceHeight; y++) {
                            std::fill_n(&dest[y * buffer_out.stride], surfaceWidth, 0xFF000000);
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
                        uint32_t traceColor;
                        if (colorScale == 1) { // Light Blue -> Solid Light Blue trace
                            traceColor = 0xFFFFBF00; 
                        } else if (colorScale == 2) { // Grayscale -> Solid White trace
                            traceColor = 0xFFFFFFFF; // White
                        } else {
                            traceColor = getColor(colorNormalized * 255.0);
                        }
                        
                        int yPos = surfaceHeight - 1 - (int)(normalized * plotHeight);
                        yPos = yPos < TOP_MARGIN ? TOP_MARGIN : (yPos >= surfaceHeight ? surfaceHeight - 1 : yPos);
                        
                        if (prevY != -1) {
                            // Draw continuous line between prevY and yPos
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
                } else {
                    // Draw waterfall data
                    for (int y = 0; y < surfaceHeight; y++) {
                        memcpy(&dest[y * buffer_out.stride], &waterfallBuffer[y * surfaceWidth], surfaceWidth * sizeof(uint32_t));
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
        ANativeWindow_release(window);
    }
}

// ============================================================================
// Oboe JNI Methods
// ============================================================================

// --- Audio Capture Helpers ---

struct AudioThreadEnv {
    JNIEnv* env;
    jmethodID processAndDrawMethod;
    int16_t* buffer;
    bool attached;
};

static bool setupAudioEnvironment(AudioThreadEnv& audio) {
    audio.env = nullptr;
    audio.attached = false;
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
    audio.buffer = new int16_t[fftSize * 2];

    jclass mainActivityClass = audio.env->GetObjectClass(g_mainActivityRef);
    if (mainActivityClass == nullptr) {
        LOGI("ERROR: Failed to get mainActivityClass");
        delete[] audio.buffer;
        if (audio.attached) g_vm->DetachCurrentThread();
        return false;
    }

    audio.processAndDrawMethod = audio.env->GetMethodID(
        mainActivityClass, "processAndDraw", "([SILandroid/view/Surface;)V");

    if (audio.processAndDrawMethod == nullptr) {
        LOGI("ERROR: Failed to find processAndDraw method");
        audio.env->DeleteLocalRef(mainActivityClass);
        delete[] audio.buffer;
        if (audio.attached) g_vm->DetachCurrentThread();
        return false;
    }

    audio.env->DeleteLocalRef(mainActivityClass);
    return true;
}

// Audio capture thread function
void audioReadingThread() {
    if (g_audioCapture == nullptr || !g_isCapturing) return;

    AudioThreadEnv audio{};
    if (!setupAudioEnvironment(audio)) return;

    const int32_t fftSize = 2048;
    const int32_t framesPerRead = fftSize;

    while (g_isCapturing && g_audioCapture != nullptr) {
        int32_t framesRead = g_audioCapture->readFrames(audio.buffer, framesPerRead);
        if (framesRead < 0) {
            LOGI("AAudio read error");
            break;
        }
        if (framesRead == 0) {
            usleep(1000);
            continue;
        }

        jshortArray javaBuffer = audio.env->NewShortArray(framesRead * 2);
        if (javaBuffer == nullptr) {
            LOGI("ERROR: Failed to create short array for framesRead=%d", framesRead * 2);
            continue;
        }
        audio.env->SetShortArrayRegion(javaBuffer, 0, framesRead * 2, audio.buffer);

        jobject surface = g_surfaceRef;

        audio.env->CallVoidMethod(g_mainActivityRef, audio.processAndDrawMethod, javaBuffer, framesRead * 2, surface);

        if (audio.env->ExceptionCheck()) {
            LOGI("ERROR: Exception in CallVoidMethod");
            audio.env->ExceptionDescribe();
            audio.env->ExceptionClear();
        }

        audio.env->DeleteLocalRef(javaBuffer);
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

    LOGI("Oboe capture stopped");
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_belkarx_MainActivity_setOboeSurface(JNIEnv* env, jobject /* this */, jobject surface) {
    // Clean up old surface reference if it exists
    if (g_surfaceRef != nullptr) {
        env->DeleteGlobalRef(g_surfaceRef);
        LOGI("setOboeSurface: Deleted old surface reference");
    }
    
    // Store new surface reference
    if (surface != nullptr) {
        g_surfaceRef = env->NewGlobalRef(surface);
        LOGI("setOboeSurface: Updated to new surface reference %p", surface);
    } else {
        g_surfaceRef = nullptr;
        LOGI("setOboeSurface: Cleared surface reference");
    }
}
