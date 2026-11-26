#include "CwDecoder.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace {

constexpr float kChannelCutoffHz = 50.0f;   // low-pass on I and Q -> 100 Hz wide channel
constexpr float kStage1RateHz = 4000.0f;
constexpr float kEnvelopeRateHz = 500.0f;
constexpr size_t kHistory = 24;             // marks/spaces kept for the speed estimate
constexpr float kNoiseGateRatio = 2.0f;     // samples below noise +6 dB update the noise floor
constexpr float kNoiseTauSec = 0.5f;        // noise floor averaging near the floor
constexpr float kNoiseEscapeTauSec = 20.0f; // noise floor creep from louder samples
constexpr float kMarkReleaseDits = 4.0f;    // tone level follows fades within ~4 dots (key down)
constexpr float kMarkReleasePauseSec = 1.0f;// ... and within ~1 s during pauses
constexpr float kCutUpperFraction = 0.55f;  // key-down cut, fraction of the noise..tone amplitude
constexpr float kCutLowerFraction = 0.40f;  // key-up cut
constexpr float kAutoWindowSec = 0.2f;      // auto squelch: noise peak measured per 0.2 s
constexpr float kAutoFallSec = 1.0f;        // ... estimate falls within ~1 s
constexpr float kAutoRiseSec = 10.0f;       // ... and rises within ~10 s
constexpr float kAutoMarginDb = 2.5f;       // ... squelch this far above the noise peaks
constexpr float kAutoMinThresholdDb = 4.0f;
constexpr float kMinDitSec = 0.024f;        // 50 WPM: fastest speed decoded
constexpr float kMaxDitSec = 0.240f;        // 5 WPM: slowest speed decoded

const std::unordered_map<std::string, std::string>& morseTable() {
    static const std::unordered_map<std::string, std::string> table = {
        {".-", "A"}, {"-...", "B"}, {"-.-.", "C"}, {"-..", "D"}, {".", "E"}, {"..-.", "F"},
        {"--.", "G"}, {"....", "H"}, {"..", "I"}, {".---", "J"}, {"-.-", "K"}, {".-..", "L"},
        {"--", "M"}, {"-.", "N"}, {"---", "O"}, {".--.", "P"}, {"--.-", "Q"}, {".-.", "R"},
        {"...", "S"}, {"-", "T"}, {"..-", "U"}, {"...-", "V"}, {".--", "W"}, {"-..-", "X"},
        {"-.--", "Y"}, {"--..", "Z"},
        {"-----", "0"}, {".----", "1"}, {"..---", "2"}, {"...--", "3"}, {"....-", "4"},
        {".....", "5"}, {"-....", "6"}, {"--...", "7"}, {"---..", "8"}, {"----.", "9"},
        {".-.-.-", "."}, {"--..--", ","}, {"..--..", "?"}, {"-..-.", "/"}, {"-...-", "="},
        {".-.-.", "+"}, {"-....-", "-"}, {".----.", "'"}, {"-.--.", "("}, {"-.--.-", ")"},
        {"---...", ":"}, {"-.-.-.", ";"}, {".-..-.", "\""}, {".--.-.", "@"}, {"-.-.--", "!"},
        {"..--.-", "_"}, {".-...", "&"},
        {"...-.-", "<SK>"}, {"...---...", "<SOS>"}, {"........", "<HH>"},
    };
    return table;
}

// Two-cluster split of durations; returns false if there are fewer than 2 values.
bool twoMeans(const std::vector<int>& values, float& low, float& high, int& lowCount, int& highCount) {
    if (values.size() < 2) return false;
    auto [minIt, maxIt] = std::minmax_element(values.begin(), values.end());
    low = static_cast<float>(*minIt);
    high = static_cast<float>(*maxIt);
    for (int iter = 0; iter < 8; iter++) {
        float split = 0.5f * (low + high);
        double sumLow = 0, sumHigh = 0;
        lowCount = highCount = 0;
        for (int v : values) {
            if (v < split) { sumLow += v; lowCount++; } else { sumHigh += v; highCount++; }
        }
        if (lowCount > 0) low = static_cast<float>(sumLow / lowCount);
        if (highCount > 0) high = static_cast<float>(sumHigh / highCount);
    }
    return true;
}

