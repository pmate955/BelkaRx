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
#include <memory>
#include <cstring>
#include <climits>
#include <unistd.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include "NativeShared.h"
#include "ColorScales.h"
#include "DspHelpers.h"
#include "RenderHelpers.h"
#include "GlWaterfall.h"
#include "CwDecoder.h"

#define LOG_TAG "BelkaRx-Native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

std::mutex g_mutex;
int surfaceWidth = 0;
int surfaceHeight = 0;
constexpr int kTopMargin = 15;
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
bool adjustableMarkerEnabled = false;
bool cwDecoderEnabled = false;   // decode Morse at the adjustable marker frequency
int overlayBottomInset = 0;      // pixels at the bottom covered by the CW text strip
double adjustableMarkerFrequencyHz = 0.0;
int demodMode = 0;  // 0=AM, 1=USB, 2=LSB
bool showSpectrumEnabled = false;
bool isSpectrumFilled = false;  // true: filled bars, false: lines (default)
bool isSpectrumConstantColor = false;  // true: use constant middle color, false: use dynamic color (default)
int colorScale = 0;  // 0: Rainbow, 1: Light Blue, 2: Grayscale, 3: Cool-Hot, 4: Green Phosphor, 5: LCD

bool fixedWindowEnabled = true;  // fixed dB window is always enabled

// Adaptive EMA magnitude tracking (used when fixedWindowEnabled == false)
double smoothedMinMag = 110.0;
double smoothedMaxMag = 170.0;

ANativeWindow* g_nativeWindow = nullptr;
// Held by the render thread while it draws into g_nativeWindow, and by setNativeSurface while it
// swaps the window, so surfaceDestroyed cannot release the window in the middle of a frame.
static std::mutex g_windowMutex;
static int g_windowGeneration = 0;  // bumped on every window change; forces a full redraw

// One FFT line is produced per kHopSize stereo frames (4096-point FFT, 50% overlap).
// MainActivity uses the same hop. Only the initial line-rate estimate depends on it:
// the real rate is measured from the arrival times of the lines.
constexpr int kHopSize = 2048;

// Waterfall history. The audio thread writes one row per FFT line into slot
// (rowIndex % capacityRows) and then publishes rowsWritten; the render thread scrolls through
// the rows at a steady pace and copies them without taking g_mutex. A resize swaps in a new
// ring, so a reader always holds a shared_ptr to a ring whose size never changes.
struct WaterfallRing {
    int width = 0;
    int height = 0;        // surface height in pixels
    int visibleRows = 0;   // waterfall rows on screen (height - top margin)
    int capacityRows = 0;  // visibleRows + kRingExtraRows of look-ahead
    std::vector<uint32_t> pixels;
    std::atomic<int64_t> rowsWritten{0};
};
constexpr int kRingExtraRows = 256;
static std::mutex g_ringPtrMutex;  // guards only the g_ring pointer swap
static std::shared_ptr<WaterfallRing> g_ring;

static std::shared_ptr<WaterfallRing> loadRing() {
    std::lock_guard<std::mutex> lock(g_ringPtrMutex);
    return g_ring;
}

// CW decoder: fed on the audio thread, polled from the UI thread (takeText/status are thread-safe).
static CwDecoder g_cwDecoder;

// Smoothed power per FFT bin (natural FFT order), used to snap the marker onto a nearby signal.
static std::mutex g_snapMutex;
static std::vector<float> g_snapPower;

// Measured waterfall row rate (rows per second), written by the audio thread.
static std::atomic<double> g_rowsPerSecond{192000.0 / kHopSize};

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
    bool adjustableMarkerEnabled;
    double adjustableMarkerFrequencyHz;
    int demodMode;
    bool fixedWindow;
    bool showSpectrum;
    bool spectrumFilled;
    bool spectrumConstantColor;
    bool cwEnabled;
    int bottomInset;
};

int getWaterfallRows(int height) {
    return height > kTopMargin ? (height - kTopMargin) : 0;
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
        adjustableMarkerEnabled,
        adjustableMarkerFrequencyHz,
        demodMode,
        fixedWindowEnabled,
        showSpectrumEnabled,
        isSpectrumFilled,
        isSpectrumConstantColor,
        cwDecoderEnabled,
        overlayBottomInset};
}

bool sameRenderConfig(const RenderConfigSnapshot& a, const RenderConfigSnapshot& b) {
    return a.sensitivity == b.sensitivity && a.contrast == b.contrast && a.sampleRate == b.sampleRate &&
           a.colorScaleValue == b.colorScaleValue && a.swapIq == b.swapIq && a.zoom == b.zoom &&
           a.zoomHasCustomCenter == b.zoomHasCustomCenter && a.zoomCenterHz == b.zoomCenterHz &&
           a.adjustableMarkerEnabled == b.adjustableMarkerEnabled &&
           a.adjustableMarkerFrequencyHz == b.adjustableMarkerFrequencyHz && a.demodMode == b.demodMode &&
           a.fixedWindow == b.fixedWindow && a.showSpectrum == b.showSpectrum &&
           a.spectrumFilled == b.spectrumFilled && a.spectrumConstantColor == b.spectrumConstantColor;
}

int computeMarkerX(int width, bool zoom) {
    if (zoom) {
        return width / 2;
    }
    return static_cast<int>(std::lround((width * 7) / 11.06));
}

double getDemodReferenceOffsetHz(int mode) {
    switch (mode) {
        case 1:  // USB
            return -3000.0;
        case 2:  // LSB
        case 0:  // AM
        default:
            return 0.0;
    }
}

void getDemodReceiveBandOffsetsHz(int mode, double& outStartOffsetHz, double& outEndOffsetHz) {
    switch (mode) {
        case 1:  // USB
            outStartOffsetHz = 0.0;
            outEndOffsetHz = 3000.0;
            break;
        case 2:  // LSB
            outStartOffsetHz = -3000.0;
            outEndOffsetHz = 0.0;
            break;
        case 0:  // AM
        default:
            outStartOffsetHz = -4000.0;
            outEndOffsetHz = 4000.0;
            break;
    }
}

