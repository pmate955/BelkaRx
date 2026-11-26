#include "RttyDecoder.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kStage1RateHz = 8000.0;
constexpr double kStage2RateHz = 2000.0;
constexpr double kStage1PassHz = 1000.0;  // kept by stage 1 (+-1 kHz around the center)
constexpr double kStage2PassHz = 600.0;   // kept by stage 2: enough for 850 Hz shift
constexpr float kToneCutoffBauds = 0.75f; // mark/space low-pass cutoff, in multiples of the baud rate
constexpr float kToneFilterBits = 2.0f;   // mark/space filter length, in bits
constexpr float kEnvAttackBits = 0.25f;   // ATC envelope: follows a rising tone within 1/4 bit
constexpr float kEnvDecayBits = 16.0f;    // ... and a fading one within 16 bits
constexpr float kNoiseFallBits = 0.25f;   // ATC noise floor: falls within 1/4 bit
constexpr float kNoiseRiseBits = 48.0f;   // ... rises within 48 bits
constexpr float kSnrAverageBits = 16.0f;  // SNR (squelch) averaging
constexpr double kStartConfirmBits = 0.15; // start edge confirmed after this long at space
constexpr float kScopeCarrierRatio = 0.0704f;  // display carrier, fraction of the 2 kHz rate

// ITA2. Codes are the 5 data bits, first transmitted bit = bit 0. 0 = nothing to print.
// The figures shift of F, G, H is not assigned in ITA2; the usual US TTY characters are used.
constexpr char kLetters[32] = {
    0,   'E', '\n', 'A', ' ', 'S', 'I', 'U', '\n', 'D', 'R', 'J', 'N', 'F', 'C', 'K',
    'T', 'Z', 'L',  'W', 'H', 'Y', 'P', 'Q', 'O',  'B', 'G', 0,   'M', 'X', 'V', 0};
constexpr char kFigures[32] = {
    0,   '3', '\n', '-', ' ', '\'', '8', '7', '\n', 0,   '4', 0,   ',', '!', ':', '(',
    '5', '+', ')',  '2', '#', '6',  '0', '1', '9',  '?', '&', 0,   '.', '/', '=', 0};
constexpr int kCodeFigures = 27;
constexpr int kCodeLetters = 31;

}  // namespace

// ---------------------------------------------------------------------------------------------
// Helpers

void RttyDecoder::ComplexFirDecimator::design(double fs, double cutoffHz, int numTaps, int decimation,
                                              bool blackman) {
    numTaps = std::max(1, numTaps | 1);  // odd: symmetric around a center tap
    taps.assign(numTaps, 0.0f);
    double fc = cutoffHz / fs;
    double mid = 0.5 * (numTaps - 1);
    double sum = 0.0;
    for (int n = 0; n < numTaps; n++) {
        double t = n - mid;
        double sinc = (std::fabs(t) < 1e-9) ? 2.0 * fc : std::sin(2.0 * M_PI * fc * t) / (M_PI * t);
        double x = numTaps > 1 ? static_cast<double>(n) / (numTaps - 1) : 0.5;
        double window = blackman
            ? 0.42 - 0.5 * std::cos(2.0 * M_PI * x) + 0.08 * std::cos(4.0 * M_PI * x)
            : 0.5 - 0.5 * std::cos(2.0 * M_PI * x);
        if (numTaps > 1 && !blackman && (n == 0 || n == numTaps - 1)) window = 0.0;
        double h = sinc * window;
        taps[n] = static_cast<float>(h);
        sum += h;
    }
    if (std::fabs(sum) > 1e-12) {
        for (float& h : taps) h = static_cast<float>(h / sum);  // unity gain at 0 Hz
    }
    decim = std::max(1, decimation);
    bufI.assign(2 * numTaps, 0.0f);
    bufQ.assign(2 * numTaps, 0.0f);
    pos = 0;
    phase = 0;
}

