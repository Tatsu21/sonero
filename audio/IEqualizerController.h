#pragma once

#include "audio/Channel.h"
#include "dsp/BassStage.h"
#include "dsp/Equalizer.h"

namespace sonar::audio {

// Drives the per-channel stages of the real audio path: the equalizer and the
// harmonic bass stage that follows it. Both are stages of one chain
// (dsp::ChannelChain) running inside the channel's own node, so they are driven
// through one interface rather than two.
class IEqualizerController {
public:
    virtual ~IEqualizerController() = default;

    // The channel's graphic equalizer: a cascade of biquads on its virtual sink.
    virtual void applyEqualizer(ChannelId id, const dsp::EqSettings& settings) = 0;

    // The harmonic bass stage that follows it in the same chain, plus the
    // limiter guarding its output.
    virtual void applyBassStage(ChannelId id, const dsp::BassStageSettings& settings) = 0;
};

}  // namespace sonar::audio
