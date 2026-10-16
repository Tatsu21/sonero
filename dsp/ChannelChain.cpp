#include "dsp/ChannelChain.h"

#include <algorithm>
#include <cmath>

namespace sonar::dsp {

namespace {

constexpr float kTinyLinear = 1e-6f;

// Even a flat biquad cascade overshoots slightly, and material mastered to full
// scale then clips on conversion to the device's integer format. The old
// filter-chain path reserved the same margin in the sink's channelVolumes.
constexpr float kFlatReserveDb = -0.5f;

[[nodiscard]] float dbToLin(float db) { return std::pow(10.0f, db * 0.05f); }

}  // namespace

ChannelChain::ChannelChain() { prepare(48000.0f); }

void ChannelChain::prepare(float sampleRate) {
    sampleRate_ = sampleRate > 0.0f ? sampleRate : 48000.0f;
    // Room for the widest equalizer this chain can be given, taken here where
    // allocating is allowed, so rebuildEqualizer() never has to ask the
    // allocator for anything on the audio thread.
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
    // Runs on the audio thread whenever a slider moves, so it must not allocate:
    // the reference avoids copying the frequency table and prepare() reserved
    // room for the largest band count.
    const std::vector<float>& freqs = standardFrequenciesRef(eq_.bandCount);
    const float q = bandQ(eq_.bandCount);

    for (std::vector<Biquad>& bands : eqBands_) {
        // Resized, not reassigned: a band already running keeps its state, so
        // moving one slider does not click every other filter in the cascade.
        if (bands.size() != freqs.size()) {
            bands.resize(freqs.size());
        }
        for (std::size_t i = 0; i < freqs.size(); ++i) {
            // bandGainAt, not responseDbAt: the filters are in series and their
            // responses already add, so feeding the summed display curve into
            // each one would apply the overlap between neighbours twice.
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

    // Attenuate ahead of the filters by exactly what the curve boosts, so the
    // loudest equalized frequency lands back at unity and the fader stays usable
    // across its whole range.
    const float boost = eq_.enabled ? peakBoostDb(eq_) : 0.0f;
    eqHeadroom_ = dbToLin(-boost + kFlatReserveDb);
}

void ChannelChain::process(float* interleaved, int frames) {
    if (interleaved == nullptr || frames <= 0) {
        return;
    }

    if (s_.muted) {
        std::fill(interleaved, interleaved + static_cast<std::size_t>(frames) * 2, 0.0f);
        meters_ = ChannelMeters{};
        return;
    }

    const float trim = dbToLin(s_.trimDb);
    // Balance pans by attenuating one side rather than boosting the other, so it
    // can never add gain to a channel that was already at full scale.
    const float leftGain = s_.volume * (s_.balance > 0.0f ? 1.0f - s_.balance : 1.0f);
    const float rightGain = s_.volume * (s_.balance < 0.0f ? 1.0f + s_.balance : 1.0f);

    float inputPeak = 0.0f;

    // --- Trim and equalizer, per channel ------------------------------------
    for (int i = 0; i < frames; ++i) {
        for (int c = 0; c < 2; ++c) {
            const std::size_t idx = static_cast<std::size_t>(2 * i + c);
            float x = interleaved[idx];
            if (!std::isfinite(x)) {
                x = 0.0f;  // a decoder handing us a NaN must not poison the filters
            }
            inputPeak = std::max(inputPeak, std::abs(x));
            x *= trim * eqHeadroom_;
            if (eq_.enabled) {
                for (Biquad& bq : eqBands_[c]) {
                    x = bq.process(x);
                }
            }
            interleaved[idx] = x;
        }
    }

    // --- Harmonic bass and limiter, per channel -----------------------------
    // Both stages work on a contiguous mono block, so each side is deinterleaved
    // into the scratch, processed whole, and written back. A buffer longer than
    // the scratch is done in several passes rather than reallocating.
    // Accumulated across passes: the limiter reports per call, so with a buffer
    // long enough to be split, only the last pass would otherwise be counted.
    int clipped = 0;
    float worstReduction = 0.0f;
    for (int c = 0; c < 2; ++c) {
        for (int done = 0; done < frames; done += kMaxBlock) {
            const int block = std::min(kMaxBlock, frames - done);
            float* work = scratch_.data();
            for (int i = 0; i < block; ++i) {
                work[i] = interleaved[static_cast<std::size_t>(2 * (done + i) + c)];
            }
            bass_[c].process(work, block);
            limiter_[c].process(work, work, block);
            for (int i = 0; i < block; ++i) {
                interleaved[static_cast<std::size_t>(2 * (done + i) + c)] = work[i];
            }
            clipped += limiter_[c].clipped();
            worstReduction = std::min(worstReduction, limiter_[c].reductionDb());
        }
    }

    // --- Fader ---------------------------------------------------------------
    float peakL = 0.0f;
    float peakR = 0.0f;
    for (int i = 0; i < frames; ++i) {
        const std::size_t li = static_cast<std::size_t>(2 * i);
        const float l = interleaved[li] * leftGain;
        const float r = interleaved[li + 1] * rightGain;
        interleaved[li] = l;
        interleaved[li + 1] = r;
        peakL = std::max(peakL, std::abs(l));
        peakR = std::max(peakR, std::abs(r));
    }

    meters_.inputPeak = inputPeak;
    meters_.peakLeft = peakL;
    meters_.peakRight = peakR;
    meters_.limiterReductionDb = worstReduction;
    meters_.clippedSamples = clipped;
    meters_.bass = bass_[0].meters();
}

}  // namespace sonar::dsp