void RttyDecoder::ComplexFirDecimator::clear() {
    std::fill(bufI.begin(), bufI.end(), 0.0f);
    std::fill(bufQ.begin(), bufQ.end(), 0.0f);
    pos = 0;
    phase = 0;
}

bool RttyDecoder::ComplexFirDecimator::push(float i, float q, float& outI, float& outQ) {
    const int n = static_cast<int>(taps.size());
    bufI[pos] = bufI[pos + n] = i;
    bufQ[pos] = bufQ[pos + n] = q;
    pos = (pos + 1 == n) ? 0 : pos + 1;
    if (++phase < decim) return false;
    phase = 0;
    // bufX[pos .. pos + n - 1]: the last n samples, oldest first. The taps are symmetric.
    const float* wi = &bufI[pos];
    const float* wq = &bufQ[pos];
    const float* h = taps.data();
    float accI = 0.0f, accQ = 0.0f;
    for (int k = 0; k < n; k++) {
        accI += h[k] * wi[k];
        accQ += h[k] * wq[k];
    }
    outI = accI;
    outQ = accQ;
    return true;
}

void RttyDecoder::Oscillator::setFrequency(double hz, double fs) {
    double angle = 2.0 * M_PI * hz / fs;
    stepRe = std::cos(angle);
    stepIm = std::sin(angle);
}

void RttyDecoder::Oscillator::mix(float i, float q, float& outI, float& outQ) {
    float oRe = static_cast<float>(re);
    float oIm = static_cast<float>(im);
    outI = i * oRe - q * oIm;
    outQ = i * oIm + q * oRe;
    double nextRe = re * stepRe - im * stepIm;
    im = re * stepIm + im * stepRe;
    re = nextRe;
}

void RttyDecoder::Oscillator::normalize() {
    double mag = std::sqrt(re * re + im * im);
    if (mag > 0.0) {
        re /= mag;
        im /= mag;
    }
}

// ---------------------------------------------------------------------------------------------
// Configuration

void RttyDecoder::configure(int sampleRate, double centerHz, float baud, float shiftHz, bool invert) {
    if (sampleRate <= 0 || baud <= 0.0f || shiftHz <= 0.0f) return;
    bool rateChanged = sampleRate != sampleRate_;
    if (rateChanged) {
        sampleRate_ = sampleRate;
        // Stage 1: input rate -> ~8 kHz. Keeps +-1 kHz; everything that would alias into it is
        // in the stop band (Blackman: ~-74 dB).
        int d1 = std::max(1, static_cast<int>(std::lround(sampleRate / kStage1RateHz)));
        double fs1 = static_cast<double>(sampleRate) / d1;
        double transition1 = std::max(500.0, fs1 - 2.0 * kStage1PassHz);
        int taps1 = d1 > 1 ? static_cast<int>(std::ceil(5.5 * sampleRate / transition1)) : 1;
        dec1_.design(sampleRate, 0.5 * fs1, taps1, d1, true);
        // Stage 2: ~8 kHz -> ~2 kHz. Keeps +-600 Hz.
        int d2 = std::max(1, static_cast<int>(std::lround(fs1 / kStage2RateHz)));
        fs2_ = fs1 / d2;
        double transition2 = std::max(200.0, fs2_ - 2.0 * kStage2PassHz);
        dec2_.design(fs1, 0.5 * fs2_, static_cast<int>(std::ceil(5.5 * fs1 / transition2)), d2, true);
        scopeOsc_.setFrequency(kScopeCarrierRatio * fs2_, fs2_);
    }
    bool baudChanged = rateChanged || baud != baud_;
    bool tonesChanged = baudChanged || shiftHz != shiftHz_ || invert != invert_;
    baud_ = baud;
    shiftHz_ = shiftHz;
    invert_ = invert;
    if (baudChanged) designToneFilters();
    if (tonesChanged) retuneTones();
    if (rateChanged) {
        reset();
        centerHz_ = centerHz + 1e9;  // force the oscillator update below
    } else if (tonesChanged) {
        resetLevels();
    }

    if (std::fabs(centerHz - centerHz_) > 1e-6) {
        // A big jump means a different signal: start the level tracking over.
        if (std::fabs(centerHz - centerHz_) > 0.5 * baud_) resetLevels();
        centerHz_ = centerHz;
        centerOsc_.setFrequency(-centerHz, sampleRate_);
    }
}

