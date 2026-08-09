#include "dsp/AutoGain.h"

#include <algorithm>
#include <cmath>

namespace sonar::dsp {

void AutoGain::reset(float gainDb) {
    gainDb_ = std::clamp(gainDb, config_.minGainDb, config_.maxGainDb);
    targetGainDb_ = gainDb_;
    phase_ = Phase::Observing;
    envelope_ = 0.0f;
    observedPeak_ = 0.0f;
    observedFor_ = 0.0f;
    offTargetFor_ = 0.0f;
}

void AutoGain::commit() {
    // The measurement already includes the gain applied while observing, so the
    // decision is a correction to it rather than an absolute value.
    if (observedPeak_ > 0.0f) {
        const float errorDb = 20.0f * std::log10(config_.targetPeak / observedPeak_);
        targetGainDb_ = std::clamp(gainDb_ + errorDb, config_.minGainDb, config_.maxGainDb);
    }
    phase_ = Phase::Holding;
    observedPeak_ = 0.0f;
    observedFor_ = 0.0f;
    offTargetFor_ = 0.0f;
}

float AutoGain::update(float peak, float dtSeconds) {
    if (!(dtSeconds > 0.0f)) {
        return gainDb_;
    }
    dtSeconds = std::min(dtSeconds, 0.25f);  // a stalled UI must not cause a jump
    peak = std::max(peak, 0.0f);

    const float decay =
        std::pow(0.5f, dtSeconds / std::max(config_.envelopeHalfLife, 0.01f));
    envelope_ = std::max(peak, envelope_ * decay);

    // --- safety limiter -------------------------------------------------------
    // Independent of the phases: a peak this close to full scale is about to clip,
    // and no amount of "settled" gain is worth that.
    if (peak >= config_.ceilingPeak) {
        gainDb_ = std::max(config_.minGainDb, gainDb_ - config_.duckDbPerSec * dtSeconds);
        targetGainDb_ = gainDb_;  // the ducked value becomes the new resting point
        offTargetFor_ = 0.0f;
        return gainDb_;
    }

    // Silence says nothing about how loud the material is; never adapt to it.
    if (peak < config_.silenceFloor) {
        return gainDb_;
    }

    if (phase_ == Phase::Observing) {
        observedPeak_ = std::max(observedPeak_, envelope_);
        observedFor_ += dtSeconds;
        if (observedFor_ >= config_.observeSeconds) {
            commit();
        }
    } else {
        // Holding. Only a sustained, large deviation counts as "different
        // material" — brief loud passages must not restart the whole process.
        const float errorDb = std::abs(20.0f * std::log10(config_.targetPeak / envelope_));
        if (errorDb > config_.reevaluateDeltaDb) {
            offTargetFor_ += dtSeconds;
            if (offTargetFor_ >= config_.reevaluateSeconds) {
                phase_ = Phase::Observing;
                observedPeak_ = 0.0f;
                observedFor_ = 0.0f;
                offTargetFor_ = 0.0f;
            }
        } else {
            offTargetFor_ = 0.0f;  // back in range: forget the excursion
        }
    }

    // Ease towards the chosen gain. Outside this ramp the gain does not move at
    // all, which is the whole point: a constant gain is inaudible, a moving one
    // is not.
    const float step = config_.rampDbPerSec * dtSeconds;
    gainDb_ = targetGainDb_ > gainDb_ ? std::min(targetGainDb_, gainDb_ + step)
                                      : std::max(targetGainDb_, gainDb_ - step);
    return gainDb_;
}

}  // namespace sonar::dsp