void pushHistory(std::vector<int>& history, int value) {
    history.push_back(value);
    if (history.size() > kHistory) history.erase(history.begin());
}

}  // namespace

void CwDecoder::designFilters() {
    // RBJ low-pass biquads; Q values of a 4th-order Butterworth.
    const float qs[2] = {0.54119610f, 1.30656296f};
    float w0 = 2.0f * static_cast<float>(M_PI) * kChannelCutoffHz / fs1_;
    float cosW = std::cos(w0);
    float sinW = std::sin(w0);
    for (int s = 0; s < 2; s++) {
        float alpha = sinW / (2.0f * qs[s]);
        float a0 = 1.0f + alpha;
        Biquad bq;
        bq.b0 = (1.0f - cosW) * 0.5f / a0;
        bq.b1 = (1.0f - cosW) / a0;
        bq.b2 = (1.0f - cosW) * 0.5f / a0;
        bq.a1 = -2.0f * cosW / a0;
        bq.a2 = (1.0f - alpha) / a0;
        lpI_[s] = bq;
        lpQ_[s] = bq;
    }
}

void CwDecoder::configure(int sampleRate, double toneHz) {
    if (sampleRate <= 0) return;
    if (sampleRate != sampleRate_) {
        sampleRate_ = sampleRate;
        dec1_ = std::max(1, static_cast<int>(std::lround(sampleRate / kStage1RateHz)));
        fs1_ = static_cast<float>(sampleRate) / dec1_;
        dec2_ = std::max(1, static_cast<int>(std::lround(fs1_ / kEnvelopeRateHz)));
        envFs_ = fs1_ / dec2_;
        ditSamples_ = 0.060f * envFs_;
        designFilters();
        reset();
        toneHz_ = toneHz + 1e9;  // force the oscillator update below
    }
    if (std::fabs(toneHz - toneHz_) > 1e-6) {
        // A big jump means a different signal: start the level tracking over.
        if (std::fabs(toneHz - toneHz_) > kChannelCutoffHz * 0.6) resetLevels();
        toneHz_ = toneHz;
        double angle = -2.0 * M_PI * toneHz / sampleRate_;
        stepRe_ = std::cos(angle);
        stepIm_ = std::sin(angle);
    }
}

void CwDecoder::resetLevels() {
    levelsInit_ = false;
    warmupCount_ = 0;
    warmupSum_ = 0.0f;
    signal_ = false;
    keyDown_ = false;
    stateLen_ = 0;
    candidateLen_ = 0;
    charMarks_.clear();
    wordSpaceDone_ = true;
}

void CwDecoder::reset() {
    oscRe_ = 1.0;
    oscIm_ = 0.0;
    dec1Count_ = 0;
    acc1I_ = acc1Q_ = 0.0f;
    dec2Count_ = 0;
    accEnv_ = 0.0f;
    for (auto& bq : lpI_) bq.clear();
    for (auto& bq : lpQ_) bq.clear();
    env_ = noise_ = mark_ = 0.0f;
    marks_.clear();
    spaces_.clear();
    noisePeakKnown_ = false;
    noiseWindowMaxDb_ = -100.0f;
    noiseWindowCount_ = 0;
    ditKnown_ = false;
    resetLevels();
    wpm_.store(0.0f);
    snrDb_.store(0.0f);
    keyDownOut_.store(false);
    signalOut_.store(false);
}

