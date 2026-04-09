#include <jni.h>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <atomic>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include "NativeShared.h"
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
std::vector<double> spectrumSmoothedBuffer;
std::vector<double> spectrumDecayBuffer;
int sensitivityValue = 100;
int contrastValue = 100;
int currentSampleRate = 96000;
bool swapIQEnabled = false;
bool zoomEnabled = false;
bool zoomCenteredFromTouch = false;
double zoomCenterFrequencyHz = 0.0;
bool showSpectrumEnabled = false;
bool isSpectrumFilled = false;  // true: filled bars, false: lines (default)
bool isSpectrumConstantColor = false;  // true: use constant middle color, false: use dynamic color (default)
int colorScale = 0;  // 0: Rainbow, 1: Light Blue, 2: Grayscale, 3: Cool-Hot, 4: Green Phosphor, 5: LCD

bool fixedWindowEnabled = true;  // fixed dB window is always enabled

// Adaptive EMA magnitude tracking (used when fixedWindowEnabled == false)
double smoothedMinMag = 110.0;
double smoothedMaxMag = 170.0;

ANativeWindow* g_nativeWindow = nullptr;

namespace {

struct RenderConfigSnapshot {
    int sensitivity;
    int contrast;
    int sampleRate;
    int colorScaleValue;
    bool swapIq;
    bool zoom;
    bool zoomHasCustomCenter;
    double zoomCenterHz;
    bool fixedWindow;
    bool showSpectrum;
    bool spectrumFilled;
    bool spectrumConstantColor;
};

int getWaterfallRows(int height) {
    return height > kTopMargin ? (height - kTopMargin) : 0;
}

int getShiftRows() {
    return 2;
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

void updateSmoothedMagnitudeRange(double minMag, double maxMag) {
    constexpr double alpha = 0.4;
    smoothedMinMag = smoothedMinMag * (1.0 - alpha) + minMag * alpha;
    smoothedMaxMag = smoothedMaxMag * (1.0 - alpha) + maxMag * alpha;
}

void computeRobustMagnitudeRange_UNUSED(const std::vector<double>& lineMags, int robustStrength, double& outMinMag, double& outMaxMag) {
    if (robustStrength <= 0) {
        outMinMag = 1e9;
        outMaxMag = -1e9;
        for (double value : lineMags) {
            if (value < outMinMag) outMinMag = value;
            if (value > outMaxMag) outMaxMag = value;
        }
        return;
    }

    double strengthNorm = static_cast<double>(robustStrength) / 100.0;
    double lowPercentile = 0.00 + strengthNorm * 0.02;    // 0.0% -> 2.0%
    double highPercentile = 1.00 - strengthNorm * 0.02;   // 100.0% -> 98.0%

    size_t sampleCount = lineMags.size();
    if (sampleCount < 8) {
        outMinMag = 1e9;
        outMaxMag = -1e9;
        for (double value : lineMags) {
            if (value < outMinMag) outMinMag = value;
            if (value > outMaxMag) outMaxMag = value;
        }
        return;
    }

    static thread_local std::vector<double> sortedScratch;
    sortedScratch = lineMags;

    size_t lowIndex = static_cast<size_t>(lowPercentile * (sampleCount - 1));
    size_t highIndex = static_cast<size_t>(highPercentile * (sampleCount - 1));
    if (highIndex <= lowIndex) {
        highIndex = lowIndex + 1;
    }
    if (highIndex >= sampleCount) {
        highIndex = sampleCount - 1;
    }

    std::nth_element(sortedScratch.begin(), sortedScratch.begin() + lowIndex, sortedScratch.end());
    outMinMag = sortedScratch[lowIndex];

    std::nth_element(sortedScratch.begin(), sortedScratch.begin() + highIndex, sortedScratch.end());
    outMaxMag = sortedScratch[highIndex];

    if (outMaxMag <= outMinMag + 1e-6) {
        outMinMag = sortedScratch.front();
        outMaxMag = sortedScratch.front() + 1.0;
        for (double value : sortedScratch) {
            if (value < outMinMag) outMinMag = value;
            if (value > outMaxMag) outMaxMag = value;
        }
    }
}

RenderConfigSnapshot snapshotRenderConfigLocked() {
    return {
        sensitivityValue,
        contrastValue,
        currentSampleRate,
        colorScale,
        swapIQEnabled,
        zoomEnabled,
        zoomCenteredFromTouch,
        zoomCenterFrequencyHz,
        fixedWindowEnabled,
        showSpectrumEnabled,
        isSpectrumFilled,
        isSpectrumConstantColor};
}

int computeMarkerX(int width, bool zoom) {
    if (zoom) {
        return width / 2;
    }
    return static_cast<int>(std::lround((width * 7) / 11.06));
}

void setNativeSurfaceLocked(JNIEnv* env, jobject surface) {
    if (g_nativeWindow != nullptr) {
        ANativeWindow_release(g_nativeWindow);
        g_nativeWindow = nullptr;
    }

    if (surface != nullptr) {
        g_nativeWindow = ANativeWindow_fromSurface(env, surface);
    }
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
    zoomCenteredFromTouch = false;
    LOGI("setZoom: %s", zoomEnabled ? "true (centered @+6.2kHz)" : "false (±24kHz @DC)");
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setZoomFromTouch(JNIEnv* env, jobject /* this */, jboolean enabled, jfloat touchX) {
    std::lock_guard<std::mutex> lock(g_mutex);

    zoomEnabled = (enabled == JNI_TRUE);
    if (!zoomEnabled) {
        zoomCenteredFromTouch = false;
        LOGI("setZoomFromTouch: false");
        return;
    }

    if (surfaceWidth <= 1 || currentSampleRate <= 0) {
        zoomCenteredFromTouch = false;
        LOGI("setZoomFromTouch: true but missing surface/sample-rate, fallback to default center");
        return;
    }

    float clampedX = std::max(0.0f, std::min(touchX, static_cast<float>(surfaceWidth - 1)));
    double normalized = static_cast<double>(clampedX) / static_cast<double>(surfaceWidth - 1);

    constexpr int kFftSize = 4096;
    int fullVisibleBins = kFftSize / 4;
    int fullStartBin = -(fullVisibleBins / 2);
    double tappedBin = static_cast<double>(fullStartBin) + (normalized * static_cast<double>(fullVisibleBins));
    double fullEndBin = static_cast<double>(fullStartBin + fullVisibleBins - 1);
    if (tappedBin < static_cast<double>(fullStartBin)) tappedBin = static_cast<double>(fullStartBin);
    if (tappedBin > fullEndBin) tappedBin = fullEndBin;
    double binHz = static_cast<double>(currentSampleRate) / static_cast<double>(kFftSize);

    zoomCenterFrequencyHz = tappedBin * binHz;
    zoomCenteredFromTouch = true;

    LOGI("setZoomFromTouch: true x=%.1f normalized=%.4f centerHz=%.1f", touchX, normalized, zoomCenterFrequencyHz);
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setFixedWindowEnabled(JNIEnv*, jobject, jboolean enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    fixedWindowEnabled = true;
    LOGI("setFixedWindowEnabled: true (hardcoded)");
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

static void drawFrequencyGridLines(uint32_t* dest, int stride, int width, int height, int markerX, int markerY, double viewStartHz, double viewSpanHz, double referenceFrequencyHz) {
    const uint32_t YELLOW = 0xFF00FFFF;
    const uint8_t font[12][5] = {
        {0x7, 0x5, 0x5, 0x5, 0x7}, {0x2, 0x6, 0x2, 0x2, 0x7}, {0x7, 0x1, 0x7, 0x4, 0x7},
        {0x7, 0x1, 0x7, 0x1, 0x7}, {0x5, 0x5, 0x7, 0x1, 0x1}, {0x7, 0x4, 0x7, 0x1, 0x7},
        {0x7, 0x4, 0x7, 0x5, 0x7}, {0x7, 0x1, 0x2, 0x2, 0x2}, {0x7, 0x5, 0x7, 0x5, 0x7},
        {0x7, 0x5, 0x7, 0x1, 0x7}, {0x0, 0x2, 0x7, 0x2, 0x0}, {0x0, 0x0, 0x7, 0x0, 0x0}
    };

    auto drawNum = [&](int x, int y, int num) {
        int scale = 4, cx = x;
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

    if (viewSpanHz <= 1e-9) {
        return;
    }

    constexpr double kTickStepHz = 5000.0;
    double viewEndHz = viewStartHz + viewSpanHz;
    double firstRelativeHz = std::ceil((viewStartHz - referenceFrequencyHz) / kTickStepHz) * kTickStepHz;
    double lastRelativeHz = std::floor((viewEndHz - referenceFrequencyHz) / kTickStepHz) * kTickStepHz;

    for (double relativeHz = firstRelativeHz; relativeHz <= lastRelativeHz + 0.5; relativeHz += kTickStepHz) {
        double tickFrequencyHz = referenceFrequencyHz + relativeHz;
        int pixelX = static_cast<int>(std::lround(((tickFrequencyHz - viewStartHz) / viewSpanHz) * static_cast<double>(width)));
        if (pixelX < 0 || pixelX >= width) continue;

        int relativeKHz = static_cast<int>(std::lround(relativeHz / 1000.0));
        bool majorTick = (relativeKHz % 10) == 0;
        int lineHeight = majorTick ? 20 : 10;

        if (relativeKHz != 0) {
            for (int y = markerY; y < markerY + lineHeight && y < height; y++) {
                if (y >= 0) dest[y * stride + pixelX] = YELLOW;
            }
        }

        if (majorTick && relativeKHz != 0) {
            drawNum(pixelX, markerY + lineHeight + 2, relativeKHz);
        }
    }
}

void drawArrowMarkerOnWindow(uint32_t* dest, int stride, int width, int height, int markerPixelX, int markerPixelY, double viewStartHz, double viewSpanHz, double referenceFrequencyHz) {
    if (markerPixelX < 0 || markerPixelX >= width) return;
    drawArrowHead(dest, stride, width, height, markerPixelX, markerPixelY);
    drawFrequencyGridLines(dest, stride, width, height, markerPixelX, markerPixelY, viewStartHz, viewSpanHz, referenceFrequencyHz);
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

    int shiftRows = getShiftRows();
    if (!config.showSpectrum && waterfallRows > 0) {
        auto t0 = Clock::now();
        advanceWaterfallHeadLocked(waterfallRows, shiftRows);
        waterfallUpdateNs += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
    }

    SpectrumSpan span = computeSpectrumSpan(fftSize, config.sampleRate, config.zoom, zoomCenteredFromTouch, zoomCenterFrequencyHz);

    double minMag = 1e9, maxMag = -1e9;
    thread_local std::vector<double> lineMags;
    if ((int)lineMags.size() != surfaceWidth) lineMags.resize(surfaceWidth);
    auto magTs = Clock::now();
    computeLineMagnitudes(real, imag, fftSize, surfaceWidth, config.zoom, span, lineMags, minMag, maxMag);
    magPassNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - magTs).count();

    if (!config.showSpectrum && waterfallRows > 0) {
        auto t0 = Clock::now();
        thread_local std::array<uint32_t, 256> colorLut;
        thread_local int lutSens = -1, lutContr = -1, lutSc = -1;
        if (lutSens != config.sensitivity || lutContr != config.contrast || lutSc != config.colorScaleValue) {
            buildColorLut(config.sensitivity, config.contrast, config.colorScaleValue, colorLut);
            lutSens = config.sensitivity; lutContr = config.contrast; lutSc = config.colorScaleValue;
        }
        // Fixed dB window: sensitivity controls level placement, contrast only shapes color response.
        double wMin, wMax, invRange;
        if (config.fixedWindow) {
            double fwMax = 130.0 - (config.sensitivity - 100) * 0.8;
            constexpr double fwRange = 70.0;
            wMin = fwMax - fwRange;
            wMax = fwMax;
            invRange = 255.0 / fwRange;
        } else {
            // Adaptive EMA autoscale
            double displayMin = minMag, displayMax = maxMag;
            updateSmoothedMagnitudeRange(displayMin, displayMax);
            wMin = smoothedMinMag;
            wMax = smoothedMaxMag;
            invRange = 255.0 / (smoothedMaxMag - smoothedMinMag + 1e-6);
        }
        uint32_t* row = &waterfallBuffer[waterfallHeadRow * surfaceWidth];
        double contrastExponent = 0.5 + (config.contrast / 300.0) * 4.0;
        for (int x = 0; x < surfaceWidth; x++) {
            double ni = (lineMags[x] - wMin) * invRange;
            ni = ni < 0.0 ? 0.0 : (ni > 255.0 ? 255.0 : ni);
            double shaped = std::pow(ni / 255.0, contrastExponent) * 255.0;
            int lutIdx = static_cast<int>(shaped);
            if (lutIdx < 0) lutIdx = 0;
            if (lutIdx > 255) lutIdx = 255;
            row[x] = colorLut[lutIdx];
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

                static int64_t lastSpectrumRenderNs = 0;
                int64_t nowSpectrumRenderNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
                double frameDeltaSec = 1.0 / 60.0;
                if (lastSpectrumRenderNs > 0) {
                    frameDeltaSec = static_cast<double>(nowSpectrumRenderNs - lastSpectrumRenderNs) / 1e9;
                }
                lastSpectrumRenderNs = nowSpectrumRenderNs;

                SpectrumRenderParams spectrumParams{
                    sw,
                    sh,
                    TOP_MARGIN,
                    config.sensitivity,
                    config.contrast,
                    config.colorScaleValue,
                    config.spectrumFilled,
                    config.spectrumConstantColor,
                    0.60f,
                    45.0,
                    frameDeltaSec};
                drawSpectrumFrame(dest, buf.stride, lineMagCopy, spectrumSmoothedBuffer, spectrumDecayBuffer, spectrumParams);
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

            constexpr int kFftSize = 4096;
            SpectrumSpan scaleSpan = computeSpectrumSpan(
                kFftSize,
                config.sampleRate,
                config.zoom,
                config.zoomHasCustomCenter,
                config.zoomCenterHz);
            SpectrumSpan referenceSpan = computeSpectrumSpan(
                kFftSize,
                config.sampleRate,
                false,
                false,
                0.0);
            double binHz = static_cast<double>(config.sampleRate) / static_cast<double>(kFftSize);

            int referenceMarkerX = computeMarkerX(sw, false);
            double referenceFrequencyHz =
                (static_cast<double>(referenceSpan.startBin) +
                 (static_cast<double>(referenceMarkerX) * static_cast<double>(referenceSpan.visibleBins) / static_cast<double>(sw))) *
                binHz;
            double viewStartHz = static_cast<double>(scaleSpan.startBin) * binHz;
            double viewSpanHz = static_cast<double>(scaleSpan.visibleBins) * binHz;
            int mx = static_cast<int>(std::lround(((referenceFrequencyHz - viewStartHz) / viewSpanHz) * static_cast<double>(sw)));
            if (mx < 0) mx = 0;
            if (mx >= sw) mx = sw - 1;

            drawArrowMarkerOnWindow(
                dest,
                buf.stride,
                sw,
                sh,
                mx,
                0,
                viewStartHz,
                viewSpanHz,
                referenceFrequencyHz);
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

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setNativeSurface(JNIEnv* env, jobject /* this */, jobject surface) {
    std::lock_guard<std::mutex> lock(g_mutex);
    setNativeSurfaceLocked(env, surface);
    LOGI("setNativeSurface: %s", surface != nullptr ? "updated" : "cleared");
}