void RttyDecoder::designToneFilters() {
    samplesPerBit_ = fs2_ / baud_;
    int taps = static_cast<int>(std::lround(kToneFilterBits * samplesPerBit_));
    markFilter_.design(fs2_, kToneCutoffBauds * baud_, taps, 1, false);
    spaceFilter_.design(fs2_, kToneCutoffBauds * baud_, taps, 1, false);
}

void RttyDecoder::retuneTones() {
    // Mark is the higher frequency unless inverted. Each tone is mixed down to 0 Hz.
    double markHz = (invert_ ? -0.5 : 0.5) * shiftHz_;
    markOsc_.setFrequency(-markHz, fs2_);
    spaceOsc_.setFrequency(markHz, fs2_);
}

void RttyDecoder::resetLevels() {
    levelsInit_ = false;
    warmup_ = 0;
    markEnv_ = spaceEnv_ = markNoise_ = spaceNoise_ = 0.0f;
    onPower_ = offPower_ = 0.0f;
    signal_ = false;
    frameState_ = FrameState::WaitMark;
    bitClock_ = 0.0;
    startRun_ = 0;
    figures_ = false;
    snrDbOut_.store(0.0f, std::memory_order_relaxed);
    signalOut_.store(false, std::memory_order_relaxed);
}

void RttyDecoder::reset() {
    centerOsc_.re = 1.0;
    centerOsc_.im = 0.0;
    markOsc_.re = spaceOsc_.re = scopeOsc_.re = 1.0;
    markOsc_.im = spaceOsc_.im = scopeOsc_.im = 0.0;
    dec1_.clear();
    dec2_.clear();
    markFilter_.clear();
    spaceFilter_.clear();
    scopeGain_ = 0.0f;
    lastWasNewline_ = true;
    resetLevels();
    std::lock_guard<std::mutex> lock(scopeMutex_);
    scopeHead_ = 0;
    scopeFilled_ = 0;
}

// ---------------------------------------------------------------------------------------------
// Signal path

void RttyDecoder::process(const float* inI, const float* inQ, int count) {
    if (sampleRate_ <= 0) return;
    scopeStage_.clear();
    for (int n = 0; n < count; n++) {
        float i0, q0, i1, q1, i2, q2;
        centerOsc_.mix(inI[n], inQ[n], i0, q0);
        if (!dec1_.push(i0, q0, i1, q1)) continue;
        if (!dec2_.push(i1, q1, i2, q2)) continue;
        processBasebandSample(i2, q2);
    }
    centerOsc_.normalize();
    markOsc_.normalize();
    spaceOsc_.normalize();
    scopeOsc_.normalize();

    if (!scopeStage_.empty()) {
        std::lock_guard<std::mutex> lock(scopeMutex_);
        for (size_t k = 0; k + 1 < scopeStage_.size(); k += 2) {
            scope_[2 * scopeHead_] = scopeStage_[k];
            scope_[2 * scopeHead_ + 1] = scopeStage_[k + 1];
            scopeHead_ = (scopeHead_ + 1) % kScopePoints;
            if (scopeFilled_ < kScopePoints) scopeFilled_++;
        }
    }
}

