#pragma once

#include "audio/Channel.h"
#include "dsp/BassStage.h"
#include "dsp/Equalizer.h"

namespace sonar::audio {

// Drives the equalizer and harmonic bass stages of each channel's dsp::ChannelChain.
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