double computeReferenceFrequencyHz(int width, int sampleRate, int mode) {
    if (width <= 0 || sampleRate <= 0) {
        return 0.0;
    }

    constexpr int kFftSize = 4096;
    SpectrumSpan referenceSpan = computeSpectrumSpan(kFftSize, sampleRate, false, false, 0.0);
    double binHz = static_cast<double>(sampleRate) / static_cast<double>(kFftSize);
    int referenceMarkerX = computeMarkerX(width, false);
    double baseReferenceHz = (static_cast<double>(referenceSpan.startBin) +
                              (static_cast<double>(referenceMarkerX) * static_cast<double>(referenceSpan.visibleBins) / static_cast<double>(width))) *
                             binHz;
    return baseReferenceHz + getDemodReferenceOffsetHz(mode);
}

void computeCurrentViewWindowHz(double& outViewStartHz, double& outViewSpanHz) {
    if (currentSampleRate <= 0) {
        outViewStartHz = 0.0;
        outViewSpanHz = 0.0;
        return;
    }

    constexpr int kFftSize = 4096;
    SpectrumSpan scaleSpan = computeSpectrumSpan(
        kFftSize,
        currentSampleRate,
        zoomEnabled,
        zoomCenteredFromTouch,
        zoomCenterFrequencyHz,
        cwDecoderEnabled);
    double binHz = static_cast<double>(currentSampleRate) / static_cast<double>(kFftSize);
    outViewStartHz = static_cast<double>(scaleSpan.startBin) * binHz;
    outViewSpanHz = static_cast<double>(scaleSpan.visibleBins) * binHz;
}

void setNativeSurfaceLocked(JNIEnv* env, jobject surface) {
    ANativeWindow* next = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    if (next != nullptr && next == g_nativeWindow) {
        // Same window (surfaceChanged): keep the EGL surface, drop the extra reference.
        ANativeWindow_release(next);
        return;
    }
    if (g_nativeWindow != nullptr) {
        ANativeWindow_release(g_nativeWindow);
    }
    g_nativeWindow = next;
    g_windowGeneration++;
}

}  // namespace

// Radix-2 Cooley-Tukey FFT (in place, n must be a power of two).
// The bit-reversal swaps and the twiddle factors are computed once per size and reused, so a
// call is just the swaps and the butterflies. Exact table twiddles are also more accurate than
// the old per-butterfly rotation recurrence, which accumulated float error along each stage.
void fft(std::vector<float>& real, std::vector<float>& imag) {
    const size_t n = real.size();
    if (n <= 1) return;

    struct FftTables {
        size_t size = 0;
        std::vector<std::pair<uint32_t, uint32_t>> swaps;  // index pairs i < j to exchange
        std::vector<float> cosTable;                       // cos(-2*pi*k/n), k < n/2
        std::vector<float> sinTable;                       // sin(-2*pi*k/n), k < n/2
    };
    thread_local FftTables tables;
    if (tables.size != n) {
        tables.size = n;
        tables.swaps.clear();
        for (size_t i = 1, j = 0; i < n; i++) {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) tables.swaps.emplace_back(static_cast<uint32_t>(i), static_cast<uint32_t>(j));
        }
        tables.cosTable.resize(n / 2);
        tables.sinTable.resize(n / 2);
        for (size_t k = 0; k < n / 2; k++) {
            double angle = -2.0 * M_PI * static_cast<double>(k) / static_cast<double>(n);
            tables.cosTable[k] = static_cast<float>(std::cos(angle));
            tables.sinTable[k] = static_cast<float>(std::sin(angle));
        }
    }

    float* re = real.data();
    float* im = imag.data();
    const float* cosT = tables.cosTable.data();
    const float* sinT = tables.sinTable.data();

    for (const auto& s : tables.swaps) {
        std::swap(re[s.first], re[s.second]);
        std::swap(im[s.first], im[s.second]);
    }

    // First stage (len = 2): twiddle is 1, no multiplications needed.
    for (size_t i = 0; i < n; i += 2) {
        float aRe = re[i], aIm = im[i];
        float bRe = re[i + 1], bIm = im[i + 1];
        re[i] = aRe + bRe;     im[i] = aIm + bIm;
        re[i + 1] = aRe - bRe; im[i + 1] = aIm - bIm;
    }

    for (size_t len = 4; len <= n; len <<= 1) {
        const size_t half = len >> 1;
        const size_t step = n / len;  // twiddle index stride for this stage
        for (size_t i = 0; i < n; i += len) {
            float* aRe = re + i;
            float* aIm = im + i;
            float* bRe = re + i + half;
            float* bIm = im + i + half;
            for (size_t j = 0, k = 0; j < half; j++, k += step) {
                const float wRe = cosT[k];
                const float wIm = sinT[k];
                const float tRe = wRe * bRe[j] - wIm * bIm[j];
                const float tIm = wRe * bIm[j] + wIm * bRe[j];
                const float uRe = aRe[j];
                const float uIm = aIm[j];
                aRe[j] = uRe + tRe;
                aIm[j] = uIm + tIm;
                bRe[j] = uRe - tRe;
                bIm[j] = uIm - tIm;
            }
        }
    }
}



extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setSurfaceSize(JNIEnv* env, jobject /* this */, jint width, jint height) {
    auto ring = std::make_shared<WaterfallRing>();
    ring->width = width > 0 ? width : 0;
    ring->height = height > 0 ? height : 0;
    ring->visibleRows = getWaterfallRows(ring->height);
    ring->capacityRows = ring->visibleRows > 0 ? ring->visibleRows + kRingExtraRows : 0;
    ring->pixels.assign(static_cast<size_t>(ring->width) * ring->capacityRows, 0xFF000000u);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        surfaceWidth = width;
        surfaceHeight = height;
    }
    std::lock_guard<std::mutex> lock(g_ringPtrMutex);
    g_ring = std::move(ring);
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
    // In CW mode, zoom in around the decoder's marker instead of the fixed IF frequency.
    zoomCenteredFromTouch = zoomEnabled && cwDecoderEnabled && adjustableMarkerEnabled;
    if (zoomCenteredFromTouch) zoomCenterFrequencyHz = adjustableMarkerFrequencyHz;
    LOGI("setZoom: %s", zoomEnabled ? "true" : "false (±24kHz @DC)");
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
Java_hu_ha8mz_belkarx_MainActivity_setAdjustableMarkerEnabled(JNIEnv* env, jobject /* this */, jboolean enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    adjustableMarkerEnabled = (enabled == JNI_TRUE);
    if (adjustableMarkerEnabled) {
        adjustableMarkerFrequencyHz = computeReferenceFrequencyHz(surfaceWidth, currentSampleRate, demodMode);
    }
    LOGI("setAdjustableMarkerEnabled: %s", adjustableMarkerEnabled ? "true" : "false");
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setDemodMode(JNIEnv* env, jobject /* this */, jint mode) {
    std::lock_guard<std::mutex> lock(g_mutex);
    int clampedMode = static_cast<int>(mode);
    if (clampedMode < 0) clampedMode = 0;
    if (clampedMode > 2) clampedMode = 2;
    demodMode = clampedMode;
    if (adjustableMarkerEnabled) {
        adjustableMarkerFrequencyHz = computeReferenceFrequencyHz(surfaceWidth, currentSampleRate, demodMode);
    }
    const char* modeNames[] = {"AM", "USB", "LSB"};
    LOGI("setDemodMode: %s", modeNames[demodMode]);
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setAdjustableMarkerTouchX(JNIEnv* env, jobject /* this */, jfloat touchX) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!adjustableMarkerEnabled || surfaceWidth <= 1 || currentSampleRate <= 0) {
        return;
    }

    float clampedX = std::max(0.0f, std::min(touchX, static_cast<float>(surfaceWidth - 1)));
    double normalized = static_cast<double>(clampedX) / static_cast<double>(surfaceWidth - 1);
    double viewStartHz = 0.0;
    double viewSpanHz = 0.0;
    computeCurrentViewWindowHz(viewStartHz, viewSpanHz);
    if (viewSpanHz <= 1e-9) {
        return;
    }

    adjustableMarkerFrequencyHz = viewStartHz + normalized * viewSpanHz;
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

        std::string digits = std::to_string(std::abs(num));
        int numChars = static_cast<int>(digits.size());
        if (num != 0) {
            numChars += 1;  // sign
        }
        cx -= ((numChars * 4 - 1) * scale) / 2;
        if (num > 0) drawChar(10);
        else if (num < 0) {
            drawChar(11);
        }

        for (char ch : digits) {
            drawChar(ch - '0');
        }
    };

    if (viewSpanHz <= 1e-9) {
        return;
    }

    // Narrow views (CW zoom) get a tick and a label every kHz; wider ones every 5 kHz / 10 kHz.
    const bool fineScale = viewSpanHz < 10000.0;
    const double kTickStepHz = fineScale ? 1000.0 : 5000.0;
    const int labelEveryKHz = fineScale ? 1 : 10;
    double viewEndHz = viewStartHz + viewSpanHz;
    double firstRelativeHz = std::ceil((viewStartHz - referenceFrequencyHz) / kTickStepHz) * kTickStepHz;
    double lastRelativeHz = std::floor((viewEndHz - referenceFrequencyHz) / kTickStepHz) * kTickStepHz;

    for (double relativeHz = firstRelativeHz; relativeHz <= lastRelativeHz + 0.5; relativeHz += kTickStepHz) {
        double tickFrequencyHz = referenceFrequencyHz + relativeHz;
        int pixelX = static_cast<int>(std::lround(((tickFrequencyHz - viewStartHz) / viewSpanHz) * static_cast<double>(width)));
        if (pixelX < 0 || pixelX >= width) continue;

        int relativeKHz = static_cast<int>(std::lround(relativeHz / 1000.0));
        bool majorTick = (relativeKHz % labelEveryKHz) == 0;
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

static void drawReceiveBandBar(
    uint32_t* dest,
    int stride,
    int width,
    int height,
    double viewStartHz,
    double viewSpanHz,
    double referenceFrequencyHz,
    int mode) {
    if (viewSpanHz <= 1e-9 || width <= 0 || height <= 0) {
        return;
    }

    double startOffsetHz = 0.0;
    double endOffsetHz = 0.0;
    getDemodReceiveBandOffsetsHz(mode, startOffsetHz, endOffsetHz);
    double bandStartHz = referenceFrequencyHz + startOffsetHz;
    double bandEndHz = referenceFrequencyHz + endOffsetHz;
    if (bandEndHz < bandStartHz) {
        std::swap(bandStartHz, bandEndHz);
    }

    int x1 = static_cast<int>(std::lround(((bandStartHz - viewStartHz) / viewSpanHz) * static_cast<double>(width)));
    int x2 = static_cast<int>(std::lround(((bandEndHz - viewStartHz) / viewSpanHz) * static_cast<double>(width)));
    if (x2 < x1) std::swap(x1, x2);
    if (x2 < 0 || x1 >= width) return;

    if (x1 < 0) x1 = 0;
    if (x2 >= width) x2 = width - 1;

    const uint32_t ORANGE = 0xFF007FFF;
    int barY = kTopMargin - 2;
    if (barY < 0) barY = 0;
    int barYEnd = std::min(height - 1, barY + 4);
    for (int y = barY; y <= barYEnd; ++y) {
        for (int x = x1; x <= x2; ++x) {
            dest[y * stride + x] = ORANGE;
        }
    }
}

void drawArrowMarkerOnWindow(uint32_t* dest, int stride, int width, int height, int markerPixelX, int markerPixelY, double viewStartHz, double viewSpanHz, double referenceFrequencyHz, int mode) {
    if (markerPixelX < 0 || markerPixelX >= width) return;
    drawArrowHead(dest, stride, width, height, markerPixelX, markerPixelY);
    drawFrequencyGridLines(dest, stride, width, height, markerPixelX, markerPixelY, viewStartHz, viewSpanHz, referenceFrequencyHz);
    drawReceiveBandBar(dest, stride, width, height, viewStartHz, viewSpanHz, referenceFrequencyHz, mode);
}

void drawAdjustableMarker(uint32_t* dest, int stride, int width, int height, double viewStartHz, double viewSpanHz, double markerFrequencyHz, double referenceFrequencyHz) {
    if (viewSpanHz <= 1e-9 || width <= 0 || height <= 0) {
        return;
    }

    int markerX = static_cast<int>(std::lround(((markerFrequencyHz - viewStartHz) / viewSpanHz) * static_cast<double>(width)));
    if (markerX < 0 || markerX >= width) {
        return;
    }

    const uint32_t CYAN = 0xFFFFFF00;

    const uint8_t digitFont[10][5] = {
        {0x7, 0x5, 0x5, 0x5, 0x7}, {0x2, 0x6, 0x2, 0x2, 0x7}, {0x7, 0x1, 0x7, 0x4, 0x7},
        {0x7, 0x1, 0x7, 0x1, 0x7}, {0x5, 0x5, 0x7, 0x1, 0x1}, {0x7, 0x4, 0x7, 0x1, 0x7},
        {0x7, 0x4, 0x7, 0x5, 0x7}, {0x7, 0x1, 0x2, 0x2, 0x2}, {0x7, 0x5, 0x7, 0x5, 0x7},
        {0x7, 0x5, 0x7, 0x1, 0x7}
    };
    constexpr uint8_t kGlyphPlus[5] = {0x0, 0x2, 0x7, 0x2, 0x0};
    constexpr uint8_t kGlyphMinus[5] = {0x0, 0x0, 0x7, 0x0, 0x0};
    constexpr uint8_t kGlyphSpace[5] = {0x0, 0x0, 0x0, 0x0, 0x0};
    constexpr uint8_t kGlyphH[5] = {0x5, 0x5, 0x7, 0x5, 0x5};
    constexpr uint8_t kGlyphz[5] = {0x7, 0x1, 0x2, 0x4, 0x7};

    auto glyphForChar = [&](char ch) -> const uint8_t* {
        if (ch >= '0' && ch <= '9') return digitFont[ch - '0'];
        if (ch == '+') return kGlyphPlus;
        if (ch == '-') return kGlyphMinus;
        if (ch == 'H') return kGlyphH;
        if (ch == 'z') return kGlyphz;
        return kGlyphSpace;
    };

    constexpr int kTextScale = 5;
    constexpr int kTextGlyphWidth = 3;
    constexpr int kTextGlyphHeight = 5;
    constexpr int kTextCellWidth = 4;
    constexpr int kLabelPaddingX = 8;
    constexpr int kLabelPaddingY = 6;
    const uint32_t BLACK = 0xFF000000;

    auto drawText = [&](int x, int y, const std::string& text) {
        int scale = kTextScale;
        int cx = x;
        int textChars = static_cast<int>(text.size());
        cx -= ((textChars * 4 - 1) * scale) / 2;

        for (char ch : text) {
            const uint8_t* glyph = glyphForChar(ch);
            for (int r = 0; r < 5; ++r) {
                for (int c = 0; c < 3; ++c) {
                    if ((glyph[r] >> (2 - c)) & 1) {
                        for (int sy = 0; sy < scale; ++sy) {
                            for (int sx = 0; sx < scale; ++sx) {
                                int px = cx + c * scale + sx;
                                int py = y + r * scale + sy;
                                if (px >= 0 && px < width && py >= 0 && py < height) {
                                    dest[py * stride + px] = CYAN;
                                }
                            }
                        }
                    }
                }
            }
            cx += 4 * scale;
        }
    };

    auto fillRect = [&](int left, int top, int right, int bottom, uint32_t color) {
        if (left > right || top > bottom) {
            return;
        }
        if (left < 0) left = 0;
        if (right >= width) right = width - 1;
        if (top < 0) top = 0;
        if (bottom >= height) bottom = height - 1;
        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) {
                dest[y * stride + x] = color;
            }
        }
    };

    auto formatHzLabel = [](int hz) -> std::string {
        bool negative = hz < 0;
        int absHz = std::abs(hz);
        std::string digits = std::to_string(absHz);
        std::string grouped;
        grouped.reserve(digits.size() + digits.size() / 3);

        int firstGroup = static_cast<int>(digits.size()) % 3;
        if (firstGroup == 0) firstGroup = 3;
        grouped.append(digits.substr(0, firstGroup));
        for (size_t i = static_cast<size_t>(firstGroup); i < digits.size(); i += 3) {
            grouped.push_back(' ');
            grouped.append(digits.substr(i, 3));
        }

        std::string out;
        if (negative) {
            out.push_back('-');
        }
        out += grouped;
        out += " Hz";
        return out;
    };

    int deltaHz = static_cast<int>(std::lround(markerFrequencyHz - referenceFrequencyHz));
    std::string labelText = formatHzLabel(deltaHz);
    int textWidth = ((static_cast<int>(labelText.size()) * kTextCellWidth - 1) * kTextScale);
    int textHeight = kTextGlyphHeight * kTextScale;
    int labelTop = height - textHeight - (2 * kLabelPaddingY) - 12;
    if (labelTop < kTopMargin + 4) {
        labelTop = kTopMargin + 4;
    }
    int labelBottom = labelTop + textHeight + (2 * kLabelPaddingY) - 1;
    int labelLeft = markerX - (textWidth / 2) - kLabelPaddingX;
    int labelRight = markerX + ((textWidth + 1) / 2) + kLabelPaddingX - 1;

    fillRect(labelLeft, labelTop, labelRight, labelBottom, BLACK);
    drawText(markerX, labelTop + kLabelPaddingY, labelText);

    int lineEndY = labelTop - 4;
    if (lineEndY < kTopMargin) lineEndY = kTopMargin;
    for (int y = kTopMargin; y <= lineEndY; ++y) {
        dest[y * stride + markerX] = CYAN;
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

    constexpr int fftSize = 4096;
    if (size < 2 * fftSize) return;

    // Copy straight into a reused native buffer (no pinning, no extra Java-side copy).
    thread_local std::vector<jshort> samples;
    if (samples.size() != static_cast<size_t>(2 * fftSize)) samples.resize(2 * fftSize);
    env->GetShortArrayRegion(data, 0, 2 * fftSize, samples.data());

    RenderConfigSnapshot config;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        config = snapshotRenderConfigLocked();
    }

    thread_local std::vector<float> real, imag;
    if (real.size() != (size_t)fftSize) { real.resize(fftSize); imag.resize(fftSize); }

    auto t0 = Clock::now();
    unpackIqSamples(samples.data(), fftSize, config.swapIq, real, imag);
    unpackNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();

    // CW decoder: windows overlap by 50%, so only the newest hop is new audio.
    thread_local bool cwWasRunning = false;
    bool cwRunning = config.cwEnabled && config.adjustableMarkerEnabled;
    if (cwRunning) {
        if (!cwWasRunning) g_cwDecoder.reset();
        g_cwDecoder.configure(config.sampleRate, config.adjustableMarkerFrequencyHz);
        g_cwDecoder.process(&real[fftSize - kHopSize], &imag[fftSize - kHopSize], kHopSize);
    }
    cwWasRunning = cwRunning;

    auto fftStart = Clock::now();
    fft(real, imag);
    fftNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - fftStart).count();

    if (config.adjustableMarkerEnabled) {
        std::lock_guard<std::mutex> snapLock(g_snapMutex);
        if (g_snapPower.size() != static_cast<size_t>(fftSize)) g_snapPower.assign(fftSize, 0.0f);
        for (int k = 0; k < fftSize; k++) {
            float power = real[k] * real[k] + imag[k] * imag[k];
            g_snapPower[k] += (power - g_snapPower[k]) * 0.3f;
        }
    }

    std::shared_ptr<WaterfallRing> ring = loadRing();
    if (!ring || ring->width <= 0 || ring->visibleRows <= 0) return;
    const int width = ring->width;

    SpectrumSpan span = computeSpectrumSpan(fftSize, config.sampleRate, config.zoom, config.zoomHasCustomCenter,
                                            config.zoomCenterHz, config.cwEnabled);

    double minMag = 1e9, maxMag = -1e9;
    thread_local std::vector<double> lineMags;
    if ((int)lineMags.size() != width) lineMags.resize(width);
    auto magTs = Clock::now();
    computeLineMagnitudes(real, imag, fftSize, width, config.zoom, span, lineMags, minMag, maxMag);
    magPassNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - magTs).count();

    if (!config.showSpectrum) {
        auto tw = Clock::now();
        // Level -> color table with the contrast curve folded in, so there is no pow() per pixel.
        constexpr int kShapedLutSize = 1024;
        thread_local std::array<uint32_t, 256> colorLut;
        thread_local std::array<uint32_t, kShapedLutSize> shapedLut;
        thread_local int lutSens = -1, lutContr = -1, lutSc = -1;
        if (lutSens != config.sensitivity || lutContr != config.contrast || lutSc != config.colorScaleValue) {
            buildColorLut(config.sensitivity, config.contrast, config.colorScaleValue, colorLut);
            double contrastExponent = 0.5 + (config.contrast / 300.0) * 4.0;
            for (int i = 0; i < kShapedLutSize; i++) {
                double shaped = std::pow(static_cast<double>(i) / (kShapedLutSize - 1), contrastExponent) * 255.0;
                int idx = static_cast<int>(shaped);
                shapedLut[i] = colorLut[idx < 0 ? 0 : (idx > 255 ? 255 : idx)];
            }
            lutSens = config.sensitivity; lutContr = config.contrast; lutSc = config.colorScaleValue;
        }
        // Fixed dB window: sensitivity controls level placement, contrast only shapes color response.
        double wMin, wRange;
        if (config.fixedWindow) {
            double fwMax = 130.0 - (config.sensitivity - 100) * 0.8;
            constexpr double fwRange = 70.0;
            wMin = fwMax - fwRange;
            wRange = fwRange;
        } else {
            // Adaptive EMA autoscale
            updateSmoothedMagnitudeRange(minMag, maxMag);
            wMin = smoothedMinMag;
            wRange = smoothedMaxMag - smoothedMinMag + 1e-6;
        }
        double lutScale = (kShapedLutSize - 1) / wRange;

        // Write the new row into the slot after the newest one; the renderer never shows that slot.
        int64_t rowIndex = ring->rowsWritten.load(std::memory_order_relaxed);
        uint32_t* row = &ring->pixels[static_cast<size_t>(rowIndex % ring->capacityRows) * width];
        for (int x = 0; x < width; x++) {
            double t = (lineMags[x] - wMin) * lutScale;
            int idx = t <= 0.0 ? 0 : (t >= kShapedLutSize - 1 ? kShapedLutSize - 1 : static_cast<int>(t));
            row[x] = shapedLut[idx];
        }
        ring->rowsWritten.store(rowIndex + 1, std::memory_order_release);

        // Track the real line rate from arrival times (EMA of the interval over ~50 lines).
        thread_local Clock::time_point lastLineTime{};
        thread_local double emaIntervalSec = 0.0;
        thread_local int emaRate = 0;
        auto now = Clock::now();
        if (emaRate != config.sampleRate || emaIntervalSec <= 0.0) {
            emaRate = config.sampleRate;
            emaIntervalSec = config.sampleRate > 0 ? static_cast<double>(kHopSize) / config.sampleRate : 0.02;
        } else {
            double dt = std::chrono::duration<double>(now - lastLineTime).count();
            if (dt > 0.0 && dt < 0.5) {
                emaIntervalSec += (dt - emaIntervalSec) * 0.02;
            }
        }
        lastLineTime = now;
        g_rowsPerSecond.store(1.0 / emaIntervalSec, std::memory_order_relaxed);

        waterfallUpdateNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - tw).count();
    } else {
        std::lock_guard<std::mutex> specLk(g_spectrumMutex);
        if ((int)g_lineMagnitudes.size() != width) g_lineMagnitudes.resize(width);
        memcpy(g_lineMagnitudes.data(), lineMags.data(), width * sizeof(double));
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
        LOGI("audio avg ms: frame=%.3f unpack=%.3f fft=%.3f mag=%.3f water=%.3f lines/s=%.1f",
            (sFr*inv)/1e6, (sU*inv)/1e6, (sF*inv)/1e6, (sM*inv)/1e6, (sW*inv)/1e6,
            (profN * 1e9) / (double)(nowNs - profWin));
        profWin = nowNs; profN = 0; sU = sF = sM = sW = sFr = 0;
    }
}