void RttyDecoder::processBasebandSample(float i, float q) {
    float mi, mq, si, sq;
    markOsc_.mix(i, q, mi, mq);
    spaceOsc_.mix(i, q, si, sq);
    float fmI, fmQ, fsI, fsQ;
    markFilter_.push(mi, mq, fmI, fmQ);
    spaceFilter_.push(si, sq, fsI, fsQ);
    float mark = std::sqrt(fmI * fmI + fmQ * fmQ);
    float space = std::sqrt(fsI * fsI + fsQ * fsQ);

    // Tuning scope: both filter outputs turned by the same display carrier, so a steady mark tone
    // draws a horizontal line, a space tone a vertical one (the classic "crossed ellipses").
    float c = static_cast<float>(scopeOsc_.re);
    float s = static_cast<float>(scopeOsc_.im);
    float x = fmI * c - fmQ * s;
    float y = fsI * c - fsQ * s;
    double nextRe = scopeOsc_.re * scopeOsc_.stepRe - scopeOsc_.im * scopeOsc_.stepIm;
    scopeOsc_.im = scopeOsc_.re * scopeOsc_.stepIm + scopeOsc_.im * scopeOsc_.stepRe;
    scopeOsc_.re = nextRe;

    const float spb = static_cast<float>(samplesPerBit_);
    // Warm-up: let the filters fill (about 3 bits) before any decision, then start the
    // trackers from the average level.
    if (!levelsInit_) {
        int warmupLength = static_cast<int>(3.0f * spb);
        markEnv_ += mark;
        spaceEnv_ += space;
        if (++warmup_ < warmupLength) return;
        float mean = 0.5f * (markEnv_ + spaceEnv_) / warmup_;
        markEnv_ = spaceEnv_ = markNoise_ = spaceNoise_ = mean;
        onPower_ = offPower_ = mean * mean;
        scopeGain_ = std::max(scopeGain_, mean);
        levelsInit_ = true;
    }

    // ATC trackers (as in fldigi): envelopes follow the tone peaks, noise floors the valleys.
    auto envelope = [spb](float& env, float value) {
        float bits = value > env ? kEnvAttackBits : kEnvDecayBits;
        env += (value - env) / (bits * spb);
    };
    auto noise = [spb](float& floor, float value) {
        float bits = value < floor ? kNoiseFallBits : kNoiseRiseBits;
        floor += (value - floor) / (bits * spb);
    };
    envelope(markEnv_, mark);
    envelope(spaceEnv_, space);
    noise(markNoise_, mark);
    noise(spaceNoise_, space);
    float floor = std::min(markNoise_, spaceNoise_);

    // W7AY optimal ATC: each tone is weighted by its own envelope, so a tone that fades away
    // (selective fading) does not bias the decision.
    float mClip = std::clamp(mark, floor, std::max(floor, markEnv_));
    float sClip = std::clamp(space, floor, std::max(floor, spaceEnv_));
    float mE = markEnv_ - floor;
    float sE = spaceEnv_ - floor;
    float v = (mClip - floor) * mE - (sClip - floor) * sE - 0.25f * (mE * mE - sE * sE);
    bool bit = v > 0.0f;

    // SNR: power in the filter of the decided tone over the power in the other one.
    float on = bit ? mark : space;
    float off = bit ? space : mark;
    float k = 1.0f / (kSnrAverageBits * spb);
    onPower_ += (on * on - onPower_) * k;
    offPower_ += (off * off - offPower_) * k;
    float snrDb = 10.0f * std::log10(std::max(onPower_, 1e-30f) / std::max(offPower_, 1e-30f));
    signal_ = signal_ ? snrDb > kSquelchCloseDb : snrDb > kSquelchOpenDb;

    decodeBit(v, bit);

    // Scope scale follows the stronger tone (fast up, slow down).
    float level = std::max(markEnv_, spaceEnv_);
    scopeGain_ += (level - scopeGain_) * (level > scopeGain_ ? 0.05f : 1.0f / (8.0f * spb));
    float gain = scopeGain_ > 1e-12f ? 0.8f / scopeGain_ : 0.0f;
    scopeStage_.push_back(std::clamp(x * gain, -1.2f, 1.2f));
    scopeStage_.push_back(std::clamp(y * gain, -1.2f, 1.2f));

    snrDbOut_.store(snrDb, std::memory_order_relaxed);
    signalOut_.store(signal_, std::memory_order_relaxed);
    markOut_.store(bit, std::memory_order_relaxed);
}

