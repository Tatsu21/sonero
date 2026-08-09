#include "dsp/Equalizer.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sonar::dsp {

namespace {

const std::vector<float> kFreq10 = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};

const std::vector<float> kFreq15 = {25,  40,  63,  100,  160,  250,  400,  630,
                                    1000, 1600, 2500, 4000, 6300, 10000, 16000};

const std::vector<float> kFreq31 = {
    20,   25,   31,   40,   50,   63,   80,   100,  125,  160,  200,
    250,  315,  400,  500,  630,  800,  1000, 1250, 1600, 2000, 2500,
    3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000};

float gaussian(float x, float mu, float sigma) {
    const float d = (x - mu) / sigma;
    return std::exp(-0.5f * d * d);
}

// Professional preset gains (dB) at the 10 ISO frequencies of kFreq10:
//   31  62  125 250 500  1k  2k  4k  8k  16k
const std::array<float, 10>& presetAnchors(EqPreset preset) {
    static const std::array<float, 10> kFlat         = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    static const std::array<float, 10> kBassBoost    = {7, 6, 5, 3, 1, 0, 0, 0, 0, 0};
    static const std::array<float, 10> kBassReducer  = {-6, -5, -4, -2, -1, 0, 0, 0, 0, 0};
    static const std::array<float, 10> kTrebleBoost  = {0, 0, 0, 0, 0, 1, 2, 4, 5, 6};
    static const std::array<float, 10> kTrebleReduc  = {0, 0, 0, 0, 0, -1, -2, -4, -5, -6};
    static const std::array<float, 10> kLoudness     = {6, 5, 3, 0, -1, -1, 0, 2, 4, 6};
    static const std::array<float, 10> kVoice        = {-2, -2, -1, 1, 3, 4, 4, 3, 1, -1};
    static const std::array<float, 10> kPodcast      = {-3, -2, 0, 2, 3, 3, 2, 1, -1, -3};
    static const std::array<float, 10> kMusic        = {3, 2, 1, 0, 0, 0, 1, 2, 3, 3};
    static const std::array<float, 10> kRock         = {5, 4, 3, 1, -1, -1, 1, 3, 4, 5};
    static const std::array<float, 10> kPop          = {-1, 0, 1, 3, 4, 4, 2, 0, -1, -1};
    static const std::array<float, 10> kElectronic   = {6, 5, 2, 0, -2, 0, 1, 3, 4, 5};
    static const std::array<float, 10> kClassical    = {4, 3, 2, 1, -1, -1, 0, 2, 3, 4};
    static const std::array<float, 10> kWarm         = {3, 4, 3, 2, 1, 0, -1, -2, -2, -1};
    static const std::array<float, 10> kBright       = {-2, -2, -1, 0, 0, 1, 3, 4, 5, 5};
    static const std::array<float, 10> kJazz         = {3, 3, 1, 2, -1, -1, 0, 1, 3, 4};
    static const std::array<float, 10> kHipHop       = {5, 5, 4, 2, -1, -1, 1, 2, 2, 3};
    static const std::array<float, 10> kAcoustic     = {4, 3, 2, 0, 1, 1, 2, 3, 2, 2};
    static const std::array<float, 10> kRnB          = {5, 4, 2, 0, 2, 3, 1, 2, 3, 4};
    static const std::array<float, 10> kDance        = {6, 5, 2, -1, -2, 0, 2, 3, 4, 3};
    static const std::array<float, 10> kMetal        = {4, 4, 3, 0, -2, 1, 2, 3, 3, 4};
    static const std::array<float, 10> kFPS          = {-4, -4, -2, 0, 1, 2, 5, 5, 3, 2};
    static const std::array<float, 10> kMovie        = {5, 4, 2, 0, -1, 0, 1, 2, 3, 4};
    switch (preset) {
        case EqPreset::Flat:
        case EqPreset::Custom:        return kFlat;
        case EqPreset::BassBoost:     return kBassBoost;
        case EqPreset::BassReducer:   return kBassReducer;
        case EqPreset::TrebleBoost:   return kTrebleBoost;
        case EqPreset::TrebleReducer: return kTrebleReduc;
        case EqPreset::Loudness:      return kLoudness;
        case EqPreset::Voice:         return kVoice;
        case EqPreset::Podcast:       return kPodcast;
        case EqPreset::Music:         return kMusic;
        case EqPreset::Rock:          return kRock;
        case EqPreset::Pop:           return kPop;
        case EqPreset::Electronic:    return kElectronic;
        case EqPreset::Classical:     return kClassical;
        case EqPreset::Warm:          return kWarm;
        case EqPreset::Bright:        return kBright;
        case EqPreset::Jazz:          return kJazz;
        case EqPreset::HipHop:        return kHipHop;
        case EqPreset::Acoustic:      return kAcoustic;
        case EqPreset::RnB:           return kRnB;
        case EqPreset::Dance:         return kDance;
        case EqPreset::Metal:         return kMetal;
        case EqPreset::FPS:           return kFPS;
        case EqPreset::Movie:         return kMovie;
    }
    return kFlat;
}

// Preset gain (dB) at a frequency: log-frequency linear interpolation of the 10
// anchor points — precise and smooth for any band count.
float presetGain(EqPreset preset, float freq) {
    const std::array<float, 10>& a = presetAnchors(preset);
    const std::vector<float>& f = kFreq10;
    if (freq <= f.front()) return a.front();
    if (freq >= f.back()) return a.back();
    const float lf = std::log10(freq);
    for (std::size_t i = 0; i + 1 < f.size(); ++i) {
        if (freq <= f[i + 1]) {
            const float t = (lf - std::log10(f[i])) / (std::log10(f[i + 1]) - std::log10(f[i]));
            return a[i] + t * (a[i + 1] - a[i]);
        }
    }
    return a.back();
}

}  // namespace

