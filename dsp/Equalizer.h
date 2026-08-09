#pragma once

#include <string_view>
#include <vector>

namespace sonar::dsp {

// A graphic equalizer: a set of bands at fixed ISO frequencies, each with an
// adjustable gain. This is the pure model — no Qt, no audio backend — so it can
// be unit-tested and later fed into a PipeWire filter-chain.

enum class BandCount : int { Bands10 = 10, Bands15 = 15, Bands31 = 31 };

enum class EqPreset {
    Flat,
    BassBoost,
    BassReducer,
    TrebleBoost,
    TrebleReducer,
    Loudness,
    Voice,
    Podcast,
    Music,
    Rock,
    Pop,
    Electronic,
    Classical,
    Warm,
    Bright,
    Jazz,
    HipHop,
    Acoustic,
    RnB,
    Dance,
    Metal,
    FPS,
    Movie,
    Custom
};

struct EqBand {
    float frequency = 1000.0f;  // Hz, fixed per band
    float gainDb = 0.0f;        // clamped to [kMinGainDb, kMaxGainDb]
};

inline constexpr float kMinGainDb = -12.0f;
inline constexpr float kMaxGainDb = 12.0f;
inline constexpr float kMinFreq = 20.0f;
inline constexpr float kMaxFreq = 20000.0f;

struct EqSettings {
    bool enabled = false;
    BandCount bandCount = BandCount::Bands10;
    EqPreset preset = EqPreset::Flat;
    std::vector<EqBand> bands;  // size() == static_cast<int>(bandCount)
};

// Standard ISO centre frequencies for each band configuration.
std::vector<float> standardFrequencies(BandCount count);

// Rebuild `bands` for the requested count, keeping every gain at 0 dB.
void resetBands(EqSettings& settings, BandCount count);

// Fill the band gains from a preset (Custom leaves them untouched).
void applyPreset(EqSettings& settings, EqPreset preset);

[[nodiscard]] std::string_view presetName(EqPreset preset);
[[nodiscard]] std::string_view bandCountName(BandCount count);

// Combined magnitude response (dB) at a frequency, approximated as a sum of
// per-band bell curves — good enough for the on-screen curve and stable to draw.
// Combined response of the whole curve at a frequency: the sum of every band's
// contribution. This is what the curve widget draws — it is NOT the gain to give
// an individual filter, because feeding a summed curve into each filter of a
// cascade applies the overlap between bands a second time.
[[nodiscard]] float responseDbAt(const EqSettings& settings, float freqHz);

// Gain a single filter centred at `freqHz` should apply, interpolated between the
// user's band gains in log-frequency space. Use this — not responseDbAt — when
// driving a cascade of filters, so bands are not counted twice.
[[nodiscard]] float bandGainAt(const EqSettings& settings, float freqHz);

// Peak boost (dB, never negative) the settings produce anywhere in the audible
// range. Used to attenuate the signal by the same amount so that boosting an EQ
// band cannot push the output past full scale, where it would clip on conversion
// to the device's sample format.
[[nodiscard]] float peakBoostDb(const EqSettings& settings);

}  // namespace sonar::dsp