// Start-stop framing. Each bit is decided from the sum of v over the middle half of the bit,
// timed from the mark -> space edge of the start bit.
void RttyDecoder::decodeBit(float v, bool bit) {
    switch (frameState_) {
        case FrameState::WaitMark:
            if (bit) frameState_ = FrameState::WaitStart;
            return;
        case FrameState::WaitStart: {
            // The start edge counts once the line has stayed at space for ~1/6 bit, so a short
            // noise spike in the stop bit does not start a character at the wrong time.
            if (bit) {
                startRun_ = 0;
                return;
            }
            int needed = std::max(1, static_cast<int>(kStartConfirmBits * samplesPerBit_));
            if (++startRun_ < needed) return;
            frameState_ = FrameState::InChar;
            bitClock_ = startRun_ - 0.5;  // the edge was, on average, half a sample before the first space
            startRun_ = 0;
            std::fill(std::begin(bitAcc_), std::end(bitAcc_), 0.0f);
            return;
        }
        case FrameState::InChar:
            break;
    }

    double pos = bitClock_ / samplesPerBit_;
    int index = static_cast<int>(pos);
    double frac = pos - index;
    if (index < 7 && frac >= 0.25 && frac < 0.75) bitAcc_[index] += v;
    bitClock_ += 1.0;

    if (index == 0 && frac >= 0.75 && bitAcc_[0] >= 0.0f) {
        // The start bit did not hold: a noise spike. Look for the next edge.
        frameState_ = bit ? FrameState::WaitStart : FrameState::WaitMark;
        return;
    }
    if (pos < 6.75) return;

    // Stop bit (mark) checked in its first half, so 1, 1.5 and 2 stop bits all work.
    frameState_ = FrameState::WaitMark;
    if (bitAcc_[6] <= 0.0f) return;  // framing error: drop the character
    int code = 0;
    for (int b = 0; b < 5; b++) {
        if (bitAcc_[b + 1] > 0.0f) code |= 1 << b;
    }
    emitCode(code);
}

void RttyDecoder::emitCode(int code) {
    // Shifts are followed even while the squelch is closed, so the case is right when it opens.
    if (code == kCodeLetters) {
        figures_ = false;
        return;
    }
    if (code == kCodeFigures) {
        figures_ = true;
        return;
    }
    if (!signal_) return;
    char ch = figures_ ? kFigures[code] : kLetters[code];
    if (ch == 0) return;
    if (ch == '\n') {
        // CR and LF both end a line; CR LF, CR CR LF etc. give one line break.
        if (lastWasNewline_) return;
        lastWasNewline_ = true;
        emitText("\n");
        return;
    }
    if (ch == ' ' && lastWasNewline_) return;  // no leading spaces on a new line
    lastWasNewline_ = false;
    char text[2] = {ch, 0};
    emitText(text);
}

void RttyDecoder::emitText(const char* text) {
    std::lock_guard<std::mutex> lock(textMutex_);
    pending_ += text;
    if (pending_.size() > 4096) pending_.erase(0, pending_.size() - 4096);
}

// ---------------------------------------------------------------------------------------------
// Any thread

std::string RttyDecoder::takeText() {
    std::lock_guard<std::mutex> lock(textMutex_);
    std::string out;
    out.swap(pending_);
    return out;
}

RttyStatus RttyDecoder::status() const {
    return {snrDbOut_.load(std::memory_order_relaxed), signalOut_.load(std::memory_order_relaxed),
            markOut_.load(std::memory_order_relaxed)};
}

int RttyDecoder::copyScope(float* outXY, int maxPoints) const {
    std::lock_guard<std::mutex> lock(scopeMutex_);
    int count = std::min(maxPoints, scopeFilled_);
    int start = (scopeHead_ - count + kScopePoints) % kScopePoints;
    for (int k = 0; k < count; k++) {
        int index = (start + k) % kScopePoints;
        outXY[2 * k] = scope_[2 * index];
        outXY[2 * k + 1] = scope_[2 * index + 1];
    }
    return count;
}
