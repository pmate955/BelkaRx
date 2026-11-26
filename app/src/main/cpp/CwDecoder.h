#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct CwStatus {
    float wpm;        // 0 until the speed is known
    float snrDb;      // mark level over noise level
    bool keyDown;     // tone currently detected
    bool signal;      // tone level above the user threshold
    float effectiveThresholdDb;  // keying cut level in use, dB above the noise floor
    float squelchDb;             // squelch in use (the manual value, or the automatic one)
};

// Morse (CW) decoder working on the complex I/Q stream.
//
// Signal path (audio thread):
//   I/Q at the input rate -> mix the selected tone down to 0 Hz -> boxcar decimation to ~4 kHz
//   -> 4th-order Butterworth low-pass on I and Q (100 Hz wide channel) -> envelope
//   -> averaged to ~500 Hz (2 ms steps) -> adaptive threshold with hysteresis and debounce
//   -> mark/space timing -> dots and dashes -> characters.
//
// The dot length is estimated continuously from the recent marks (two clusters: dots and
// dashes) and, when only one kind of element was seen, from the gaps inside characters.
class CwDecoder {
public:
    // Audio thread. Retunes to `toneHz` (offset from the I/Q center, may be negative).
    void configure(int sampleRate, double toneHz);
    void process(const float* inI, const float* inQ, int count);
    void reset();

    // Any thread.
    std::string takeText();
    CwStatus status() const;
    // Squelch: the tone must reach this many dB above the noise floor to be decoded.
    void setThresholdDb(float db);
    // Automatic squelch: set a little above the noise peaks measured during pauses.
    void setAutoThreshold(bool enabled);
    // Copies up to maxPoints of the newest level history (dB over the noise floor, one point per
    // 1/kScopeRateHz s, oldest first). Returns the number of points written; outTotal receives
    // the number of points produced so far (the newest copied point has index outTotal - 1).
    int copyScope(float* out, int maxPoints, int64_t& outTotal) const;

    static constexpr float kMinThresholdDb = 3.0f;
    static constexpr float kMaxThresholdDb = 30.0f;
    static constexpr float kDefaultThresholdDb = 8.0f;
    static constexpr float kScopeRateHz = 100.0f;
    static constexpr int kScopePoints = 512;

private:
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z1 = 0, z2 = 0;
        float run(float x) {
            float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void clear() { z1 = z2 = 0; }
    };

    void designFilters();
    void resetLevels();
    void processEnvelopeSample(float env);
    void onMarkEnd(int length);
    void onSpaceEnd(int length);
    void updateDitEstimate();
    float shortSpaceEstimate() const;
    void emitChar();
    void emitText(const std::string& text);

    int sampleRate_ = 0;
    double toneHz_ = 0.0;

    // Local oscillator (unit phasor rotated by step_ every input sample).
    double oscRe_ = 1.0, oscIm_ = 0.0;
    double stepRe_ = 1.0, stepIm_ = 0.0;

    // Stage 1: boxcar decimation after the mixer.
    int dec1_ = 48;
    int dec1Count_ = 0;
    float acc1I_ = 0.0f, acc1Q_ = 0.0f;
    float fs1_ = 4000.0f;

    // Stage 2: channel filter.
    Biquad lpI_[2], lpQ_[2];

    // Stage 3: envelope averaged down to ~500 Hz.
    int dec2_ = 8;
    int dec2Count_ = 0;
    float accEnv_ = 0.0f;
    float envFs_ = 500.0f;

    // Levels.
    float env_ = 0.0f;
    float noise_ = 0.0f;
    float mark_ = 0.0f;
    bool levelsInit_ = false;
    int warmupCount_ = 0;
    float warmupSum_ = 0.0f;
    bool signal_ = false;

    // Keying state, durations in envelope samples.
    bool keyDown_ = false;
    int stateLen_ = 0;
    int candidateLen_ = 0;
    std::vector<int> charMarks_;  // mark lengths of the character being received
    bool wordSpaceDone_ = true;
    std::vector<int> marks_;
    std::vector<int> spaces_;
    float ditSamples_ = 30.0f;  // 60 ms at 500 Hz = 20 WPM
    bool ditKnown_ = false;

    mutable std::mutex textMutex_;
    std::string pending_;
    std::atomic<float> wpm_{0.0f};
    std::atomic<float> snrDb_{0.0f};
    std::atomic<bool> keyDownOut_{false};
    std::atomic<bool> signalOut_{false};
    std::atomic<float> thresholdDb_{kDefaultThresholdDb};
    std::atomic<bool> autoThreshold_{true};
    std::atomic<float> activeThresholdDb_{kDefaultThresholdDb};

    // Auto squelch state (audio thread).
    float noisePeakDb_ = 0.0f;
    bool noisePeakKnown_ = false;
    float noiseWindowMaxDb_ = -100.0f;
    int noiseWindowCount_ = 0;
    std::atomic<float> effectiveThresholdDb_{kDefaultThresholdDb};

    // Level graph ring buffer (written by the audio thread, read by the UI).
    mutable std::mutex scopeMutex_;
    float scope_[kScopePoints] = {};
    int scopeHead_ = 0;
    int scopeFilled_ = 0;
    int64_t scopeTotal_ = 0;
    int scopeCount_ = 0;
    float scopePeakDb_ = -100.0f;
};