void CwDecoder::process(const float* inI, const float* inQ, int count) {
    if (sampleRate_ <= 0) return;
    const float invDec1 = 1.0f / dec1_;
    const float invDec2 = 1.0f / dec2_;
    for (int n = 0; n < count; n++) {
        float x = inI[n];
        float y = inQ[n];
        float oRe = static_cast<float>(oscRe_);
        float oIm = static_cast<float>(oscIm_);
        acc1I_ += x * oRe - y * oIm;
        acc1Q_ += x * oIm + y * oRe;
        double nextRe = oscRe_ * stepRe_ - oscIm_ * stepIm_;
        oscIm_ = oscRe_ * stepIm_ + oscIm_ * stepRe_;
        oscRe_ = nextRe;

        if (++dec1Count_ < dec1_) continue;
        dec1Count_ = 0;
        float i1 = lpI_[1].run(lpI_[0].run(acc1I_ * invDec1));
        float q1 = lpQ_[1].run(lpQ_[0].run(acc1Q_ * invDec1));
        acc1I_ = acc1Q_ = 0.0f;

        accEnv_ += std::sqrt(i1 * i1 + q1 * q1);
        if (++dec2Count_ < dec2_) continue;
        dec2Count_ = 0;
        processEnvelopeSample(accEnv_ * invDec2);
        accEnv_ = 0.0f;
    }
    // Keep the oscillator on the unit circle.
    double mag = std::sqrt(oscRe_ * oscRe_ + oscIm_ * oscIm_);
    if (mag > 0.0) {
        oscRe_ /= mag;
        oscIm_ /= mag;
    }
}

