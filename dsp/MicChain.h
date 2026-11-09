#pragma once

#include <array>
#include <string_view>
#include <vector>

#include "dsp/Biquad.h"
#include "dsp/Equalizer.h"
#include "dsp/Limiter.h"

namespace sonar::dsp {

// The microphone processing chain: pure DSP, no allocation while running.
//   input gain -> high-pass -> noise suppression -> gate -> de-esser
//               -> equalizer -> compressor -> output gain -> limiter
// High-pass first so rumble does not drive the detectors; noise suppression before
// the gate so it judges a cleaned floor; compressor after the EQ; limiter last.
// Noise suppression itself lives in dsp/NoiseSuppressor.

// --- Per-stage settings -------------------------------------------------------

struct HighPassSettings {
    bool enabled = true;
    float frequencyHz = 80.0f;  // 12 dB/octave; below the lowest male fundamental
};

// A downward expander, not a hard gate, so word endings and the room are not chopped.
struct GateSettings {
    bool enabled = true;
    float thresholdDb = -45.0f;
    float ratio = 4.0f;        // 4:1 downward expansion below the threshold
    float attackMs = 1.0f;     // open fast: a slow gate eats the first consonant
    float holdMs = 120.0f;     // stay open across the gaps inside a sentence
    float releaseMs = 200.0f;
    float hysteresisDb = 3.0f;  // close lower than it opens, or it chatters
};

// A compressor listening only to the "ess" band, ducking the voice briefly instead
// of dulling it with an EQ cut.
struct DeEsserSettings {
    bool enabled = true;
    float frequencyHz = 6500.0f;
    float thresholdDb = -28.0f;
    float ratio = 3.0f;
};

struct CompressorSettings {
    bool enabled = true;
    float thresholdDb = -18.0f;
    float ratio = 3.0f;
    float kneeDb = 6.0f;      // soft knee: the onset of compression is inaudible
    float attackMs = 10.0f;   // slow enough to let consonants through intact
    float releaseMs = 120.0f;
    bool autoMakeup = true;   // put back what the threshold and ratio took out
    float makeupDb = 0.0f;    // used when autoMakeup is off
};

// Brick wall: no setting can send a sample past the ceiling.
struct LimiterSettings {
    bool enabled = true;
    float ceilingDb = -1.0f;
    float releaseMs = 50.0f;
};

struct MicSettings {
    bool muted = false;
    float inputGainDb = 0.0f;   // trim, before everything
    float outputGainDb = 0.0f;  // before the limiter, which still has the last word

    // Switches the learned noise suppression (dsp/NoiseSuppressor) that runs ahead of
    // this chain; kept here so page, settings and audio thread share one struct.
    bool noiseSuppression = true;

    HighPassSettings highPass;
    GateSettings gate;
    DeEsserSettings deEsser;
    CompressorSettings compressor;
    LimiterSettings limiter;
};

// --- Presets ------------------------------------------------------------------

enum class MicPreset {
    Raw,        // everything off: the microphone as the device delivers it
    Podcast,    // close mic, quiet room, warm and even
    Streaming,  // louder, faster, survives a noisy room and a keyboard
    Meeting,    // intelligibility first, aggressive gate
    Custom      // whatever the user built
};

[[nodiscard]] MicSettings presetSettings(MicPreset preset);
[[nodiscard]] std::string_view micPresetName(MicPreset preset);

// --- What the UI draws --------------------------------------------------------
// Sampled per block. Reductions are negative dB: how far each stage holds the
// signal down right now.
struct MicMeters {
    float inputPeakDb = -120.0f;
    float outputPeakDb = -120.0f;
    float gateReductionDb = 0.0f;
    float deEsserReductionDb = 0.0f;
    float compressorReductionDb = 0.0f;
    float limiterReductionDb = 0.0f;
    bool gateOpen = false;
};

// --- The chain ----------------------------------------------------------------

class MicChain {
public:
    MicChain();

    // Sample rate in Hz. Recomputes every coefficient and clears the state, so it
    // is a graph change, not something to call per block.
    void prepare(float sampleRate);

    void setSettings(const MicSettings& settings);
    [[nodiscard]] const MicSettings& settings() const { return settings_; }

    // The voice equalizer, between de-esser and compressor. The app's ordinary
    // EqSettings, so it stores, draws and imports presets like every other.
    void setEqualizer(const EqSettings& eq);
    [[nodiscard]] const EqSettings& equalizer() const { return eq_; }

    // Process `count` mono samples in place. Allocation-free and branch-stable:
    // safe to call from a realtime thread.
    void process(float* samples, int count);

    // Readings from the most recently processed block.
    [[nodiscard]] MicMeters meters() const { return meters_; }

    // Drop filter and envelope state, keeping settings (on an input device change).
    void reset();

    [[nodiscard]] float sampleRate() const { return sampleRate_; }

    // Samples of delay the chain adds, all of it the limiter's lookahead. The
    // caller needs it to keep monitoring in sync with what is being recorded.
    [[nodiscard]] int latencySamples() const;

private:
    void rebuildCoefficients();
    void rebuildEqualizer();

    float sampleRate_ = 48000.0f;
    MicSettings settings_;
    MicMeters meters_;

    // The high-pass, and the band-pass feeding the de-esser's detector.
    Biquad highPass_;
    Biquad deEsserBand_;  // band-pass feeding the de-esser's detector only

    // One peaking filter per ISO band; only bands that boost or cut are run.
    EqSettings eq_;
    std::vector<Biquad> eqBands_;

    // Envelope state. All of it in linear gain except where noted.
    float gateEnv_ = 0.0f;
    float gateGain_ = 1.0f;
    int gateHoldCounter_ = 0;
    bool gateIsOpen_ = false;

    float deEsserEnv_ = 0.0f;
    float deEsserGain_ = 1.0f;

    float compEnv_ = 0.0f;
    float compGainDb_ = 0.0f;

    // The same lookahead limiter the output channels end in.
    Limiter limiter_;

    // Coefficients derived from the settings and the sample rate.
    float gateAttackCoef_ = 0.0f;
    float gateReleaseCoef_ = 0.0f;
    int gateHoldSamples_ = 0;
    float deEsserAttackCoef_ = 0.0f;
    float deEsserReleaseCoef_ = 0.0f;
    float compAttackCoef_ = 0.0f;
    float compReleaseCoef_ = 0.0f;
};

}  // namespace sonar::dsp
