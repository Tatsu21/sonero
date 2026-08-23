#include "dsp/AutoGain.h"

#include <algorithm>
#include <cmath>

namespace sonar::dsp {

namespace {
// The gain that would place `peak` exactly on `target`, given the gain already
// applied when `peak` was measured.
float gainFor(float currentGainDb, float peak, float target) {
    return currentGainDb + 20.0f * std::log10(target / peak);
}
}  // namespace

void AutoGain::reset(float gainDb) {
    gainDb_ = std::clamp(gainDb, config_.minGainDb, config_.maxGainDb);
    peakMemory_ = 0.0f;
    duckTarget_ = config_.maxGainDb;  // nothing pending
    sinceDuck_ = 0.0f;
    silentFor_ = 0.0f;
}

float AutoGain::update(float outputPeak, float dtSeconds) {
    if (!(dtSeconds > 0.0f)) {
        return gainDb_;
    }
    dtSeconds = std::min(dtSeconds, 0.25f);  // a stalled UI must not cause a jump
    outputPeak = std::max(outputPeak, 0.0f);

    // Silence proves nothing about how much room there is; climbing through it
    // would boost the noise floor and then slam on the first note back. Long
    // enough silence means the material is over, so the old peaks stop counting.
    if (outputPeak < config_.silenceFloor) {
        silentFor_ += dtSeconds;
        if (silentFor_ >= config_.memoryResetSeconds) {
            peakMemory_ = 0.0f;
        }
        return gainDb_;
    }
    silentFor_ = 0.0f;

    // --- down: what the overshoot demands, plus a margin, immediately ----------
    if (outputPeak > config_.ceilingPeak) {
        const float needed =
            std::clamp(gainFor(gainDb_, outputPeak, config_.ceilingPeak) - config_.duckMarginDb,
                       config_.minGainDb, config_.maxGainDb);
        duckTarget_ = std::min(duckTarget_, needed);  // the worst peak wins
        peakMemory_ = outputPeak;                     // and is the one to respect
    }
    if (gainDb_ > duckTarget_) {
        // Never overshoot past what was demanded, however fast the ramp is allowed.
        gainDb_ = std::max(duckTarget_, gainDb_ - config_.duckDbPerSec * dtSeconds);
        sinceDuck_ = 0.0f;
        if (gainDb_ <= duckTarget_ + 1e-4f) {
            duckTarget_ = config_.maxGainDb;  // reached: stop pulling
        }
        return gainDb_;
    }

    // --- up: only while the loudest recent peak still leaves room --------------
    const float decay =
        std::pow(0.5f, dtSeconds / std::max(config_.peakMemoryHalfLife, 0.01f));
    peakMemory_ = std::max(outputPeak, peakMemory_ * decay);
    sinceDuck_ += dtSeconds;

    if (sinceDuck_ < config_.climbDelaySeconds) {
        return gainDb_;  // still settling after a reduction
    }
    const float allowed =
        std::clamp(gainFor(gainDb_, peakMemory_, config_.climbTargetPeak),
                   config_.minGainDb, config_.maxGainDb);
    if (allowed > gainDb_) {
        gainDb_ = std::min(allowed, gainDb_ + config_.climbDbPerSec * dtSeconds);
    }
    return gainDb_;
}

}  // namespace sonar::dsp