void CwDecoder::processEnvelopeSample(float e) {
    // Smooth over ~1/5 of a dot: removes noise spikes without blurring the keying.
    float alpha = std::clamp(1.0f / (0.2f * ditSamples_), 0.15f, 1.0f);
    env_ += (e - env_) * alpha;

    // Warm-up (~100 ms): let the filters settle and take the average level as the noise floor
    // before making any key decision; otherwise the first noise looks like a long mark.
    if (!levelsInit_) {
        int warmup = static_cast<int>(0.1f * envFs_);
        warmupCount_++;
        warmupSum_ += env_;
        if (warmupCount_ < warmup) return;
        noise_ = warmupSum_ / warmupCount_;
        mark_ = noise_;
        levelsInit_ = true;
    }

    // Tone level: fast attack. It follows a fading signal down within ~4 dots while the key is
    // down, and within ~1 s during pauses, so the cut level drops along with QSB instead of
    // staying at the last peak (that lost every mark once the signal faded ~6 dB).
    if (env_ > mark_) {
        mark_ += (env_ - mark_) * 0.2f;
    } else {
        float releaseSec = keyDown_ ? kMarkReleaseDits * ditSamples_ / envFs_ : kMarkReleasePauseSec;
        mark_ += (env_ - mark_) * (1.0f / (releaseSec * envFs_));
    }
    // Noise floor: learns only from samples near the floor (below +6 dB), averaging over ~0.5 s,
    // so tones and their decaying tails cannot pull it up (that made the graph and the
    // thresholds behave like an AGC during long transmissions). Louder samples still move it,
    // very slowly (~20 s), so a floor that really rose (band noise) is still followed. Faster
    // creep (tested: 3 s) pulls the floor up during long transmissions and the tones sink.
    if (env_ < kNoiseGateRatio * noise_) noise_ += (env_ - noise_) * (1.0f / (kNoiseTauSec * envFs_));
    else noise_ += (env_ - noise_) * (1.0f / (kNoiseEscapeTauSec * envFs_));

    float floor = std::max(noise_, 1e-12f);
    float ratio = mark_ / floor;
    float levelDb = 20.0f * std::log10(std::max(env_ / floor, 1e-6f));

    // Auto squelch: measure how high the noise peaks reach during pauses (key up for more than
    // 3 dots), as the maximum of each 0.2 s, and set the squelch a little above. The estimate
    // falls within ~1 s but rises only over ~10 s: during a weak transmission the marks the
    // decoder misses also land in "pauses", and following them quickly pushed the squelch up
    // until the whole signal was lost.
    if (!keyDown_ && stateLen_ > 3.0f * ditSamples_) {
        noiseWindowMaxDb_ = std::max(noiseWindowMaxDb_, levelDb);
        if (++noiseWindowCount_ >= static_cast<int>(kAutoWindowSec * envFs_)) {
            if (!noisePeakKnown_) {
                noisePeakDb_ = noiseWindowMaxDb_;
                noisePeakKnown_ = true;
            } else {
                float tau = noiseWindowMaxDb_ > noisePeakDb_ ? kAutoRiseSec : kAutoFallSec;
                noisePeakDb_ += (noiseWindowMaxDb_ - noisePeakDb_) * (kAutoWindowSec / tau);
            }
            noiseWindowMaxDb_ = -100.0f;
            noiseWindowCount_ = 0;
        }
    }
    float thresholdDb = thresholdDb_.load(std::memory_order_relaxed);
    if (autoThreshold_.load(std::memory_order_relaxed)) {
        thresholdDb = noisePeakKnown_
            ? std::clamp(noisePeakDb_ + kAutoMarginDb, kAutoMinThresholdDb, kMaxThresholdDb)
            : kDefaultThresholdDb;
    }
    activeThresholdDb_.store(thresholdDb, std::memory_order_relaxed);

    // The threshold (dB above the noise floor) works as a squelch: the tone peaks have to
    // reach it before anything is decoded. The keying itself is cut just above halfway between
    // noise and tone, where the edges are steepest, so the timing stays accurate. (Using the
    // threshold as the cut would chop up signals only slightly above it.)
    float userLevel = floor * std::pow(10.0f, thresholdDb / 20.0f);
    signal_ = mark_ > userLevel;
    // Cut levels just above halfway between noise and tone (linear amplitude), where the keying
    // edges are steepest, so marks and gaps keep their true length. (A lower, dB-scale cut was
    // tested: slightly better on deep fades, but it stretches the marks and breaks 50 WPM.)
    float span = mark_ - noise_;
    float upper = noise_ + kCutUpperFraction * span;
    float lower = noise_ + kCutLowerFraction * span;
    bool want = signal_ && (keyDown_ ? env_ > lower : env_ > upper);

    // Level graph: envelope relative to the noise floor, peak of each 10 ms.
    scopePeakDb_ = std::max(scopePeakDb_, levelDb);
    if (++scopeCount_ >= std::max(1, static_cast<int>(envFs_ / kScopeRateHz + 0.5f))) {
        std::lock_guard<std::mutex> lock(scopeMutex_);
        scope_[scopeHead_] = scopePeakDb_;
        scopeHead_ = (scopeHead_ + 1) % kScopePoints;
        if (scopeFilled_ < kScopePoints) scopeFilled_++;
        scopeTotal_++;
        scopeCount_ = 0;
        scopePeakDb_ = -100.0f;
    }
    effectiveThresholdDb_.store(20.0f * std::log10(std::max(upper / floor, 1e-6f)), std::memory_order_relaxed);

    // Debounce: a new state must last ~1/4 dot, and never less than half a dot at the top speed
    // (12 ms). Shorter blips are noise; letting them through drags the speed estimate to the
    // maximum on weak signals. The state's length is counted from its first sample.
    int debounce = std::max(static_cast<int>(0.5f * kMinDitSec * envFs_ + 0.5f),
                            static_cast<int>(ditSamples_ * 0.25f));
    stateLen_++;
    if (want != keyDown_) {
        if (++candidateLen_ >= debounce) {
            int length = stateLen_ - candidateLen_;
            if (keyDown_) onMarkEnd(length);
            else onSpaceEnd(length);
            keyDown_ = want;
            stateLen_ = candidateLen_;
            candidateLen_ = 0;
        }
    } else {
        candidateLen_ = 0;
    }

    // During silence, finish the character after 2 dots and add a word gap after 5.
    if (!keyDown_) {
        if (!charMarks_.empty() && stateLen_ > 2.0f * ditSamples_) emitChar();
        if (!wordSpaceDone_ && stateLen_ > 5.0f * ditSamples_) {
            emitText(" ");
            wordSpaceDone_ = true;
        }
    }

    snrDb_.store(20.0f * std::log10(std::max(ratio, 1e-6f)), std::memory_order_relaxed);
    keyDownOut_.store(keyDown_, std::memory_order_relaxed);
    signalOut_.store(signal_, std::memory_order_relaxed);
}

void CwDecoder::onMarkEnd(int length) {
    if (length <= 0) return;
    // A mark much longer than a dash (tuning, a carrier, noise) would wreck the speed estimate.
    float maxPlausible = ditKnown_ ? 6.0f * ditSamples_ : 0.5f * envFs_;
    if (length <= maxPlausible) {
        pushHistory(marks_, length);
        updateDitEstimate();
    }
    // Dots and dashes are told apart when the character ends, with the speed estimate that
    // already includes this character (matters at the start of a fast transmission).
    charMarks_.push_back(length);
    if (charMarks_.size() > 9) {
        // Way too long for any symbol: probably a stuck carrier or noise.
        charMarks_.clear();
        emitText("*");
    }
    wordSpaceDone_ = false;
}

