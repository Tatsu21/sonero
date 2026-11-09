#pragma once

#include <string_view>
#include <vector>

namespace sonar::dsp {

// A graphic equalizer: bands at fixed ISO frequencies with adjustable gains. Pure
// model, no Qt or audio backend.

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

// The same table without the copy, for the audio thread, where allocating can block.
[[nodiscard]] const std::vector<float>& standardFrequenciesRef(BandCount count);

// The Q that makes neighbouring bands meet without holes: it follows the spacing
// (an octave for 10 bands, a third for 31).
[[nodiscard]] float bandQ(BandCount count);

// Rebuild `bands` for the requested count, keeping every gain at 0 dB.
void resetBands(EqSettings& settings, BandCount count);

// Fill the band gains from a preset (Custom leaves them untouched).
void applyPreset(EqSettings& settings, EqPreset preset);

[[nodiscard]] std::string_view presetName(EqPreset preset);
[[nodiscard]] std::string_view bandCountName(BandCount count);

// Combined response at a frequency (the sum of every band's bell), for drawing —
// not for driving a filter cascade.
[[nodiscard]] float responseDbAt(const EqSettings& settings, float freqHz);

// The gain one filter at `freqHz` should apply, interpolated in log frequency. Use
// this, not responseDbAt, for a cascade.
[[nodiscard]] float bandGainAt(const EqSettings& settings, float freqHz);

// Peak boost (dB, >= 0) anywhere in the audible range, attenuated up front so a
// boosted band cannot clip on conversion to the device format.
[[nodiscard]] float peakBoostDb(const EqSettings& settings);

}  // namespace sonar::dsp
