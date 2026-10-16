#pragma once

#include <string_view>

#include "dsp/Biquad.h"

namespace sonar::dsp {

// Harmonic bass synthesis: the psychoacoustic trick behind every "bass
// enhancer" worth the name, including the ones sold under a brand.
//
// A small driver cannot move enough air to reproduce 40 Hz. Boosting 40 Hz with
// an equalizer therefore does not produce bass — it produces excursion, and with
// it distortion of everything else the driver is trying to play at the same time.
// The way out is that pitch is not a frequency. Given a series of harmonics at
// 80, 120, 160, 200 Hz the ear reconstructs the 40 Hz fundamental that is not
// there at all (the "missing fundamental", or virtual pitch). So instead of
// asking the driver for the fundamental, we synthesise its harmonics in the range
// where the driver is efficient and let the listener's hearing supply the rest.
//
// The chain, per sample:
//
//   in -> lowpass(fc) -> envelope ------------------------------+
//            |                                                 |
//            +-> normalise -> Chebyshev -> x -> bandpass -------+-> + -> out
//                                                                  |
//   in --------------[ optional highpass(fc) ]---------------------+
//
// Three decisions in there are what separate this from a distortion pedal:
//
//  - The harmonics come from Chebyshev polynomials of the first kind, not from
//    clipping or tanh. T_n(cos x) = cos(n x) is an identity, so T_n fed a
//    sinusoid produces *exactly* its n-th harmonic. That is per-order control
//    over what is generated instead of a waveshaper's uncontrolled mush.
//  - The low band is normalised by its own envelope before shaping and
//    multiplied by that envelope afterwards. Chebyshev polynomials are only
//    harmonic generators on [-1, 1]; without the normalisation the harmonic
//    content would change with how loud the passage is, which is the single
//    thing that makes an enhancer sound like an effect rather than like bass.
//  - The harmonics are band-limited to fc .. harmonics*fc. The even orders carry
//    a DC term (T2(u) = 2u^2 - 1), and nothing above the last requested harmonic
//    is wanted; the bandpass removes both.
//
// MONO, one instance per channel; dsp::ChannelChain runs two of them, as it runs
// two of everything else. Deriving the low band from the stereo sum instead
// would be marginally better — bass below ~120 Hz carries almost no directional
// information, so one shared envelope could not drift between the sides — but it
// would make this the one stage in the chain that is not per-channel, for a
// difference that is inaudible on material where the two sides' low end is
// nearly identical anyway, which is most of it.

struct HarmonicBassSettings {
    bool enabled = false;

    // The crossover, and the most important setting here: it should sit at the
    // frequency where the transducer stops delivering. Below it the driver is
    // wasting excursion, above it the harmonics live. 80-100 Hz suits most
    // headphones, 150-200 Hz small laptop or phone speakers, 40-50 Hz a system
    // that has a real subwoofer (where this whole stage has little to do).
    float crossoverHz = 90.0f;

    // Level of the synthesised harmonics, relative to the low band they came
    // from. Past about +12 dB the harmonics stop reading as the fundamental and
    // start reading as their own pitch — the point where it becomes an effect.
    float amountDb = 6.0f;

    // How many orders above the fundamental to generate, 2..kMaxHarmonics. Fewer
    // is cleaner and more "fundamental"; more is louder and more present, at the
    // cost of sounding buzzy on sustained bass notes.
    int harmonics = 4;

    // Relative weight of the even orders (2nd, 4th, 6th). Even harmonics sit an
    // octave and a major third above and read as "fuller"; odd orders (3rd, 5th)
    // read as "growl" and carry the pitch more strongly. 0 is odd-only, 1 is
    // even-only. Not a level: the series is renormalised either way.
    float evenWeight = 0.5f;

    // Envelope follower time constants on the low band. The attack has to be
    // quick enough to put the harmonics under the front of a kick drum; the
    // release decides whether a bass line sounds tight or smeared.
    float attackMs = 5.0f;
    float releaseMs = 60.0f;

    // Take the lows out of the original instead of leaving them in. This is what
    // turns the stage from "add bass" into "replace bass": the driver stops being
    // asked for what it cannot produce, so the headroom goes to the harmonics
    // that are actually audible. On anything with a real low end, leave it off.
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

    // The same, writing to a separate output buffer, for callers that cannot
    // process in place. Kept because it is the honest shape of the operation —
    // the in-place overload is this one with both pointers equal — and because
    // it is what makes the out-of-place path testable at all.
    void process(const float* in, float* out, int count);

    void reset();

    [[nodiscard]] HarmonicBassMeters meters() const { return meters_; }
    [[nodiscard]] float sampleRate() const { return sampleRate_; }

private:
    void rebuild();

    float sampleRate_ = 48000.0f;
    HarmonicBassSettings s_;
    HarmonicBassMeters meters_;

    // Crossover. Two Butterworth sections each, so low and high are
    // Linkwitz-Riley 4th order and sum back flat — the lows can be removed and
    // the harmonics put in their place without a dip at the crossover.
    Biquad lowSplit_[2];
    Biquad highSplit_[2];  // only used by replaceLows

    // Band limit on the synthesised harmonics: above the crossover (kills the
    // even orders' DC) and below the last harmonic (kills what nobody asked
    // for). Linkwitz-Riley on both sides, matching the crossover, so the
    // harmonics occupy exactly the band the lows were taken out of.
    Biquad harmHigh_[2];
    Biquad harmLow_[2];

    float env_ = 0.0f;
    float attackCoef_ = 0.0f;
    float releaseCoef_ = 0.0f;

    // Per-order weights, normalised so that the sum of what the shaper emits has
    // unit amplitude whatever the harmonic count and balance are. Index n holds
    // the weight of the n-th harmonic; 0 and 1 are unused.
    float weight_[kMaxHarmonics + 1] = {};
};

[[nodiscard]] std::string_view harmonicBassCrossoverHint(float crossoverHz);

}  // namespace sonar::dsp