// ============================================================================
// Vsync-paced GPU render, called once per display frame from MainActivity's
// render thread (its own Choreographer, not the UI thread).
//
// Lines arrive in bursts (audio buffer periods) at ~94/s, while the display runs
// at 60-120 Hz. The renderer keeps a small look-ahead of rows (kTargetLatencySec)
// and scrolls through them at the measured line rate, nudging the speed to keep
// the look-ahead steady. The scroll position is fractional and the GPU
// interpolates between rows, so every frame moves by exactly the right amount.
// ============================================================================
namespace {

constexpr double kTargetLatencySec = 0.08;  // look-ahead that absorbs audio burst jitter
constexpr double kCatchUpPerSec = 0.8;      // how fast the speed corrects the look-ahead error

struct ScrollPacer {
    const WaterfallRing* ring = nullptr;
    bool started = false;
    double position = 0.0;  // ring row index shown at the top of the waterfall (fractional)
    int64_t lastFrameNs = 0;
    int underruns = 0;      // frames where the scroll had to wait for data
};

// Render-thread-only state.
ScrollPacer g_pacer;
GlWaterfall g_gl;
int64_t g_rowsUploaded = 0;  // ring rows already copied into the GL texture
std::vector<uint32_t> g_overlayPixels;
bool g_overlayValid = false;
RenderConfigSnapshot g_overlayConfig{};
int g_overlayWidth = 0, g_overlayHeight = 0;

double advanceScroll(ScrollPacer& pacer, const WaterfallRing& ring, int64_t newestRow, int64_t frameTimeNs) {
    double newest = static_cast<double>(newestRow);
    double rate = g_rowsPerSecond.load(std::memory_order_relaxed);
    double targetLead = std::max(3.0, rate * kTargetLatencySec);
    // How far the writer may run ahead before it would overwrite rows still on screen.
    double maxLead = kRingExtraRows / 2.0;

    if (!pacer.started || pacer.ring != &ring) {
        pacer.ring = &ring;
        pacer.started = true;
        pacer.position = newest - targetLead;
        pacer.lastFrameNs = frameTimeNs;
    }

    double dt = static_cast<double>(frameTimeNs - pacer.lastFrameNs) / 1e9;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1;
    pacer.lastFrameNs = frameTimeNs;

    double lead = newest - pacer.position;
    if (lead > maxLead) {
        // Far behind (e.g. no frames were drawn for a while): jump instead of racing.
        pacer.position = newest - targetLead;
    } else {
        double speed = rate + (lead - targetLead) * kCatchUpPerSec;
        if (speed < 0.0) speed = 0.0;
        if (speed > rate * 1.5) speed = rate * 1.5;
        pacer.position += speed * dt;
        // Never scroll onto rows that have not been written yet. The B-spline filter reads up
        // to two rows ahead of floor(position), so stay at least one row behind the newest.
        if (pacer.position > newest - 1.0) {
            pacer.position = newest - 1.0;
            pacer.underruns++;
        }
    }
    return pacer.position;
}

// The overlay only depends on these settings (not on sensitivity, contrast or colors).
bool sameOverlayConfig(const RenderConfigSnapshot& a, const RenderConfigSnapshot& b) {
    return a.sampleRate == b.sampleRate && a.zoom == b.zoom && a.zoomHasCustomCenter == b.zoomHasCustomCenter &&
           a.zoomCenterHz == b.zoomCenterHz && a.demodMode == b.demodMode &&
           a.adjustableMarkerEnabled == b.adjustableMarkerEnabled &&
           a.adjustableMarkerFrequencyHz == b.adjustableMarkerFrequencyHz &&
           a.showSpectrum == b.showSpectrum && a.cwEnabled == b.cwEnabled && a.bottomInset == b.bottomInset;
}

// Draws the frequency scale, the demod marker and the adjustable marker into `dest`.
void drawOverlayMarks(uint32_t* dest, int width, int height, const RenderConfigSnapshot& config) {
    constexpr int kFftSize = 4096;
    SpectrumSpan scaleSpan = computeSpectrumSpan(
        kFftSize, config.sampleRate, config.zoom, config.zoomHasCustomCenter, config.zoomCenterHz, config.cwEnabled);
    double binHz = static_cast<double>(config.sampleRate) / static_cast<double>(kFftSize);

    double referenceFrequencyHz = computeReferenceFrequencyHz(width, config.sampleRate, config.demodMode);
    double viewStartHz = static_cast<double>(scaleSpan.startBin) * binHz;
    double viewSpanHz = static_cast<double>(scaleSpan.visibleBins) * binHz;
    int mx = static_cast<int>(std::lround(((referenceFrequencyHz - viewStartHz) / viewSpanHz) * static_cast<double>(width)));
    if (mx < 0) mx = 0;
    if (mx >= width) mx = width - 1;

    drawArrowMarkerOnWindow(dest, width, width, height, mx, 0, viewStartHz, viewSpanHz, referenceFrequencyHz, config.demodMode);
    // Keep the marker and its label above the CW text strip.
    int markerHeight = std::max(kTopMargin + 40, height - config.bottomInset);
    if (config.adjustableMarkerEnabled && config.cwEnabled && !config.showSpectrum && viewSpanHz > 1e-9) {
        // Translucent green band showing the decoder's 100 Hz channel.
        constexpr double kHalfBandwidthHz = 50.0;
        constexpr uint32_t kBandColor = 0x5000FF40u;  // ABGR: green, ~30% opacity
        int x1 = static_cast<int>(std::floor((config.adjustableMarkerFrequencyHz - kHalfBandwidthHz - viewStartHz) / viewSpanHz * width));
        int x2 = static_cast<int>(std::ceil((config.adjustableMarkerFrequencyHz + kHalfBandwidthHz - viewStartHz) / viewSpanHz * width));
        x1 = std::max(0, x1);
        x2 = std::min(width - 1, std::max(x2, x1 + 2));
        for (int y = kTopMargin; y < markerHeight; y++) {
            for (int x = x1; x <= x2; x++) dest[static_cast<size_t>(y) * width + x] = kBandColor;
        }
    }
    if (config.adjustableMarkerEnabled) {
        drawAdjustableMarker(dest, width, width, markerHeight, viewStartHz, viewSpanHz,
                             config.adjustableMarkerFrequencyHz, referenceFrequencyHz);
    }
}

// Copies the ring rows written since the last frame into the GL texture.
void uploadNewWaterfallRows(const WaterfallRing& ring, int64_t written) {
    int64_t first = std::max(g_rowsUploaded, written - ring.capacityRows + 1);
    if (first < 0) first = 0;
    while (first < written) {
        int slot = static_cast<int>(first % ring.capacityRows);
        int count = static_cast<int>(std::min<int64_t>(written - first, ring.capacityRows - slot));
        g_gl.uploadWaterfallRows(slot, count, ring.pixels.data());
        first += count;
    }
    g_rowsUploaded = written;
}

}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_renderFrame(JNIEnv* env, jobject, jlong frameTimeNanos) {
    using Clock = std::chrono::steady_clock;
    auto renderStart = Clock::now();

    std::lock_guard<std::mutex> windowLock(g_windowMutex);
    ANativeWindow* window = g_nativeWindow;
    if (!window) return;

    std::shared_ptr<WaterfallRing> ring = loadRing();
    if (!ring || ring->width <= 0 || ring->capacityRows <= 0) return;

    int sw = 0, sh = 0;
    if (!g_gl.makeCurrent(window, g_windowGeneration, sw, sh)) return;

    RenderConfigSnapshot config;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        config = snapshotRenderConfigLocked();
    }

    // Waterfall texture: recreate on resize, then upload only the new rows.
    int64_t written = ring->rowsWritten.load(std::memory_order_acquire);
    if (g_gl.ensureWaterfallTexture(ring.get(), ring->width, ring->capacityRows, ring->pixels.data())) {
        g_rowsUploaded = written;  // the initial upload already holds every row written so far
    }
    uploadNewWaterfallRows(*ring, written);

    double position = advanceScroll(g_pacer, *ring, written - 1, frameTimeNanos);
    int64_t oldestValid = std::max<int64_t>(0, written - ring->capacityRows + 1);
    // The B-spline filter also reads one row older than the sampled pair.
    double maxAge = position - static_cast<double>(oldestValid) - 1.0;

    // Overlay: markers and scale, redrawn only when they change. In spectrum mode the whole
    // spectrum frame goes into the overlay (opaque) every frame.
    size_t overlaySize = static_cast<size_t>(sw) * sh;
    if (g_overlayPixels.size() != overlaySize) {
        g_overlayPixels.assign(overlaySize, 0u);
        g_overlayValid = false;
    }
    bool overlayChanged = config.showSpectrum || !g_overlayValid || g_overlayWidth != sw ||
                          g_overlayHeight != sh || g_overlayConfig.showSpectrum ||
                          !sameOverlayConfig(config, g_overlayConfig);
    if (overlayChanged) {
        if (config.showSpectrum) {
            static std::vector<double> lineMagCopy;
            {
                std::lock_guard<std::mutex> specLk(g_spectrumMutex);
                lineMagCopy.assign(g_lineMagnitudes.begin(), g_lineMagnitudes.end());
            }
            static int64_t lastSpectrumRenderNs = 0;
            double frameDeltaSec = 1.0 / 60.0;
            if (lastSpectrumRenderNs > 0) {
                frameDeltaSec = static_cast<double>(frameTimeNanos - lastSpectrumRenderNs) / 1e9;
            }
            lastSpectrumRenderNs = frameTimeNanos;

            SpectrumRenderParams spectrumParams{
                sw, sh, kTopMargin,
                config.sensitivity, config.contrast, config.colorScaleValue,
                config.spectrumFilled, config.spectrumConstantColor,
                0.60f, 45.0, frameDeltaSec};
            if (!drawSpectrumFrame(g_overlayPixels.data(), sw, lineMagCopy, spectrumSmoothedBuffer,
                                   spectrumDecayBuffer, spectrumParams)) {
                std::fill(g_overlayPixels.begin(), g_overlayPixels.end(), 0xFF000000u);
            }
        } else {
            std::fill(g_overlayPixels.begin(), g_overlayPixels.end(), 0u);  // transparent
        }
        drawOverlayMarks(g_overlayPixels.data(), sw, sh, config);
        g_gl.uploadOverlay(sw, sh, g_overlayPixels.data());
        g_overlayValid = true;
        g_overlayConfig = config;
        g_overlayWidth = sw;
        g_overlayHeight = sh;
    }

    g_gl.draw(sw, sh, kTopMargin, position, config.showSpectrum ? -1.0 : maxAge);
    auto swapStart = Clock::now();
    g_gl.present();
    auto renderEnd = Clock::now();

    int64_t workNs = std::chrono::duration_cast<std::chrono::nanoseconds>(swapStart - renderStart).count();
    int64_t swapNs = std::chrono::duration_cast<std::chrono::nanoseconds>(renderEnd - swapStart).count();
    static int64_t profWin = 0; static int profN = 0; static int64_t sWork = 0, sSwap = 0;
    int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(renderEnd.time_since_epoch()).count();
    if (profWin == 0) profWin = nowNs;
    profN++; sWork += workNs; sSwap += swapNs;
    if (nowNs - profWin >= 2000000000LL && profN > 0) {
        double secs = static_cast<double>(nowNs - profWin) / 1e9;
        LOGI("gl render: fps=%.1f work=%.3fms swap=%.3fms rows/s=%.1f lead=%.1f rows underruns=%d",
             profN / secs, (sWork / static_cast<double>(profN)) / 1e6, (sSwap / static_cast<double>(profN)) / 1e6,
             g_rowsPerSecond.load(std::memory_order_relaxed),
             static_cast<double>(written - 1) - position, g_pacer.underruns);
        profWin = nowNs; profN = 0; sWork = 0; sSwap = 0; g_pacer.underruns = 0;
    }
}

