#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sonar::dsp {

// A second-order section and RBJ cookbook coefficients. MicChain computes its own
// coefficients in place, so a band keeps its state when only its gain moves.

// Direct-form transposed II: fewer state variables than DF1 and better behaved
// with float coefficients at the low frequencies a bass stage works in.
struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        // Flush denormals: they cost hundreds of cycles each in a filter fed silence.
        if (!std::isfinite(z1) || std::abs(z1) < 1e-25f) z1 = 0.0f;
        if (!std::isfinite(z2) || std::abs(z2) < 1e-25f) z2 = 0.0f;
        return y;
    }

    void reset() {
        z1 = 0.0f;
        z2 = 0.0f;
    }

    // No state left: fed silence, it would output exactly silence.
    [[nodiscard]] bool atRest() const { return z1 == 0.0f && z2 == 0.0f; }

    void setPassthrough() {
        b0 = 1.0f;
        b1 = b2 = a1 = a2 = 0.0f;
    }
};

// Butterworth Q: two cascaded sections make a Linkwitz-Riley crossover whose halves
// sum back flat, so the bass stage can replace the lows without a hole.
inline constexpr float kButterworthQ = 0.70710678f;

// Keeps a corner frequency inside the range a biquad can actually express at
// this sample rate. Above about 0.45 * Nyquist the cookbook formulas warp badly.
[[nodiscard]] inline float safeCorner(float freqHz, float sampleRate) {
    return std::clamp(freqHz, 10.0f, sampleRate * 0.45f);
}

// The intermediates every RBJ cookbook shape starts from.
struct RbjTerms {
    float cosw;
    float alpha;
};

[[nodiscard]] inline RbjTerms rbjTerms(float freqHz, float q, float sampleRate) {
    const float f = safeCorner(freqHz, sampleRate);
    const float w0 = 2.0f * std::numbers::pi_v<float> * f / sampleRate;
    return {std::cos(w0), std::sin(w0) / (2.0f * q)};
}

[[nodiscard]] inline Biquad lowPass(float freqHz, float q, float sampleRate) {
    const auto [cosw, alpha] = rbjTerms(freqHz, q, sampleRate);
    const float a0 = 1.0f + alpha;
    Biquad bq;
    bq.b0 = (1.0f - cosw) * 0.5f / a0;
    bq.b1 = (1.0f - cosw) / a0;
    bq.b2 = bq.b0;
    bq.a1 = -2.0f * cosw / a0;
    bq.a2 = (1.0f - alpha) / a0;
    return bq;
}

[[nodiscard]] inline Biquad highPass(float freqHz, float q, float sampleRate) {
    const auto [cosw, alpha] = rbjTerms(freqHz, q, sampleRate);
    const float a0 = 1.0f + alpha;
    Biquad bq;
    bq.b0 = (1.0f + cosw) * 0.5f / a0;
    bq.b1 = -(1.0f + cosw) / a0;
    bq.b2 = bq.b0;
    bq.a1 = -2.0f * cosw / a0;
    bq.a2 = (1.0f - alpha) / a0;
    return bq;
}

// Peaking (a graphic EQ band). `gainDb` is unbounded; the formula stays stable far
// past +-12 dB.
[[nodiscard]] inline Biquad peaking(float freqHz, float q, float gainDb,
                                    float sampleRate) {
    const auto [cosw, alpha] = rbjTerms(freqHz, q, sampleRate);
    const float A = std::pow(10.0f, gainDb / 40.0f);
    const float a0 = 1.0f + alpha / A;
    Biquad bq;
    bq.b0 = (1.0f + alpha * A) / a0;
    bq.b1 = -2.0f * cosw / a0;
    bq.b2 = (1.0f - alpha * A) / a0;
    bq.a1 = bq.b1;
    bq.a2 = (1.0f - alpha / A) / a0;
    return bq;
}

}  // namespace sonar::dsp
