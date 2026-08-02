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
[[nodiscard]] float responseDbAt(const EqSettings& settings, float freqHz);

}  // namespace sonar::dsp