// Render thread only: drops the EGL surface before the window is destroyed.
extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_releaseRenderSurface(JNIEnv*, jobject) {
    g_gl.releaseSurface();
}

// Render thread only: tears down the whole GL context.
extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_releaseRenderer(JNIEnv*, jobject) {
    g_gl.releaseAll();
    g_overlayValid = false;
    g_pacer = ScrollPacer{};
    g_rowsUploaded = 0;
}

// Fills `out` with colors of the given waterfall color scale, from weakest to strongest
// (ARGB, evenly spaced), for the color scale swatches in the UI.
extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_getColorScalePreview(JNIEnv* env, jobject, jint scale, jintArray out) {
    if (out == nullptr) return;
    int count = env->GetArrayLength(out);
    if (count <= 0) return;
    std::vector<jint> colors(count);
    for (int i = 0; i < count; i++) {
        double intensity = count > 1 ? 255.0 * i / (count - 1) : 255.0;
        uint32_t abgr = getColorWithParams(intensity, 100, 100, scale);
        uint32_t r = abgr & 0xFF, g = (abgr >> 8) & 0xFF, b = (abgr >> 16) & 0xFF;
        colors[i] = static_cast<jint>(0xFF000000u | (r << 16) | (g << 8) | b);
    }
    env->SetIntArrayRegion(out, 0, count, colors.data());
}

