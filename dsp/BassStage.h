#pragma once

#include "dsp/HarmonicBass.h"

namespace sonar::dsp {

// The harmonic bass stage's settings plus the limiter that catches what it adds.
struct BassStageSettings {
    HarmonicBassSettings bass;

    // Bounds what leaves the channel, not what the equalizer may do. On by default,
    // because the channel equalizer has no gain ceiling.
    bool limiterEnabled = true;
    float limiterCeilingDb = -1.0f;
};

}  // namespace sonar::dsp
