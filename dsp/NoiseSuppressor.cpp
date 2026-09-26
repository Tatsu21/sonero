#include "dsp/NoiseSuppressor.h"

#include <algorithm>
#include <cmath>

#if SONAR_HAVE_RNNOISE
#include <rnnoise.h>
#endif

namespace sonar::dsp {

namespace {
constexpr float kTinyLinear = 1e-6f;

[[nodiscard]] float linToDb(float lin) {
    return 20.0f * std::log10(std::max(std::abs(lin), kTinyLinear));
}
}  // namespace

NoiseSuppressor::NoiseSuppressor() {
#if SONAR_HAVE_RNNOISE
    // nullptr asks RNNoise for its built-in model, which is the one it ships and
    // the only one we have any business running.
    state_ = rnnoise_create(nullptr);
#endif
}

NoiseSuppressor::~NoiseSuppressor() {
#if SONAR_HAVE_RNNOISE
    if (state_ != nullptr) {
        rnnoise_destroy(state_);
    }
#endif
}

bool NoiseSuppressor::available() {
#if SONAR_HAVE_RNNOISE
    return true;
#else
    return false;
#endif
}

std::string_view NoiseSuppressor::backendName() {
#if SONAR_HAVE_RNNOISE
    return "RNNoise";
#else
    return "none";
#endif
}

void NoiseSuppressor::prepare(float sampleRate) {
    // 48 kHz exactly. Feeding a 48 kHz model at 44.1 would not fail, it would
    // quietly mis-hear every band — worse than not running at all, because the
    // switch would look like it was working.
    usable_ = available() && state_ != nullptr && std::abs(sampleRate - 48000.0f) < 1.0f;
    reset();
}

void NoiseSuppressor::reset() {
    inFrame_.fill(0.0f);
    outFrame_.fill(0.0f);
    fill_ = 0;
    outPos_ = 0;
    primed_ = false;
    reductionDb_ = 0.0f;
    speechProbability_ = 0.0f;
}

int NoiseSuppressor::latencySamples() const {
    return (usable_ && enabled_) ? kFrameSize : 0;
}

void NoiseSuppressor::process(float* samples, int count) {
    if (samples == nullptr || count <= 0) {
        return;
    }
    if (!usable_ || !enabled_) {
        // Deliberately not a reset: switching the stage off and on again inside
        // a sentence should not replay a frame of silence.
        return;
    }

#if SONAR_HAVE_RNNOISE
    for (int i = 0; i < count; ++i) {
        const float in = samples[i];

        // Emit first, then consume: the sample leaving now belongs to the frame
        // the model has already answered for, which is what makes the delay
        // exactly one frame and not one frame plus a block.
        // outPos_ and fill_ advance in lockstep after the first frame, so the
        // index is in range by construction — the guard is there so that a
        // future change to this loop degrades to silence rather than to a read
        // past the end of the frame.
        samples[i] = (primed_ && outPos_ < kFrameSize)
                         ? outFrame_[static_cast<std::size_t>(outPos_)] / kInt16Scale
                         : 0.0f;
        if (primed_) {
            ++outPos_;
        }

        inFrame_[static_cast<std::size_t>(fill_)] = in * kInt16Scale;
        if (++fill_ == kFrameSize) {
            float inputSum = 0.0f;
            for (const float sample : inFrame_) {
                inputSum += sample * sample;
            }

            speechProbability_ = rnnoise_process_frame(state_, outFrame_.data(),
                                                       inFrame_.data());

            float outputSum = 0.0f;
            for (const float sample : outFrame_) {
                outputSum += sample * sample;
            }
            // Energy in against energy out: what the meter shows as "how much of
            // what the microphone heard was not you".
            const float inRms = std::sqrt(inputSum / kFrameSize);
            const float outRms = std::sqrt(outputSum / kFrameSize);
            reductionDb_ = inRms > kTinyLinear ? std::min(0.0f, linToDb(outRms) - linToDb(inRms))
                                               : 0.0f;

            fill_ = 0;
            outPos_ = 0;
            primed_ = true;
        }
    }
#endif
}

}  // namespace sonar::dsp