// ============================================================================
// CW decoder controls (UI thread)
// ============================================================================
extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setCwEnabled(JNIEnv*, jobject, jboolean enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    cwDecoderEnabled = (enabled == JNI_TRUE);
    // Turning CW on with zoom already on: the narrower CW zoom window is centered on the marker.
    if (cwDecoderEnabled && zoomEnabled && adjustableMarkerEnabled && !zoomCenteredFromTouch) {
        zoomCenteredFromTouch = true;
        zoomCenterFrequencyHz = adjustableMarkerFrequencyHz;
    }
    LOGI("setCwEnabled: %s", cwDecoderEnabled ? "true" : "false");
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setOverlayBottomInset(JNIEnv*, jobject, jint pixels) {
    std::lock_guard<std::mutex> lock(g_mutex);
    overlayBottomInset = pixels > 0 ? pixels : 0;
}

// Moves the marker onto the strongest signal within +-100 Hz (sub-bin accurate).
// Returns false if there is no spectrum yet.
extern "C" JNIEXPORT jboolean JNICALL
Java_hu_ha8mz_belkarx_MainActivity_snapMarkerToPeak(JNIEnv*, jobject) {
    constexpr int kFftSize = 4096;
    constexpr double kSearchHz = 100.0;
    double markerHz;
    int sampleRate;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!adjustableMarkerEnabled || currentSampleRate <= 0) return JNI_FALSE;
        markerHz = adjustableMarkerFrequencyHz;
        sampleRate = currentSampleRate;
    }
    double binHz = static_cast<double>(sampleRate) / kFftSize;
    int center = static_cast<int>(std::lround(markerHz / binHz));
    int reach = std::max(1, static_cast<int>(std::ceil(kSearchHz / binHz)));

    double peakHz;
    {
        std::lock_guard<std::mutex> snapLock(g_snapMutex);
        if (g_snapPower.size() != static_cast<size_t>(kFftSize)) return JNI_FALSE;
        auto powerAt = [](int logicalBin) {
            int index = ((logicalBin % kFftSize) + kFftSize) % kFftSize;
            return g_snapPower[index];
        };
        int best = center;
        float bestPower = -1.0f;
        for (int k = center - reach; k <= center + reach; k++) {
            float power = powerAt(k);
            if (power > bestPower) {
                bestPower = power;
                best = k;
            }
        }
        if (bestPower <= 0.0f) return JNI_FALSE;
        // Parabolic interpolation on the log power of the peak and its neighbours.
        double l = std::log(powerAt(best - 1) + 1e-20);
        double c = std::log(bestPower + 1e-20);
        double r = std::log(powerAt(best + 1) + 1e-20);
        double denom = l - 2.0 * c + r;
        double delta = (denom < 0.0) ? 0.5 * (l - r) / denom : 0.0;
        delta = std::clamp(delta, -0.5, 0.5);
        peakHz = (best + delta) * binHz;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    adjustableMarkerFrequencyHz = peakHz;
    LOGI("snapMarkerToPeak: %.1f Hz -> %.1f Hz", markerHz, peakHz);
    return JNI_TRUE;
}

