#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sonar::dsp {

// A second-order section and the RBJ cookbook coefficients for the shapes the
// playback chains need. Pure header, no state beyond the filter's own.
//
// MicChain carries its own private copy of this struct. It is deliberately not
// refactored onto this one: that code runs on the microphone's realtime thread
// in every shipped build, and sharing a type with it buys nothing worth the risk
// of touching it. New chains use this header so they do not each grow a copy.

// Direct-form transposed II: fewer state variables than DF1 and better behaved
// with float coefficients at the low frequencies a bass stage works in.
struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        // Denormals cost hundreds of cycles each on x86, and they only ever turn
        // up in a filter that has been fed silence for a while — which is most of
        // a bass stage's life, since it sees a band that is often empty.
        if (!std::isfinite(z1) || std::abs(z1) < 1e-25f) z1 = 0.0f;
        if (!std::isfinite(z2) || std::abs(z2) < 1e-25f) z2 = 0.0f;
        return y;
    }

    void reset() {
        z1 = 0.0f;
        z2 = 0.0f;
    }

    void setPassthrough() {
        b0 = 1.0f;
        b1 = b2 = a1 = a2 = 0.0f;
    }
};

// Butterworth Q. Two sections cascaded at this Q make a Linkwitz-Riley 4th-order
// crossover, whose low and high halves sum back to a flat magnitude — which is
// what lets the bass stage subtract the lows and put something else there
// without leaving a hole at the crossover.
inline constexpr float kButterworthQ = 0.70710678f;

// Keeps a corner frequency inside the range a biquad can actually express at
// this sample rate. Above about 0.45 * Nyquist the cookbook formulas warp badly.
[[nodiscard]] inline float safeCorner(float freqHz, float sampleRate) {
    return std::clamp(freqHz, 10.0f, sampleRate * 0.45f);
}

[[nodiscard]] inline Biquad lowPass(float freqHz, float q, float sampleRate) {
    const float f = safeCorner(freqHz, sampleRate);
    const float w0 = 2.0f * std::numbers::pi_v<float> * f / sampleRate;
    const float cosw = std::cos(w0);
    const float alpha = std::sin(w0) / (2.0f * q);
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
    const float f = safeCorner(freqHz, sampleRate);
    const float w0 = 2.0f * std::numbers::pi_v<float> * f / sampleRate;
    const float cosw = std::cos(w0);
    const float alpha = std::sin(w0) / (2.0f * q);
    const float a0 = 1.0f + alpha;
    Biquad bq;
    bq.b0 = (1.0f + cosw) * 0.5f / a0;
    bq.b1 = -(1.0f + cosw) / a0;
    bq.b2 = bq.b0;
    bq.a1 = -2.0f * cosw / a0;
    bq.a2 = (1.0f - alpha) / a0;
    return bq;
}

// Peaking, the shape a graphic equalizer band is. `gainDb` is unbounded here on
// purpose: the Lab chain's whole point is an equalizer with no gain ceiling, and
// the cookbook formula stays stable far beyond the +-12 dB the UI used to allow.
[[nodiscard]] inline Biquad peaking(float freqHz, float q, float gainDb,
                                    float sampleRate) {
    const float f = safeCorner(freqHz, sampleRate);
    const float A = std::pow(10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * std::numbers::pi_v<float> * f / sampleRate;
    const float cosw = std::cos(w0);
    const float alpha = std::sin(w0) / (2.0f * q);
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
