#include "dsp/ChannelChain.h"

#include <algorithm>
#include <cmath>

#include "dsp/DspMath.h"

namespace sonar::dsp {

namespace {
// Even a flat cascade overshoots slightly, which clips mastered material.
constexpr float kFlatReserveDb = -0.5f;
}  // namespace

ChannelChain::ChannelChain() { prepare(48000.0f); }

void ChannelChain::prepare(float sampleRate) {
    sampleRate_ = sampleRate > 0.0f ? sampleRate : 48000.0f;
    // Reserve the widest equalizer now, so rebuildEqualizer() never allocates.
    for (std::vector<Biquad>& bands : eqBands_) {
        bands.reserve(static_cast<std::size_t>(BandCount::Bands31));
    }
    for (int c = 0; c < 2; ++c) {
        bass_[c].prepare(sampleRate_);
        limiter_[c].prepare(sampleRate_);
    }
    scratch_.assign(static_cast<std::size_t>(kMaxBlock), 0.0f);
    rebuildEqualizer();
    setBass(bassSettings_);
    reset();
}

void ChannelChain::setSettings(const ChannelChainSettings& settings) {
    s_ = settings;
    s_.volume = std::clamp(s_.volume, 0.0f, 1.0f);
    s_.balance = std::clamp(s_.balance, -1.0f, 1.0f);
}

void ChannelChain::setEqualizer(const EqSettings& eq) {
    eq_ = eq;
    rebuildEqualizer();
}

void ChannelChain::setBass(const BassStageSettings& bass) {
    bassSettings_ = bass;
    for (int c = 0; c < 2; ++c) {
        bass_[c].setSettings(bass.bass);
        limiter_[c].setEnabled(bass.limiterEnabled);
        limiter_[c].setCeilingDb(bass.limiterCeilingDb);
    }
    // Read back, so the chain's copy reflects the clamping HarmonicBass does on
    // crossover and harmonic count rather than the raw request.
    bassSettings_.bass = bass_[0].settings();
}

void ChannelChain::reset() {
    for (std::vector<Biquad>& bands : eqBands_) {
        for (Biquad& bq : bands) {
            bq.reset();
        }
    }
    for (int c = 0; c < 2; ++c) {
        bass_[c].reset();
        limiter_[c].reset();
    }
    meters_ = ChannelMeters{};
}

int ChannelChain::latencySamples() const { return limiter_[0].latencySamples(); }

void ChannelChain::rebuildEqualizer() {
    // Audio thread, on every slider move: must not allocate (prepare() reserved room).
    const std::vector<float>& freqs = standardFrequenciesRef(eq_.bandCount);
    const float q = bandQ(eq_.bandCount);

    for (std::vector<Biquad>& bands : eqBands_) {
        // Resized, not reassigned: a band already running keeps its state, so
        // moving one slider does not click every other filter in the cascade.
        if (bands.size() != freqs.size()) {
            bands.resize(freqs.size());
        }
        for (std::size_t i = 0; i < freqs.size(); ++i) {
            // bandGainAt, not responseDbAt: the filters are in series, so the summed curve
            // would apply the overlap twice.
            const float gainDb =
                eq_.enabled ? std::clamp(bandGainAt(eq_, freqs[i]), kMinGainDb, kMaxGainDb)
                            : 0.0f;
            const Biquad fresh = peaking(freqs[i], q, gainDb, sampleRate_);
            Biquad& bq = bands[i];
            bq.b0 = fresh.b0;
            bq.b1 = fresh.b1;
            bq.b2 = fresh.b2;
            bq.a1 = fresh.a1;
            bq.a2 = fresh.a2;
        }
    }

    // Attenuate by exactly what the curve boosts, so its loudest frequency lands at unity.
    const float boost = eq_.enabled ? peakBoostDb(eq_) : 0.0f;
    eqHeadroom_ = dbToLin(-boost + kFlatReserveDb);
}

void ChannelChain::process(float* interleaved, int frames) {
    if (interleaved == nullptr || frames <= 0) return;

    if (s_.muted) {
        std::fill(interleaved, interleaved + static_cast<std::size_t>(frames) * 2, 0.0f);
        meters_ = ChannelMeters{};
        return;
    }

    // Per block, constant across it.
    const float preGain = dbToLin(s_.trimDb) * eqHeadroom_;
    // Balance pans by attenuating one side rather than boosting the other, so it
    // can never add gain to a channel that was already at full scale.
    const float fader[2] = {s_.volume * (s_.balance > 0.0f ? 1.0f - s_.balance : 1.0f),
                            s_.volume * (s_.balance < 0.0f ? 1.0f + s_.balance : 1.0f)};

    float inputPeak = 0.0f;
    float outputPeak[2] = {0.0f, 0.0f};
    int clipped = 0;
    float worstReduction = 0.0f;

    // One trip per side and block: pull the side out of the interleaved buffer,
    // run the whole chain on the contiguous mono block, write it back through the
    // fader. Every stage is a recurrence on its own side, so this is the same
    // arithmetic in the same order as stage-by-stage over the whole buffer.
    float* x = scratch_.data();
    for (int c = 0; c < 2; ++c) {
        for (int done = 0; done < frames; done += kMaxBlock) {
            const int block = std::min(kMaxBlock, frames - done);
            float* side = interleaved + 2 * static_cast<std::size_t>(done) + c;

            for (int i = 0; i < block; ++i) {
                float v = side[2 * i];
                if (!std::isfinite(v)) v = 0.0f;  // a NaN must not poison the filters
                inputPeak = std::max(inputPeak, std::abs(v));
                x[i] = v * preGain;
            }
            if (eq_.enabled) {
                // Filter by filter over the block: one coefficient set in registers
                // at a time, instead of the whole cascade for every sample.
                for (Biquad& bq : eqBands_[c]) {
                    for (int i = 0; i < block; ++i) x[i] = bq.process(x[i]);
                }
            }
            bass_[c].process(x, block);
            limiter_[c].process(x, x, block);
            clipped += limiter_[c].clipped();
            worstReduction = std::min(worstReduction, limiter_[c].reductionDb());

            for (int i = 0; i < block; ++i) {
                const float v = x[i] * fader[c];
                side[2 * i] = v;
                outputPeak[c] = std::max(outputPeak[c], std::abs(v));
            }
        }
    }

    meters_.inputPeak = inputPeak;
    meters_.peakLeft = outputPeak[0];
    meters_.peakRight = outputPeak[1];
    meters_.limiterReductionDb = worstReduction;
    meters_.clippedSamples = clipped;
    meters_.bass = bass_[0].meters();
}

}  // namespace sonar::dsp
