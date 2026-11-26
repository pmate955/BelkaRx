#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct RttyStatus {
    float snrDb;   // tone level over the level in the other (idle) filter, dB
    bool signal;   // squelch open: characters are printed
    bool mark;     // current bit decision (after inversion)
};

// RTTY (Baudot / ITA2 FSK) decoder working on the complex I/Q stream.
//
// Signal path (audio thread):
//   I/Q at the input rate -> mix the center between mark and space down to 0 Hz
//   -> decimating FIR to ~8 kHz -> decimating FIR to ~2 kHz
//   -> mark and space filters (each tone mixed to 0 Hz, FIR low-pass ~0.75 x baud)
//   -> envelopes -> W7AY "optimal ATC" mark/space decision (handles selective fading)
//   -> start-stop framing: 1 start bit, 5 data bits (LSB first), >= 1 stop bit, each bit
//      decided from the middle half of the bit -> ITA2 letters/figures -> text.
//
// Mark is the higher RF frequency (center + shift/2) unless inverted.
class RttyDecoder {
public:
    // Audio thread. Retunes to `centerHz` (offset from the I/Q center, may be negative).
    void configure(int sampleRate, double centerHz, float baud, float shiftHz, bool invert);
    void process(const float* inI, const float* inQ, int count);
    void reset();

    // Any thread.
    std::string takeText();
    RttyStatus status() const;
    // Copies up to maxPoints of the newest tuning scope points (x = mark filter, y = space filter,
    // normalized to about +-1, interleaved x,y, oldest first). Returns the number of points.
    int copyScope(float* outXY, int maxPoints) const;

    static constexpr int kScopePoints = 1024;
    // Squelch on the SNR estimate (pure noise reads ~4.7 dB). Tested on white noise: about one
    // stray character in 9 minutes, while 8 dB (Eb/N0) signals still print.
    static constexpr float kSquelchOpenDb = 7.0f;   // SNR needed to start printing
    static constexpr float kSquelchCloseDb = 6.0f;  // ... and to keep printing

private:
    struct ComplexFirDecimator {
        std::vector<float> taps;
        std::vector<float> bufI, bufQ;  // each sample stored twice: the window is always contiguous
        int pos = 0;
        int phase = 0;
        int decim = 1;
        void design(double fs, double cutoffHz, int numTaps, int decimation, bool blackman);
        void clear();
        // Returns true when an output sample is produced.
        bool push(float i, float q, float& outI, float& outQ);
    };

    struct Oscillator {
        double re = 1.0, im = 0.0, stepRe = 1.0, stepIm = 0.0;
        void setFrequency(double hz, double fs);
        // Multiplies (i, q) by the oscillator and advances it.
        void mix(float i, float q, float& outI, float& outQ);
        void normalize();
    };

    void designToneFilters();
    void retuneTones();
    void resetLevels();
    void processBasebandSample(float i, float q);
    void decodeBit(float v, bool bit);
    void emitCode(int code);
    void emitText(const char* text);

    // Settings (audio thread).
    int sampleRate_ = 0;
    double centerHz_ = 0.0;
    float baud_ = 50.0f;
    float shiftHz_ = 450.0f;
    bool invert_ = false;

    Oscillator centerOsc_;
    ComplexFirDecimator dec1_, dec2_;
    double fs2_ = 2000.0;
    Oscillator markOsc_, spaceOsc_;
    ComplexFirDecimator markFilter_, spaceFilter_;
    double samplesPerBit_ = 40.0;

    // Levels (ATC).
    float markEnv_ = 0.0f, spaceEnv_ = 0.0f;
    float markNoise_ = 0.0f, spaceNoise_ = 0.0f;
    bool levelsInit_ = false;
    int warmup_ = 0;
    float onPower_ = 0.0f, offPower_ = 0.0f;  // power in the selected / the other filter
    bool signal_ = false;

    // Framing.
    enum class FrameState { WaitMark, WaitStart, InChar };
    FrameState frameState_ = FrameState::WaitMark;
    double bitClock_ = 0.0;  // samples since the start edge
    int startRun_ = 0;       // consecutive space samples while waiting for a start bit
    float bitAcc_[7] = {};
    bool figures_ = false;
    bool lastWasNewline_ = true;

    // Scope (display carrier rotates the filter outputs so a steady tone draws a line).
    Oscillator scopeOsc_;
    float scopeGain_ = 0.0f;
    std::vector<float> scopeStage_;

    mutable std::mutex textMutex_;
    std::string pending_;
    std::atomic<float> snrDbOut_{0.0f};
    std::atomic<bool> signalOut_{false};
    std::atomic<bool> markOut_{true};

    mutable std::mutex scopeMutex_;
    float scope_[2 * kScopePoints] = {};
    int scopeHead_ = 0;
    int scopeFilled_ = 0;
};
