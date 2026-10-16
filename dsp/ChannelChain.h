#pragma once

#include <vector>

#include "dsp/BassStage.h"
#include "dsp/Biquad.h"
#include "dsp/Equalizer.h"
#include "dsp/HarmonicBass.h"
#include "dsp/Limiter.h"

namespace sonar::dsp {

// Everything one output channel does to audio, in one place.
//
//   trim -> equalizer -> harmonic bass -> limiter -> fader (volume, balance, mute)
//
// This replaces what used to be split across three mechanisms: a PipeWire
// module-filter-chain for the equalizer, a LADSPA plugin dlopened into it for the
// bass, and the sink node's own channelVolumes for the fader. One chain in our
// own code is simpler than three, and it removes the parts that were awkward
// about the old arrangement — a plugin binary to find and install, an ABI header
// borrowed from another project, and a round trip through node properties to set
// a slider.
//
// The order is not a preference. The equalizer comes first because the bass
// stage's crossover should see the signal as it will be heard. The limiter comes
// after the bass because it is the only thing standing between what those two
// produce and a driver. The fader comes last, so pulling a channel down never
// changes what the limiter was protecting against — a channel at 10% is the same
// sound as one at 100%, quieter, rather than a differently-limited one.
//
// Stereo, interleaved. Allocation-free after prepare(), so process() is safe on
// a realtime thread.

struct ChannelChainSettings {
    // The Channels page's gain slider: a trim for a source that is mixed too
    // loud or too quiet, ahead of everything else.
    float trimDb = 0.0f;

    // The mixer's fader, its balance and its mute.
    float volume = 1.0f;    // linear, 0..1
    float balance = 0.0f;   // -1 full left .. +1 full right
    bool muted = false;
};

// What the mixer's meters and the Channels page draw, sampled at the end of each
// processed block. Peaks are linear and are what left the chain — post-fader, so
// a channel pulled down reads quiet, which is what someone looking at a mixer
// expects.
struct ChannelMeters {
    float peakLeft = 0.0f;
    float peakRight = 0.0f;
    float inputPeak = 0.0f;         // before anything, for spotting a hot source
    float limiterReductionDb = 0.0f;
    int clippedSamples = 0;         // only possible with the limiter switched off
    HarmonicBassMeters bass;
};

class ChannelChain {
public:
    ChannelChain();

    void prepare(float sampleRate);

    void setSettings(const ChannelChainSettings& settings);
    [[nodiscard]] const ChannelChainSettings& settings() const { return s_; }

    // The channel's graphic equalizer. Band gains are clamped to
    // [kMinGainDb, kMaxGainDb] and the curve's peak boost is compensated for by
    // an equal attenuation ahead of the filters — a boosting cascade multiplies
    // material already mastered near full scale, and without this a +6 dB band
    // clips on the way out to the device's integer format.
    void setEqualizer(const EqSettings& eq);
    [[nodiscard]] const EqSettings& equalizer() const { return eq_; }

    void setBass(const BassStageSettings& bass);
    [[nodiscard]] const BassStageSettings& bass() const { return bassSettings_; }

    // `interleaved` holds `frames` stereo frames, processed in place.
    void process(float* interleaved, int frames);

    void reset();

    [[nodiscard]] ChannelMeters meters() const { return meters_; }
    [[nodiscard]] float sampleRate() const { return sampleRate_; }

    // Samples of delay the chain adds, all of it the limiter's lookahead.
    [[nodiscard]] int latencySamples() const;

private:
    void rebuildEqualizer();

    float sampleRate_ = 48000.0f;
    ChannelChainSettings s_;
    ChannelMeters meters_;

    EqSettings eq_;
    // One peaking filter per band per channel: both sides must carry their own
    // state or the equalizer would collapse the stereo image.
    std::vector<Biquad> eqBands_[2];
    // Attenuation compensating the curve's peak boost. Derived from eq_, not a
    // setting.
    float eqHeadroom_ = 1.0f;

    BassStageSettings bassSettings_;
    // One per channel. The filter-chain arrangement replicated a mono graph
    // across the channels and this keeps that behaviour, which also means each
    // side's envelope follows its own low end.
    HarmonicBass bass_[2];
    Limiter limiter_[2];

    // One channel's worth of deinterleaved samples. The bass stage and the
    // limiter both work on a contiguous mono block, and calling them per sample
    // instead would be a function call and a loop setup for every frame on the
    // realtime thread. Allocated once in prepare(); a block larger than this is
    // processed in several passes.
    std::vector<float> scratch_;
    static constexpr int kMaxBlock = 4096;
};

}  // namespace sonar::dsp
