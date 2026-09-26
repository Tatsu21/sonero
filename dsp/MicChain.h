#pragma once

#include <array>
#include <string_view>
#include <vector>

#include "dsp/Equalizer.h"

namespace sonar::dsp {

// The microphone processing chain — the part that turns "a microphone" into "a
// voice". Pure DSP: no Qt, no PipeWire, no allocation while running, so it can be
// unit-tested against known signals and dropped into a realtime callback.
//
// The order is not a preference, it is the order every broadcast chain uses, and
// each step depends on the one before it:
//
//   input gain -> high-pass -> noise suppression -> gate -> de-esser
//               -> equalizer -> compressor -> limiter -> output gain
//
// High-pass first, because rumble and plosives otherwise drive every detector
// after it — a compressor that ducks the whole voice on a door slam is a
// compressor reacting to 30 Hz nobody can hear. Noise suppression before the
// gate, so the gate measures speech against a floor that has already been
// cleaned. Compressor after the equalizer, so it reacts to the voice as it will
// actually be heard. Limiter last, because its only job is that nothing leaves
// above the ceiling, and anything placed after it would break that promise.
//
// Noise suppression is deliberately *not* in this file: it is a learned model
// (RNNoise, DeepFilterNet) rather than arithmetic on a sample, and it lives
// behind its own interface. This chain leaves a slot for it.

// --- Per-stage settings -------------------------------------------------------

struct HighPassSettings {
    bool enabled = true;
    float frequencyHz = 80.0f;  // 12 dB/octave; below the lowest male fundamental
};

// A downward expander rather than a hard on/off switch: a hard gate chops word
// endings and breathes audibly between sentences. Below the threshold the signal
// is attenuated in proportion, so the room does not vanish and reappear.
struct GateSettings {
    bool enabled = true;
    float thresholdDb = -45.0f;
    float ratio = 4.0f;        // 4:1 downward expansion below the threshold
    float attackMs = 1.0f;     // open fast: a slow gate eats the first consonant
    float holdMs = 120.0f;     // stay open across the gaps inside a sentence
    float releaseMs = 200.0f;
    float hysteresisDb = 3.0f;  // close lower than it opens, or it chatters
};

// Sibilance control: a compressor that only listens to the "ess" band, so it
// ducks the whole signal for a few milliseconds when an S arrives instead of
// permanently dulling the voice the way an EQ cut would.
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

// Brick wall. Not a sound-shaping tool: it exists so that no combination of
// settings can send a sample past the ceiling, whatever the user does with the
// gain and the equalizer above it.
struct LimiterSettings {
    bool enabled = true;
    float ceilingDb = -1.0f;
    float releaseMs = 50.0f;
};

struct MicSettings {
    bool muted = false;
    float inputGainDb = 0.0f;   // trim, before everything
    float outputGainDb = 0.0f;  // after the limiter, kept small on purpose

    // The learned noise suppression that runs ahead of this chain. It lives here
    // rather than beside it so the page, the settings file and the audio thread
    // all read one struct — the stage is not implemented here (see
    // dsp/NoiseSuppressor), only switched on and off.
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
//
// Every number a meter needs, sampled at the end of each processed block. The
// reductions are negative decibels: how much that stage is holding the signal
// down right now.
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

    // The voice equalizer, sitting between the de-esser and the compressor. It
    // is kept separate from MicSettings because it is the same EqSettings the
    // rest of the app already stores, draws and imports presets into — the
    // microphone should not grow a second, incompatible idea of an equalizer.
    void setEqualizer(const EqSettings& eq);
    [[nodiscard]] const EqSettings& equalizer() const { return eq_; }

    // Process `count` mono samples in place. Allocation-free and branch-stable:
    // safe to call from a realtime thread.
    void process(float* samples, int count);

    // Readings from the most recently processed block.
    [[nodiscard]] MicMeters meters() const { return meters_; }

    // Drop every filter and envelope state without touching the settings. Called
    // when the input device changes, so the previous room's envelope does not
    // bleed into the new one.
    void reset();

    [[nodiscard]] float sampleRate() const { return sampleRate_; }

    // Samples of delay the chain adds, all of it the limiter's lookahead. The
    // caller needs it to keep monitoring in sync with what is being recorded.
    [[nodiscard]] int latencySamples() const;

private:
    // Direct-form transposed II: fewer state variables than DF1 and better
    // behaved with float coefficients at the low frequencies this chain uses.
    struct Biquad {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;

        [[nodiscard]] float process(float x);
        void reset();
    };

    void rebuildCoefficients();
    void rebuildEqualizer();

    float sampleRate_ = 48000.0f;
    MicSettings settings_;
    MicMeters meters_;

    // Two cascaded one-pole-pair sections make the 12 dB/octave slope; a single
    // biquad is enough, the second is what keeps the response flat in the pass
    // band at 48 kHz rather than drooping into the voice.
    Biquad highPass_;
    Biquad deEsserBand_;  // band-pass feeding the de-esser's detector only

    // One peaking filter per band, at the same ISO frequencies every other
    // equalizer in Sonero uses. Only the bands that are actually boosting or
    // cutting are run, so a flat equalizer costs nothing.
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

    // Lookahead delay line for the limiter, plus the running peak over it.
    std::vector<float> lookahead_;
    int lookaheadWrite_ = 0;
    int lookaheadSamples_ = 0;
    float limiterGain_ = 1.0f;

    // Coefficients derived from the settings and the sample rate.
    float gateAttackCoef_ = 0.0f;
    float gateReleaseCoef_ = 0.0f;
    int gateHoldSamples_ = 0;
    float deEsserAttackCoef_ = 0.0f;
    float deEsserReleaseCoef_ = 0.0f;
    float compAttackCoef_ = 0.0f;
    float compReleaseCoef_ = 0.0f;
    float limiterReleaseCoef_ = 0.0f;
};

}  // namespace sonar::dsp
