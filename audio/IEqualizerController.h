#pragma once

#include "audio/Channel.h"
#include "dsp/Equalizer.h"

namespace sonar::audio {

// Applies per-channel equalizer settings to the real audio path (a PipeWire
// filter-chain of biquads on each virtual sink). Implemented by the backend.
class IEqualizerController {
public:
    virtual ~IEqualizerController() = default;

    virtual void applyEqualizer(ChannelId id, const dsp::EqSettings& settings) = 0;
};

}  // namespace sonar::audio