void CwDecoder::onSpaceEnd(int length) {
    // Long pauses say nothing about the speed.
    if (length > 0 && length < 10.0f * ditSamples_) pushHistory(spaces_, length);
}

float CwDecoder::shortSpaceEstimate() const {
    float low, high;
    int lowCount, highCount;
    if (!twoMeans(spaces_, low, high, lowCount, highCount)) return 0.0f;
    // Gaps inside characters are 1 dot, between characters 3 dots.
    return (high > 2.0f * low) ? low : 0.5f * (low + high);
}

void CwDecoder::updateDitEstimate() {
    if (marks_.size() < 3) return;
    float low, high;
    int lowCount, highCount;
    twoMeans(marks_, low, high, lowCount, highCount);

    float dit;
    if (high >= 1.8f * low && lowCount > 0 && highCount > 0) {
        // Dots and dashes both present: dash = 3 dots.
        dit = (lowCount * low + highCount * high / 3.0f) / (lowCount + highCount);
    } else {
        // Only one kind of element so far: decide by comparing with the gaps inside characters.
        double sum = 0;
        for (int v : marks_) sum += v;
        float mean = static_cast<float>(sum / marks_.size());
        float gap = shortSpaceEstimate();
        if (gap > 0.0f) dit = (std::fabs(mean - 3.0f * gap) < std::fabs(mean - gap)) ? mean / 3.0f : mean;
        else dit = ditKnown_ ? ((mean > 2.0f * ditSamples_) ? mean / 3.0f : mean) : mean;
    }

    float minDit = kMinDitSec * envFs_;  // 50 WPM
    float maxDit = kMaxDitSec * envFs_;  // 5 WPM
    dit = std::clamp(dit, minDit, maxDit);
    ditSamples_ = ditKnown_ ? 0.7f * ditSamples_ + 0.3f * dit : dit;
    ditKnown_ = true;

    float ditMs = ditSamples_ / envFs_ * 1000.0f;
    wpm_.store(1200.0f / ditMs, std::memory_order_relaxed);
}

void CwDecoder::emitChar() {
    std::string code;
    for (int length : charMarks_) code += (length < 2.0f * ditSamples_) ? '.' : '-';
    charMarks_.clear();
    const auto& table = morseTable();
    auto it = table.find(code);
    emitText(it != table.end() ? it->second : "*");
}

void CwDecoder::emitText(const std::string& text) {
    std::lock_guard<std::mutex> lock(textMutex_);
    pending_ += text;
    if (pending_.size() > 4096) pending_.erase(0, pending_.size() - 4096);
}

std::string CwDecoder::takeText() {
    std::lock_guard<std::mutex> lock(textMutex_);
    std::string out;
    out.swap(pending_);
    return out;
}

CwStatus CwDecoder::status() const {
    return {wpm_.load(std::memory_order_relaxed), snrDb_.load(std::memory_order_relaxed),
            keyDownOut_.load(std::memory_order_relaxed), signalOut_.load(std::memory_order_relaxed),
            effectiveThresholdDb_.load(std::memory_order_relaxed),
            activeThresholdDb_.load(std::memory_order_relaxed)};
}

void CwDecoder::setAutoThreshold(bool enabled) {
    autoThreshold_.store(enabled, std::memory_order_relaxed);
}

void CwDecoder::setThresholdDb(float db) {
    thresholdDb_.store(std::clamp(db, kMinThresholdDb, kMaxThresholdDb), std::memory_order_relaxed);
}

int CwDecoder::copyScope(float* out, int maxPoints, int64_t& outTotal) const {
    std::lock_guard<std::mutex> lock(scopeMutex_);
    outTotal = scopeTotal_;
    int count = std::min(maxPoints, scopeFilled_);
    // Oldest first.
    int start = (scopeHead_ - count + kScopePoints) % kScopePoints;
    for (int i = 0; i < count; i++) out[i] = scope_[(start + i) % kScopePoints];
    return count;
}
