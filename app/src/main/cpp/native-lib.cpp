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
int sensitivityValue = 100;
int contrastValue = 100;
int currentSampleRate = 96000;
bool swapIQEnabled = false;
bool zoomEnabled = false;
bool fastWaterfallEnabled = false;
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

void drawArrowMarkerOnWindow(uint32_t* dest, int stride, int markerPixelX, int markerPixelY) {
    // Draw a yellow downward pointing arrow marker above the spectrum
    if (markerPixelX < 0 || markerPixelX >= surfaceWidth) return;
    
    const uint32_t YELLOW = 0xFF00FFFF;  // ARGB format: yellow
    const int ARROW_SIZE = 15;
    
    // Simple 3x5 font for numbers 0-9, and '+', '-'
    const uint8_t font[12][5] = {
        {0x7, 0x5, 0x5, 0x5, 0x7}, // 0
        {0x2, 0x6, 0x2, 0x2, 0x7}, // 1
        {0x7, 0x1, 0x7, 0x4, 0x7}, // 2
        {0x7, 0x1, 0x7, 0x1, 0x7}, // 3
        {0x5, 0x5, 0x7, 0x1, 0x1}, // 4
        {0x7, 0x4, 0x7, 0x1, 0x7}, // 5
        {0x7, 0x4, 0x7, 0x5, 0x7}, // 6
        {0x7, 0x1, 0x2, 0x2, 0x2}, // 7
        {0x7, 0x5, 0x7, 0x5, 0x7}, // 8
        {0x7, 0x5, 0x7, 0x1, 0x7}, // 9
        {0x0, 0x2, 0x7, 0x2, 0x0}, // 10: +
        {0x0, 0x0, 0x7, 0x0, 0x0}  // 11: -
    };

    auto drawNum = [&](int x, int y, int num) {
        int scale = 3; // scale font by 3x for readability
        int cx = x;
        auto drawChar = [&](int charIdx) {
            for (int r = 0; r < 5; ++r) {
                for (int c = 0; c < 3; ++c) {
                    if ((font[charIdx][r] >> (2 - c)) & 1) {
                        for (int sy = 0; sy < scale; ++sy) {
                            for (int sx = 0; sx < scale; ++sx) {
                                int px = cx + c * scale + sx;
                                int py = y + r * scale + sy;
                                if (px >= 0 && px < surfaceWidth && py >= 0 && py < surfaceHeight) {
                                    dest[py * stride + px] = YELLOW;
                                }
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
        
        cx -= ((numChars * 4 - 1) * scale) / 2; // Center text at x

        if (num > 0) drawChar(10);
        else if (num < 0) { drawChar(11); num = -num; }
        
        if (num >= 100) {
            drawChar(num / 100);
            drawChar((num / 10) % 10);
            drawChar(num % 10);
        } else if (num >= 10) {
            drawChar(num / 10);
            drawChar(num % 10);
        } else {
            drawChar(num % 10);
        }
    };

    // Draw downward pointing arrow head (triangle with tip at bottom)
    for (int i = 0; i <= ARROW_SIZE; i++) {
        int y = markerPixelY + i;
        if (y < surfaceHeight) {
            int width = ARROW_SIZE - i;  // Wider at top, narrower at bottom (tip)
            for (int x = markerPixelX - width; x <= markerPixelX + width; x++) {
                if (x >= 0 && x < surfaceWidth) {
                    dest[y * stride + x] = YELLOW;
                }
            }
        }
    }
    
    // Draw frequency grid lines relative to the marker position (reference point)
    // Grid lines at 5 kHz intervals, longer at 10 kHz multiples
    const int FFT_SIZE = 2048;
    const int BASE_BINS = FFT_SIZE / 4;  // 512 bins
    const float HZ_PER_BIN = (float)currentSampleRate / FFT_SIZE;  // Hz per FFT bin (~46.875 Hz at 96kHz)
    
    int visibleBins, startBin;
    float Hz_per_pixel;
    
    if (zoomEnabled) {
        // Zoom mode: ±6 kHz centered at +8 kHz (+2 to +14 kHz displayed)
        visibleBins = BASE_BINS / 2;  // 256 bins = 12 kHz bandwidth
        int binFreqResolution = currentSampleRate / FFT_SIZE;
        int targetBin = (8000 / binFreqResolution);  // +8 kHz bin position
        startBin = targetBin - (visibleBins / 2);  // Center at +8 kHz
    } else {
        // Normal mode: ±12 kHz centered at DC (-12 to +12 kHz displayed)
        visibleBins = BASE_BINS;  // 512 bins = 24 kHz bandwidth
        startBin = -(visibleBins / 2);  // -12 kHz at the left edge
    }
    
    // Calculate Hz per pixel for scaling
    Hz_per_pixel = static_cast<float>(visibleBins) * HZ_PER_BIN / static_cast<float>(surfaceWidth);
    
    // Draw grid lines at 5 kHz intervals relative to marker position
    for (int offsetKHz = -50; offsetKHz <= 50; offsetKHz += 5) {
        // Calculate pixel offset from marker position
        float pixelOffset = (static_cast<float>(offsetKHz) * 1000.0f) / Hz_per_pixel;
        int pixelX = markerPixelX + static_cast<int>(std::lround(pixelOffset));
        
        // Check if within display range
        if (pixelX < 0 || pixelX >= surfaceWidth) continue;
        
        // Skip marker position itself (offsetKHz == 0)
        if (offsetKHz == 0) continue;
        
        // Determine line height: longer for 10 kHz multiples, shorter for 5 kHz
        int lineHeight;
        if (offsetKHz % 10 == 0) {
            lineHeight = 20;  // Longer line for 10 kHz multiples
        } else {
            lineHeight = 10;  // Short line for 5 kHz intervals
        }
        
        // Draw vertical grid line
        for (int y = markerPixelY; y < markerPixelY + lineHeight && y < surfaceHeight; y++) {
            if (y >= 0) {
                dest[y * stride + pixelX] = YELLOW;
            }
        }
        
        // Draw text for 10 kHz multiples
        if (offsetKHz % 10 == 0) {
            drawNum(pixelX, markerPixelY + lineHeight + 2, offsetKHz);
        }
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
        // Classic SDR Waterfall Rainbow: Deep Blue -> Light Blue -> Green -> Yellow -> Red
        double norm = intensityInt / 255.0;
        
        // 4 color segments:
        // 0.00-0.25: Black to Deep Blue
        // 0.25-0.50: Deep Blue to Cyan/Green
        // 0.50-0.75: Green to Yellow
        // 0.75-1.00: Yellow to Red
        
        if (norm < 0.25) {
            double t = norm * 4.0;
            r = 0;
            g = 0;
            b = (uint8_t)(t * 255);
        } else if (norm < 0.5) {
            double t = (norm - 0.25) * 4.0;
            r = 0;
            g = (uint8_t)(t * 255);
            b = (uint8_t)((1.0 - t) * 255 + t * 150); // don't zero blue entirely too fast
        } else if (norm < 0.75) {
            double t = (norm - 0.5) * 4.0;
            r = (uint8_t)(t * 255);
            g = 255;
            b = (uint8_t)((1.0 - t) * 150); // fade out blue
        } else {
            double t = (norm - 0.75) * 4.0;
            r = 255;
            g = (uint8_t)((1.0 - t) * 255); // fade green to get pure red
            b = 0;
        }
    } else if (colorScale == 1) {
        // Light Blue Waterfall: Black -> Dark Blue -> Standard Blue -> Cyan -> White
        double norm = intensityInt / 255.0;
        if (norm < 0.33) {
            // Black to Dark Blue
            double t = norm * 3.0; // 0 to 1
            r = 0;
            g = 0;
            b = (uint8_t)(t * 150);
        } else if (norm < 0.66) {
            // Dark Blue to Cyan/Light Blue
            double t = (norm - 0.33) * 3.0; // 0 to 1
            r = 0;
            g = (uint8_t)(t * 206);
            b = (uint8_t)(150 + t * 105); // 150 -> 255
        } else {
            // Cyan/Light Blue to White (peak indicator)
            double t = (norm - 0.66) * 3.0; // 0 to 1
            r = (uint8_t)(t * 255);
            g = (uint8_t)(206 + t * 49);  // 206 -> 255
            b = 255;
        }
    } else if (colorScale == 2) {
        // Grayscale: black -> white
        auto gray = static_cast<uint8_t>(intensityInt);
        r = gray;
        g = gray;
        b = gray;
    } else {  // colorScale == 3
        // Cool-Hot (SDR Plasma/Inferno style): Black -> Dark Purple -> Red -> Orange -> Yellow -> White
        double norm = intensityInt / 255.0;
        
        if (norm < 0.25) {
            // Black to Dark Purple
            double t = norm * 4.0;
            r = (uint8_t)(t * 100);
            g = 0;
            b = (uint8_t)(t * 150);
        } else if (norm < 0.5) {
            // Dark Purple to Red
            double t = (norm - 0.25) * 4.0;
            r = (uint8_t)(100 + t * 155);
            g = 0;
            b = (uint8_t)((1.0 - t) * 150);
        } else if (norm < 0.75) {
            // Red to Orange/Yellow
            double t = (norm - 0.5) * 4.0;
            r = 255;
            g = (uint8_t)(t * 200);
            b = 0;
        } else {
            // Yellow to White (highest peaks)
            double t = (norm - 0.75) * 4.0;
            r = 255;
            g = (uint8_t)(200 + t * 55);
            b = (uint8_t)(t * 255);
        }
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

    std::vector<double> real(fftSize);
    std::vector<double> imag(fftSize);

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
    std::vector<double> lineMagnitudes(surfaceWidth);
    for (int x = 0; x < surfaceWidth; x++) {
        double mag;
        
        if (zoomEnabled) {
            // ZOOM MODE: use linear interpolation between bins for smoother resolution
            double binPosReal = (x * (double)visibleBins) / surfaceWidth;
            int bin1 = (int)binPosReal;
            int bin2 = bin1 + 1;
            double frac = binPosReal - bin1;  // 0.0 to 1.0 interpolation factor
            
            // Get bin indices with wrapping
            int shiftedBin1 = startBin + bin1;
            int shiftedBin2 = startBin + bin2;
            
            // Wrap negative bins
            if (shiftedBin1 < 0) shiftedBin1 += fftSize;
            if (shiftedBin1 >= fftSize) shiftedBin1 -= fftSize;
            if (shiftedBin2 < 0) shiftedBin2 += fftSize;
            if (shiftedBin2 >= fftSize) shiftedBin2 -= fftSize;
            
            // Calculate magnitudes for both bins
            double mag1 = sqrt(real[shiftedBin1] * real[shiftedBin1] + imag[shiftedBin1] * imag[shiftedBin1]);
            double mag2 = sqrt(real[shiftedBin2] * real[shiftedBin2] + imag[shiftedBin2] * imag[shiftedBin2]);
            
            // Linear interpolation
            mag = mag1 * (1.0 - frac) + mag2 * frac;
        } else {
            // NORMAL MODE: direct bin lookup (as before)
            int binIdxInSpan = (x * visibleBins) / surfaceWidth;
            int binIdx = startBin + binIdxInSpan;
            
            // Wrap bin index to handle negative frequencies
            int shiftedBin = binIdx;
            if (shiftedBin < 0) shiftedBin += fftSize;
            if (shiftedBin >= fftSize) shiftedBin -= fftSize;

            // I/Q magnitude: sqrt(I^2 + Q^2)
            mag = sqrt(real[shiftedBin] * real[shiftedBin] + imag[shiftedBin] * imag[shiftedBin]);
        }
        
        double logMag = 20 * log10(mag + 1e-6) + 120;
        
        lineMagnitudes[x] = logMag;
        if (logMag < minMag) minMag = logMag;
        if (logMag > maxMag) maxMag = logMag;
    }
    
    // Smooth the min/max with exponential filter (time constant ~0.5 seconds at 24fps)
    double alpha = 0.4;  // Smoothing factor: lower = slower changes
    smoothedMinMag = smoothedMinMag * (1.0 - alpha) + minMag * alpha;
    smoothedMaxMag = smoothedMaxMag * (1.0 - alpha) + maxMag * alpha;
    
    // Second pass: draw with normalized intensity using smoothed min/max
    for (int x = 0; x < surfaceWidth; x++) {
        double logMag = lineMagnitudes[x];
        
        // Normalize intensity to 0-255 based on smoothed min/max
        double range = smoothedMaxMag - smoothedMinMag + 1e-6;  // Avoid division by zero
        double normalizedIntensity = (logMag - smoothedMinMag) / range * 255.0;
        normalizedIntensity = fmax(0.0, fmin(255.0, normalizedIntensity));
        
        uint32_t color = getColor(normalizedIntensity);
        for (int r = 0; r < shiftRows; r++) {
            waterfallBuffer[(TOP_MARGIN + r) * surfaceWidth + x] = color;
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
                
                // Draw spectrum data
                for (int y = 0; y < surfaceHeight; y++) {
                    memcpy(&dest[y * buffer_out.stride], &waterfallBuffer[y * surfaceWidth], surfaceWidth * sizeof(uint32_t));
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

// Audio capture thread function
void audioReadingThread() {
    if (g_audioCapture == nullptr || !g_isCapturing) return;

    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_vm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }

    if (env == nullptr) {
        LOGI("ERROR: Failed to get JNI environment in audioReadingThread");
        return;
    }

    const int32_t fftSize = 2048;  // Match processAndDraw FFT size
    const int32_t framesPerRead = fftSize;
    auto* buffer = new int16_t[framesPerRead * 2]; // stereo

    jclass mainActivityClass = env->GetObjectClass(g_mainActivityRef);
    if (mainActivityClass == nullptr) {
        LOGI("ERROR: Failed to get mainActivityClass");
        delete[] buffer;
        if (attached) g_vm->DetachCurrentThread();
        return;
    }

    jmethodID processAndDrawMethod = env->GetMethodID(
        mainActivityClass, "processAndDraw", "([SILandroid/view/Surface;)V");

    if (processAndDrawMethod == nullptr) {
        LOGI("ERROR: Failed to find processAndDraw method");
        env->DeleteLocalRef(mainActivityClass);
        delete[] buffer;
        if (attached) g_vm->DetachCurrentThread();
        return;
    }

    LOGI("audioReadingThread: Started with mainActivityRef=%p, processAndDrawMethod=%p", g_mainActivityRef, processAndDrawMethod);

    int readCount = 0;
    int monoDetectCount = 0;
    int stereoDetectCount = 0;

    while (g_isCapturing && g_audioCapture != nullptr) {
        int32_t framesRead = g_audioCapture->readFrames(buffer, framesPerRead);
        if (framesRead < 0) {
            LOGI("AAudio read error");
            break;
        }
        if (framesRead == 0) {
            // No data available, sleep briefly
            usleep(1000);
            continue;
        }

        // Check if data is truly stereo or mono
        if (readCount % 10 == 0) {  // Log every 10 reads
            int64_t diffSum = 0;
            int16_t minVal = 32767, maxVal = -32768;
            int16_t left0 = 0, right0 = 0, left1 = 0, right1 = 0;
            
            for (int i = 0; i < framesRead && i < 5; i++) {
                int16_t left = buffer[2*i];
                int16_t right = buffer[2*i + 1];
                if (i == 0) { left0 = left; right0 = right; }
                if (i == 1) { left1 = left; right1 = right; }
                diffSum += abs(left - right);
                minVal = (left < minVal) ? left : minVal;
                maxVal = (left > maxVal) ? left : maxVal;
            }
            
            if (diffSum < 100) {
                monoDetectCount++;
                LOGI("Read %d: MONO (diffSum=%lld) L/R[0]=(%d,%d) L/R[1]=(%d,%d) range=[%d,%d]", 
                     readCount, (long long)diffSum, left0, right0, left1, right1, minVal, maxVal);
            } else {
                stereoDetectCount++;
                LOGI("Read %d: STEREO (diffSum=%lld) L/R[0]=(%d,%d) L/R[1]=(%d,%d)", 
                     readCount, (long long)diffSum, left0, right0, left1, right1);
            }
        }
        readCount++;

        // Create Java short array from buffer
        jshortArray javaBuffer = env->NewShortArray(framesRead * 2);
        if (javaBuffer == nullptr) {
            LOGI("ERROR: Failed to create short array for framesRead=%d", framesRead * 2);
            continue;
        }
        env->SetShortArrayRegion(javaBuffer, 0, framesRead * 2, buffer);

        // Call processAndDraw callback with the Surface reference (if available)
        jobject surface = g_surfaceRef;  // Use the stored surface reference
        if (readCount % 100 == 0) {
            LOGI("Calling processAndDraw: framesRead=%d, surface=%p, mainActivityRef=%p", framesRead * 2, surface, g_mainActivityRef);
        }
        
        env->CallVoidMethod(g_mainActivityRef, processAndDrawMethod, javaBuffer, framesRead * 2, surface);
        
        // Check for JNI exceptions
        if (env->ExceptionCheck()) {
            LOGI("ERROR: Exception in CallVoidMethod");
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        
        env->DeleteLocalRef(javaBuffer);
    }

    delete[] buffer;
    if (attached) g_vm->DetachCurrentThread();
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