std::vector<float> standardFrequencies(BandCount count) {
    switch (count) {
        case BandCount::Bands10: return kFreq10;
        case BandCount::Bands15: return kFreq15;
        case BandCount::Bands31: return kFreq31;
    }
    return kFreq10;
}

void resetBands(EqSettings& settings, BandCount count) {
    const std::vector<float> freqs = standardFrequencies(count);
    settings.bandCount = count;
    settings.bands.clear();
    settings.bands.reserve(freqs.size());
    for (const float f : freqs) {
        settings.bands.push_back(EqBand{f, 0.0f});
    }
}

void applyPreset(EqSettings& settings, EqPreset preset) {
    settings.preset = preset;
    if (preset == EqPreset::Custom) {
        return;
    }
    if (settings.bands.empty()) {
        resetBands(settings, settings.bandCount);
    }
    for (EqBand& band : settings.bands) {
        band.gainDb = std::clamp(presetGain(preset, band.frequency), kMinGainDb, kMaxGainDb);
    }
}

std::string_view presetName(EqPreset preset) {
    switch (preset) {
        case EqPreset::Flat:          return "Flat";
        case EqPreset::BassBoost:     return "Bass Boost";
        case EqPreset::BassReducer:   return "Bass Reducer";
        case EqPreset::TrebleBoost:   return "Treble Boost";
        case EqPreset::TrebleReducer: return "Treble Reducer";
        case EqPreset::Loudness:      return "Loudness";
        case EqPreset::Voice:         return "Voice";
        case EqPreset::Podcast:       return "Podcast";
        case EqPreset::Music:         return "Music";
        case EqPreset::Rock:          return "Rock";
        case EqPreset::Pop:           return "Pop";
        case EqPreset::Electronic:    return "Electronic";
        case EqPreset::Classical:     return "Classical";
        case EqPreset::Warm:          return "Warm";
        case EqPreset::Bright:        return "Bright";
        case EqPreset::Jazz:          return "Jazz";
        case EqPreset::HipHop:        return "Hip-Hop";
        case EqPreset::Acoustic:      return "Acoustic";
        case EqPreset::RnB:           return "R&B";
        case EqPreset::Dance:         return "Dance";
        case EqPreset::Metal:         return "Metal";
        case EqPreset::FPS:           return "FPS";
        case EqPreset::Movie:         return "Movie";
        case EqPreset::Custom:        return "Custom";
    }
    return "Custom";
}

std::string_view bandCountName(BandCount count) {
    switch (count) {
        case BandCount::Bands10: return "10 Bands";
        case BandCount::Bands15: return "15 Bands";
        case BandCount::Bands31: return "31 Bands";
    }
    return "10 Bands";
}

float bandGainAt(const EqSettings& settings, float freqHz) {
    if (settings.bands.empty()) {
        return 0.0f;
    }
    // The filter cascade has its own fixed centre frequencies, which need not
    // match the user's band count (10 / 15 / 31). Interpolate between the two
    // neighbouring band gains rather than summing every band's bell: summing is
    // for drawing the curve, and would double-count the overlap here.
    if (freqHz <= settings.bands.front().frequency) {
        return settings.bands.front().gainDb;
    }
    if (freqHz >= settings.bands.back().frequency) {
        return settings.bands.back().gainDb;
    }
    const float lf = std::log10(freqHz);
    for (std::size_t i = 0; i + 1 < settings.bands.size(); ++i) {
        const EqBand& lo = settings.bands[i];
        const EqBand& hi = settings.bands[i + 1];
        if (freqHz <= hi.frequency) {
            const float l0 = std::log10(lo.frequency);
            const float l1 = std::log10(hi.frequency);
            const float t = l1 > l0 ? (lf - l0) / (l1 - l0) : 0.0f;
            return lo.gainDb + t * (hi.gainDb - lo.gainDb);
        }
    }
    return settings.bands.back().gainDb;
}

float peakBoostDb(const EqSettings& settings) {
    if (!settings.enabled || settings.bands.empty()) {
        return 0.0f;
    }
    // Sweep the audible range: neighbouring boosted bands overlap, so the peak of
    // the combined curve sits above the highest single band gain.
    float peak = 0.0f;
    constexpr int kSteps = 240;
    const float lo = std::log10(kMinFreq);
    const float hi = std::log10(kMaxFreq);
    for (int i = 0; i <= kSteps; ++i) {
        const float f = std::pow(10.0f, lo + (hi - lo) * static_cast<float>(i) / kSteps);
        peak = std::max(peak, responseDbAt(settings, f));
    }
    return peak;
}

float responseDbAt(const EqSettings& settings, float freqHz) {
    if (settings.bands.empty()) {
        return 0.0f;
    }
    // Bell width scales with band density so the summed curve stays smooth.
    const float decades = std::log10(kMaxFreq) - std::log10(kMinFreq);
    const float sigma =
        decades / static_cast<float>(settings.bands.size() - 1 > 0
                                         ? settings.bands.size() - 1
                                         : 1) *
        0.55f;
    const float lf = std::log10(freqHz);

    float sum = 0.0f;
    for (const EqBand& band : settings.bands) {
        if (band.gainDb != 0.0f) {
            sum += band.gainDb * gaussian(lf, std::log10(band.frequency), sigma);
        }
    }
    return sum;
}

}  // namespace sonar::dsp
