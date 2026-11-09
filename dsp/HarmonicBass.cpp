#include "dsp/HarmonicBass.h"

#include <algorithm>
#include <cmath>

#include "dsp/DspMath.h"

namespace sonar::dsp {

namespace {
constexpr float kSilenceFloorDb = -120.0f;

// Below this the low band counts as empty: no division by ~zero, and no harmonics
// grown from the noise floor.
constexpr float kEnvelopeFloor = 1e-4f;  // about -80 dBFS
}  // namespace

HarmonicBass::HarmonicBass() { prepare(48000.0f); }

void HarmonicBass::prepare(float sampleRate) {
    sampleRate_ = sampleRate > 0.0f ? sampleRate : 48000.0f;
    rebuild();
    reset();
}

void HarmonicBass::setSettings(const HarmonicBassSettings& settings) {
    s_ = settings;
    s_.harmonics = std::clamp(s_.harmonics, kMinHarmonics, kMaxHarmonics);
    s_.crossoverHz = std::clamp(s_.crossoverHz, 30.0f, 300.0f);
    s_.evenWeight = std::clamp(s_.evenWeight, 0.0f, 1.0f);
    rebuild();
}

void HarmonicBass::reset() {
    for (Biquad& bq : lowSplit_) bq.reset();
    for (Biquad& bq : highSplit_) bq.reset();
    for (Biquad& bq : harmHigh_) bq.reset();
    for (Biquad& bq : harmLow_) bq.reset();
    env_ = 0.0f;
    meters_ = HarmonicBassMeters{};
}

void HarmonicBass::rebuild() {
    const float sr = sampleRate_;
    const float fc = s_.crossoverHz;

    // Linkwitz-Riley 4th order: two Butterworth sections at the same corner.
    for (Biquad& bq : lowSplit_) {
        bq = lowPass(fc, kButterworthQ, sr);
    }
    for (Biquad& bq : highSplit_) {
        bq = highPass(fc, kButterworthQ, sr);
    }
    for (Biquad& bq : harmHigh_) {
        bq = highPass(fc, kButterworthQ, sr);
    }
    // The top of the harmonic band. One crossover above the highest harmonic
    // requested, so that harmonic is passed rather than sitting on the slope.
    const float top = fc * static_cast<float>(s_.harmonics) * 1.25f;
    for (Biquad& bq : harmLow_) {
        bq = lowPass(top, kButterworthQ, sr);
    }

    attackCoef_ = envCoef(s_.attackMs, sr);
    releaseCoef_ = envCoef(s_.releaseMs, sr);

    // Per-order weights falling off as 1/(n-1), like a real bass note's series; tilted
    // by evenWeight, then normalised so the shaper stays in [-1, 1] and `amountDb`
    // means the same whatever the count.
    std::fill(std::begin(weight_), std::end(weight_), 0.0f);
    float sum = 0.0f;
    for (int n = kMinHarmonics; n <= s_.harmonics; ++n) {
        const float base = 1.0f / static_cast<float>(n - 1);
        const bool even = (n % 2) == 0;
        const float w = base * (even ? s_.evenWeight : 1.0f - s_.evenWeight);
        weight_[n] = w;
        sum += w;
    }
    if (sum > kTinyLinear) {
        for (int n = kMinHarmonics; n <= s_.harmonics; ++n) {
            weight_[n] /= sum;
        }
    }
}

void HarmonicBass::process(float* samples, int count) {
    process(samples, samples, count);
}

void HarmonicBass::process(const float* in, float* out, int count) {
    if (in == nullptr || out == nullptr || count <= 0) return;
    if (!s_.enabled) {
        // Still a copy when the buffers differ: a disabled stage must pass the
        // signal, and a host that gave us two buffers gets silence otherwise.
        if (in != out) {
            std::copy_n(in, static_cast<std::size_t>(count), out);
        }
        meters_ = HarmonicBassMeters{};
        return;
    }

    const float amount = dbToLin(s_.amountDb);
    const int orders = s_.harmonics;
    const bool replace = s_.replaceLows;

    float lowPeak = 0.0f;
    float harmPeak = 0.0f;

    for (int i = 0; i < count; ++i) {
        float x = in[static_cast<std::size_t>(i)];
        // A device or a decoder that hands us a NaN must not poison every filter
        // state in the chain for the rest of the session.
        if (!std::isfinite(x)) {
            x = 0.0f;
        }

        // --- The band we synthesise from ------------------------------------
        float low = lowSplit_[0].process(x);
        low = lowSplit_[1].process(low);
        lowPeak = std::max(lowPeak, std::abs(low));

        // --- Envelope of the real bass --------------------------------------
        // Scales everything, so the harmonics breathe with the bass line.
        const float rect = std::abs(low);
        const float coef = rect > env_ ? attackCoef_ : releaseCoef_;
        env_ = rect + coef * (env_ - rect);

        // --- Chebyshev harmonic generation ----------------------------------
        float h = 0.0f;
        if (env_ > kEnvelopeFloor) {
            // Normalised to the envelope (T_n is clean only on [-1, 1]); clamped because the
            // envelope lags fast transients.
            const float u = std::clamp(low / env_, -1.0f, 1.0f);

            // T_0 = 1, T_1 = u, T_{n+1} = 2u*T_n - T_{n-1}. The recurrence costs
            // two multiplies per order — no pow, no trig, no table.
            float tPrev = 1.0f;
            float tCur = u;
            for (int n = kMinHarmonics; n <= orders; ++n) {
                const float tNext = 2.0f * u * tCur - tPrev;
                tPrev = tCur;
                tCur = tNext;
                h += weight_[n] * tNext;
            }
            // Back to the scale of the music. The harmonics now track the bass
            // line's own dynamics exactly.
            h *= env_;
        }

        // --- Band-limit -------------------------------------------------------
        // T_2 and T_4 carry a constant term; without the highpass it is a thumping DC offset.
        h = harmHigh_[0].process(h);
        h = harmHigh_[1].process(h);
        h = harmLow_[0].process(h);
        h = harmLow_[1].process(h);
        h *= amount;
        harmPeak = std::max(harmPeak, std::abs(h));

        // --- Recombine --------------------------------------------------------
        if (replace) {
            x = highSplit_[1].process(highSplit_[0].process(x));
        }

        out[static_cast<std::size_t>(i)] = x + h;
    }

    meters_.lowBandDb = lowPeak > 0.0f ? linToDb(lowPeak) : kSilenceFloorDb;
    meters_.harmonicsDb = harmPeak > 0.0f ? linToDb(harmPeak) : kSilenceFloorDb;
    meters_.active = env_ > kEnvelopeFloor;
}

std::string_view harmonicBassCrossoverHint(float crossoverHz) {
    // What a crossover setting implies about the hardware, so the number in the
    // UI means something to somebody who has not read the algorithm.
    struct Range {
        float below;
        std::string_view hint;
    };
    static constexpr Range kRanges[] = {
        {55.0f, "subwoofer / full-range"},
        {80.0f, "large headphones"},
        {120.0f, "headphones / earbuds"},
        {180.0f, "small speakers"},
    };
    for (const Range& r : kRanges) {
        if (crossoverHz < r.below) return r.hint;
    }
    return "laptop / phone speakers";
}

}  // namespace sonar::dsp
