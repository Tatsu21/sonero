#pragma once

#include "dsp/HarmonicBass.h"

namespace sonar::dsp {

// Everything the harmonic bass plugin is configured by, in one struct: the
// synthesis itself plus the brick-wall limiter that follows it.
//
// The two live together because they are one stage of a channel's chain: the
// limiter exists to catch what the synthesis adds, and switching the synthesis
// on without it would be asking for the clipping it prevents.
struct BassStageSettings {
    HarmonicBassSettings bass;

    // Not a gain limit and not a sound-shaping tool: it bounds what leaves the
    // channel, never what the equalizer ahead of it may do. It is on by default
    // because the Lab channel's equalizer has no gain ceiling, so without it a
    // boosted band goes to the driver exactly as asked.
    bool limiterEnabled = true;
    float limiterCeilingDb = -1.0f;
};

}  // namespace sonar::dsp
