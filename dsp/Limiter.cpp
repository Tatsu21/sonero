#include "dsp/Limiter.h"

#include <algorithm>
#include <cmath>

namespace sonar::dsp {

namespace {

constexpr float kTinyLinear = 1e-6f;

// The same two milliseconds MicChain's limiter uses. Long enough to catch the
// rise of a transient, short enough that nobody notices it on playback.
constexpr float kLookaheadMs = 2.0f;

[[nodiscard]] float dbToLin(float db) { return std::pow(10.0f, db * 0.05f); }

[[nodiscard]] float linToDb(float lin) {
    return 20.0f * std::log10(std::max(std::abs(lin), kTinyLinear));
}

[[nodiscard]] float envCoef(float ms, float sampleRate) {
    if (ms <= 0.0f) {
        return 0.0f;
    }
    return std::exp(-1.0f / (ms * 0.001f * sampleRate));
}

}  // namespace

Limiter::Limiter() { prepare(48000.0f); }

void Limiter::prepare(float sampleRate) {
    sampleRate_ = sampleRate > 0.0f ? sampleRate : 48000.0f;
    lookahead_ = std::max(1, static_cast<int>(kLookaheadMs * 0.001f * sampleRate_ + 0.5f));
    delay_.assign(static_cast<std::size_t>(lookahead_), 0.0f);
    releaseCoef_ = envCoef(releaseMs_, sampleRate_);
    reset();
}

void Limiter::setCeilingDb(float ceilingDb) {
    ceilingDb_ = std::min(ceilingDb, 0.0f);
    ceilingLin_ = dbToLin(ceilingDb_);
}

void Limiter::setReleaseMs(float releaseMs) {
    releaseMs_ = std::max(1.0f, releaseMs);
    releaseCoef_ = envCoef(releaseMs_, sampleRate_);
}

void Limiter::reset() {
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    write_ = 0;
    gain_ = 1.0f;
    reductionDb_ = 0.0f;
    clipped_ = 0;
}

void Limiter::process(const float* in, float* out, int count) {
    if (in == nullptr || out == nullptr || count <= 0) {
        return;
    }

    clipped_ = 0;

    if (!enabled_) {
        float worst = 0.0f;
        for (int i = 0; i < count; ++i) {
            const float x = in[static_cast<std::size_t>(i)];
            if (std::abs(x) > 1.0f) {
                ++clipped_;
            }
            worst = std::max(worst, std::abs(x));
            out[static_cast<std::size_t>(i)] = x;
        }
        gain_ = 1.0f;
        reductionDb_ = 0.0f;
        return;
    }

    float worstGain = 0.0f;
    for (int i = 0; i < count; ++i) {
        const float x = in[static_cast<std::size_t>(i)];

        // The peak is read from the sample going in and the gain is applied to
        // the sample coming out of the delay, so by the time the loud one
        // arrives the gain is already down. That is what lookahead buys, and
        // why this stage adds latency.
        const float peak = std::abs(x);
        const float required = peak > ceilingLin_ ? ceilingLin_ / peak : 1.0f;
        if (required < gain_) {
            // Ramp across the lookahead window rather than stepping: a step in
            // gain is itself a click.
            gain_ += (required - gain_) / static_cast<float>(lookahead_);
        } else {
            gain_ = required + releaseCoef_ * (gain_ - required);
        }

        const std::size_t w = static_cast<std::size_t>(write_);
        const float delayed = delay_[w];
        delay_[w] = x;
        write_ = (write_ + 1) % lookahead_;

        float y = delayed * gain_;
        // The ramp can still be a fraction of a decibel behind a signal that
        // rises faster than the lookahead. Nothing leaves above the ceiling.
        y = std::clamp(y, -ceilingLin_, ceilingLin_);
        out[static_cast<std::size_t>(i)] = y;
        worstGain = std::min(worstGain, linToDb(gain_));
    }
    reductionDb_ = worstGain;
}

}  // namespace sonar::dsp
