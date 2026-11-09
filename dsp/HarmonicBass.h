#pragma once

#include <string_view>

#include "dsp/Biquad.h"

namespace sonar::dsp {

// Harmonic bass synthesis. A small driver cannot reproduce 40 Hz, but given its
// harmonics the ear reconstructs the missing fundamental — so synthesise those
// where the driver is efficient.
//   in -> lowpass(fc) -> envelope ------------------------------+
//            |                                                 |
//            +-> normalise -> Chebyshev -> x -> bandpass -------+-> + -> out
//                                                                  |
//   in --------------[ optional highpass(fc) ]---------------------+
// - Chebyshev polynomials: T_n(cos x) = cos(n x), so each order is exactly its
//   harmonic, not a waveshaper's mush.
// - Normalised by the band's envelope before shaping and rescaled after, so the
//   harmonic content does not change with loudness.
// - Band-limited to fc .. harmonics*fc, removing the even orders' DC term.
// Mono, one instance per channel side.

struct HarmonicBassSettings {
    bool enabled = false;

    // The crossover, the setting that matters most: where the transducer stops
    // delivering. ~80-100 Hz headphones, 150-200 Hz laptop speakers, 40-50 Hz with a sub.
    float crossoverHz = 90.0f;

    // Harmonic level relative to the low band; past ~+12 dB they read as their own pitch.
    float amountDb = 6.0f;

    // Orders above the fundamental, 2..kMaxHarmonics: fewer is cleaner, more is louder
    // and buzzier.
    int harmonics = 4;

    // Weight of even orders (fuller) against odd (growl, stronger pitch): 0 odd-only,
    // 1 even-only. Not a level: the series is renormalised.
    float evenWeight = 0.5f;

    // Envelope times on the low band: attack to catch a kick, release for tight vs smeared.
    float attackMs = 5.0f;
    float releaseMs = 60.0f;

    // Filter the lows out of the original ("replace bass" rather than "add bass"), so
    // the headroom goes to the audible harmonics. Leave off on anything with real lows.
    bool replaceLows = false;
};

// What the UI draws. Decibels, sampled at the end of each processed block.
struct HarmonicBassMeters {
    float lowBandDb = -120.0f;    // level in the band below the crossover
    float harmonicsDb = -120.0f;  // level of the synthesised harmonics
    bool active = false;          // there is bass down there to work with
};

// Allocation-free after prepare(); safe to call process() from a realtime thread.
class HarmonicBass {
public:
    static constexpr int kMinHarmonics = 2;
    static constexpr int kMaxHarmonics = 6;

    HarmonicBass();

    // Sample rate in Hz. Recomputes every coefficient and clears the state, so
    // it is a graph change rather than something to call per block.
    void prepare(float sampleRate);

    void setSettings(const HarmonicBassSettings& settings);
    [[nodiscard]] const HarmonicBassSettings& settings() const { return s_; }

    // `samples` holds `count` mono samples, processed in place. Disabled settings
    // return immediately, leaving the signal untouched.
    void process(float* samples, int count);

    // The same, out of place; the in-place overload is this with equal pointers.
    void process(const float* in, float* out, int count);

    void reset();

    [[nodiscard]] HarmonicBassMeters meters() const { return meters_; }
    [[nodiscard]] float sampleRate() const { return sampleRate_; }

private:
    void rebuild();

    float sampleRate_ = 48000.0f;
    HarmonicBassSettings s_;
    HarmonicBassMeters meters_;

    // Crossover: Linkwitz-Riley 4th order (two Butterworth sections), summing back flat.
    Biquad lowSplit_[2];
    Biquad highSplit_[2];  // only used by replaceLows

    // Band limit on the harmonics, Linkwitz-Riley like the crossover: above fc (kills
    // the DC) and below the last harmonic.
    Biquad harmHigh_[2];
    Biquad harmLow_[2];

    float env_ = 0.0f;
    float attackCoef_ = 0.0f;
    float releaseCoef_ = 0.0f;

    // Per-order weights normalised to unit amplitude; index n is the n-th harmonic.
    float weight_[kMaxHarmonics + 1] = {};
};

[[nodiscard]] std::string_view harmonicBassCrossoverHint(float crossoverHz);

}  // namespace sonar::dsp