// Decoded text since the previous call.
extern "C" JNIEXPORT jstring JNICALL
Java_hu_ha8mz_belkarx_MainActivity_cwPollText(JNIEnv* env, jobject) {
    std::string text = g_cwDecoder.takeText();
    return env->NewStringUTF(text.c_str());
}

// Fills out[0..5] with WPM, SNR (dB), key down (0/1), signal present (0/1),
// keying cut level (dB over noise), squelch in use (dB over noise).
extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_cwGetStatus(JNIEnv* env, jobject, jfloatArray out) {
    if (out == nullptr || env->GetArrayLength(out) < 6) return;
    CwStatus status = g_cwDecoder.status();
    jfloat values[6] = {status.wpm, status.snrDb, status.keyDown ? 1.0f : 0.0f, status.signal ? 1.0f : 0.0f,
                        status.effectiveThresholdDb, status.squelchDb};
    env->SetFloatArrayRegion(out, 0, 6, values);
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setCwAutoThreshold(JNIEnv*, jobject, jboolean enabled) {
    g_cwDecoder.setAutoThreshold(enabled == JNI_TRUE);
}

// Squelch level of the CW decoder, dB above the noise floor.
extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setCwThresholdDb(JNIEnv*, jobject, jfloat db) {
    g_cwDecoder.setThresholdDb(db);
}

// Copies the newest level history (dB over noise, 100 points/s, oldest first) into `out` and
// the number of points produced so far into total[0]. Returns the number of points copied.
extern "C" JNIEXPORT jint JNICALL
Java_hu_ha8mz_belkarx_MainActivity_cwGetScope(JNIEnv* env, jobject, jfloatArray out, jlongArray total) {
    if (out == nullptr || total == nullptr || env->GetArrayLength(total) < 1) return 0;
    int capacity = std::min(static_cast<int>(env->GetArrayLength(out)), CwDecoder::kScopePoints);
    float points[CwDecoder::kScopePoints];
    int64_t produced = 0;
    int count = g_cwDecoder.copyScope(points, capacity, produced);
    env->SetFloatArrayRegion(out, 0, count, points);
    jlong producedValue = produced;
    env->SetLongArrayRegion(total, 0, 1, &producedValue);
    return count;
}

extern "C" JNIEXPORT void JNICALL
Java_hu_ha8mz_belkarx_MainActivity_setNativeSurface(JNIEnv* env, jobject /* this */, jobject surface) {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    setNativeSurfaceLocked(env, surface);
    LOGI("setNativeSurface: %s", surface != nullptr ? "updated" : "cleared");
}
