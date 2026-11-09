#pragma once

#include <vector>

#include "dsp/BassStage.h"
#include "dsp/Biquad.h"
#include "dsp/Equalizer.h"
#include "dsp/HarmonicBass.h"
#include "dsp/Limiter.h"

namespace sonar::dsp {

// Everything one output channel does to audio:
//   trim -> equalizer -> harmonic bass -> limiter -> fader (volume, balance, mute)
// EQ first, so the crossover sees what will be heard; limiter after the bass, as
// the last guard before the driver; fader last, so a channel at 10% is the same
// sound as at 100%, only quieter. Stereo interleaved, allocation-free after prepare().

struct ChannelChainSettings {
    // The Channels page's gain slider: a trim for a source that is mixed too
    // loud or too quiet, ahead of everything else.
    float trimDb = 0.0f;

    // The mixer's fader, its balance and its mute.
    float volume = 1.0f;    // linear, 0..1
    float balance = 0.0f;   // -1 full left .. +1 full right
    bool muted = false;
};

// What the meters draw, sampled per block. Peaks are linear and post-fader.
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

    // Band gains are clamped to [kMinGainDb, kMaxGainDb], and the curve's peak boost is
    // attenuated ahead of the filters so a boosted band cannot clip the output.
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
    // One per channel side, so each follows its own low end.
    HarmonicBass bass_[2];
    Limiter limiter_[2];

    // One side's deinterleaved samples: the whole chain runs on this contiguous
    // block. Allocated in prepare(); larger buffers go in several passes.
    std::vector<float> scratch_;
    static constexpr int kMaxBlock = 4096;
};

}  // namespace sonar::dsp
