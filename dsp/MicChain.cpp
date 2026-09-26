#include "dsp/MicChain.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sonar::dsp {

namespace {

// Everything below this is silence as far as the meters and detectors are
// concerned. Also the floor for the logarithm, which has no answer at zero.
constexpr float kSilenceFloorDb = -120.0f;
constexpr float kTinyLinear = 1e-6f;

// How far the gate is ever allowed to pull the signal down. A downward expander
// with a high ratio reaches -200 dB on a quiet passage, which is both pointless
// and a denormal factory.
constexpr float kMaxGateReductionDb = -60.0f;

// Fixed, because it is latency the user pays for and there is no setting worth
// two milliseconds of argument. Long enough to catch a plosive's rise.
constexpr float kLookaheadMs = 2.0f;

// The level auto-makeup restores. Speech trimmed sensibly peaks around here, so
// undoing the compressor's reduction at this point leaves the voice where it was
// before compression rather than louder than everything else on the desktop.
constexpr float kAutoMakeupReferenceDb = -10.0f;

[[nodiscard]] float dbToLin(float db) { return std::pow(10.0f, db * 0.05f); }

[[nodiscard]] float linToDb(float lin) {
    return 20.0f * std::log10(std::max(std::abs(lin), kTinyLinear));
}

// One-pole smoothing coefficient for a time constant in milliseconds. Zero (or
// negative) means "immediately", which is what a 0 ms attack has to mean.
[[nodiscard]] float envCoef(float ms, float sampleRate) {
    if (ms <= 0.0f) {
        return 0.0f;
    }
    return std::exp(-1.0f / (ms * 0.001f * sampleRate));
}

// The static gain curve of a compressor with a soft knee, in decibels. Returns
// the gain to apply (never positive). Below the knee it is zero, above it the
// signal is pulled toward the threshold by the ratio, and across the knee the
// two meet as a quadratic so there is no audible corner.
[[nodiscard]] float compressorGainDb(float levelDb, float thresholdDb, float ratio,
                                     float kneeDb) {
    const float over = levelDb - thresholdDb;
    if (2.0f * over < -kneeDb) {
        return 0.0f;
    }
    if (kneeDb > 0.0f && 2.0f * std::abs(over) <= kneeDb) {
        const float x = over + kneeDb * 0.5f;
        return (1.0f / ratio - 1.0f) * x * x / (2.0f * kneeDb);
    }
    return over * (1.0f / ratio - 1.0f);
}

}  // namespace

// --- Biquad -------------------------------------------------------------------

float MicChain::Biquad::process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    // Denormals cost hundreds of cycles each on x86 and only ever appear in a
    // filter that has been fed silence for a while — which is most of a
    // microphone's life.
    if (!std::isfinite(z1) || std::abs(z1) < 1e-25f) z1 = 0.0f;
    if (!std::isfinite(z2) || std::abs(z2) < 1e-25f) z2 = 0.0f;
    return y;
}

void MicChain::Biquad::reset() {
    z1 = 0.0f;
    z2 = 0.0f;
}

// --- Construction -------------------------------------------------------------

MicChain::MicChain() { prepare(48000.0f); }

void MicChain::prepare(float sampleRate) {
    sampleRate_ = sampleRate > 0.0f ? sampleRate : 48000.0f;
    lookaheadSamples_ =
        std::max(1, static_cast<int>(kLookaheadMs * 0.001f * sampleRate_ + 0.5f));
    lookahead_.assign(static_cast<std::size_t>(lookaheadSamples_), 0.0f);
    // Room for the widest equalizer this chain can be given, taken here where
    // allocating is allowed, so rebuildEqualizer() never has to.
    eqBands_.reserve(static_cast<std::size_t>(BandCount::Bands31));
    rebuildCoefficients();
    reset();
}

void MicChain::setSettings(const MicSettings& settings) {
    settings_ = settings;
    rebuildCoefficients();
}

void MicChain::setEqualizer(const EqSettings& eq) {
    eq_ = eq;
    rebuildEqualizer();
}

void MicChain::reset() {
    highPass_.reset();
    deEsserBand_.reset();
    for (Biquad& band : eqBands_) {
        band.reset();
    }
    gateEnv_ = 0.0f;
    gateGain_ = 1.0f;
    gateHoldCounter_ = 0;
    gateIsOpen_ = false;
    deEsserEnv_ = 0.0f;
    deEsserGain_ = 1.0f;
    compEnv_ = 0.0f;
    compGainDb_ = 0.0f;
    std::fill(lookahead_.begin(), lookahead_.end(), 0.0f);
    lookaheadWrite_ = 0;
    limiterGain_ = 1.0f;
    meters_ = MicMeters{};
}

int MicChain::latencySamples() const {
    return settings_.limiter.enabled ? lookaheadSamples_ : 0;
}

void MicChain::rebuildCoefficients() {
    const float sr = sampleRate_;
    const float twoPi = 2.0f * std::numbers::pi_v<float>;

    // High-pass, RBJ cookbook, Butterworth Q so the pass band is flat rather
    // than bumped just above the corner — a resonance at 90 Hz would add exactly
    // the boom this filter exists to remove.
    {
        const float f = std::clamp(settings_.highPass.frequencyHz, 20.0f, sr * 0.45f);
        const float w0 = twoPi * f / sr;
        const float cosw = std::cos(w0);
        const float alpha = std::sin(w0) / (2.0f * 0.70710678f);
        const float a0 = 1.0f + alpha;
        highPass_.b0 = (1.0f + cosw) * 0.5f / a0;
        highPass_.b1 = -(1.0f + cosw) / a0;
        highPass_.b2 = highPass_.b0;
        highPass_.a1 = -2.0f * cosw / a0;
        highPass_.a2 = (1.0f - alpha) / a0;
    }

    // Band-pass feeding the de-esser's detector. Wide enough (Q = 2) to cover
    // the whole sibilant range rather than one frequency of it, and it never
    // touches the audio — only the decision.
    {
        const float f = std::clamp(settings_.deEsser.frequencyHz, 1000.0f, sr * 0.45f);
        const float w0 = twoPi * f / sr;
        const float cosw = std::cos(w0);
        const float alpha = std::sin(w0) / (2.0f * 2.0f);
        const float a0 = 1.0f + alpha;
        deEsserBand_.b0 = alpha / a0;
        deEsserBand_.b1 = 0.0f;
        deEsserBand_.b2 = -alpha / a0;
        deEsserBand_.a1 = -2.0f * cosw / a0;
        deEsserBand_.a2 = (1.0f - alpha) / a0;
    }

    gateAttackCoef_ = envCoef(settings_.gate.attackMs, sr);
    gateReleaseCoef_ = envCoef(settings_.gate.releaseMs, sr);
    gateHoldSamples_ =
        static_cast<int>(std::max(0.0f, settings_.gate.holdMs) * 0.001f * sr);
    deEsserAttackCoef_ = envCoef(1.0f, sr);    // sibilance is short; catch its start
    deEsserReleaseCoef_ = envCoef(60.0f, sr);  // and let go before the next word
    compAttackCoef_ = envCoef(settings_.compressor.attackMs, sr);
    compReleaseCoef_ = envCoef(settings_.compressor.releaseMs, sr);
    limiterReleaseCoef_ = envCoef(settings_.limiter.releaseMs, sr);
    rebuildEqualizer();
}

void MicChain::rebuildEqualizer() {
    // This runs on the audio thread every time the user moves a slider, so it
    // must not allocate: the reference avoids copying the frequency table, and
    // prepare() has already reserved room for the largest band count, so the
    // resize below never asks the allocator for anything.
    const std::vector<float>& freqs = standardFrequenciesRef(eq_.bandCount);
    // Resized, not reassigned: a band that was already running keeps its state,
    // so moving one slider does not click every other filter in the cascade.
    if (eqBands_.size() != freqs.size()) {
        eqBands_.resize(freqs.size());
    }

    const float sr = sampleRate_;
    const float twoPi = 2.0f * std::numbers::pi_v<float>;
    // Q follows the band spacing. The channel equalizers are always 31 bands, so
    // they can hard-code a third-octave Q; the microphone's is ten, an octave
    // apart, and reusing that constant here gave ten narrow spikes with holes
    // between them — the reason dragging a band was almost inaudible.
    const float q = bandQ(eq_.bandCount);

    for (std::size_t i = 0; i < freqs.size(); ++i) {
        const float gainDb = eq_.enabled ? bandGainAt(eq_, freqs[i]) : 0.0f;
        const float f = std::clamp(freqs[i], 20.0f, sr * 0.45f);
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = twoPi * f / sr;
        const float cosw = std::cos(w0);
        const float alpha = std::sin(w0) / (2.0f * q);
        const float a0 = 1.0f + alpha / A;

        Biquad& bq = eqBands_[i];
        bq.b0 = (1.0f + alpha * A) / a0;
        bq.b1 = -2.0f * cosw / a0;
        bq.b2 = (1.0f - alpha * A) / a0;
        bq.a1 = bq.b1;
        bq.a2 = (1.0f - alpha / A) / a0;
    }
}

// --- Processing ---------------------------------------------------------------

void MicChain::process(float* samples, int count) {
    if (samples == nullptr || count <= 0) {
        return;
    }

    const MicSettings& s = settings_;

    if (s.muted) {
        std::fill(samples, samples + count, 0.0f);
        meters_ = MicMeters{};
        return;
    }

    const float inputGain = dbToLin(s.inputGainDb);
    const float outputGain = dbToLin(s.outputGainDb);
    const float ceilingLin = dbToLin(s.limiter.ceilingDb);

    // The compressor's makeup is a property of the settings, not of the signal,
    // so it is computed once per block rather than per sample.
    float makeupDb = s.compressor.makeupDb;
    if (s.compressor.enabled && s.compressor.autoMakeup) {
        makeupDb = -compressorGainDb(kAutoMakeupReferenceDb, s.compressor.thresholdDb,
                                     s.compressor.ratio, s.compressor.kneeDb);
    }
    const float makeupLin = s.compressor.enabled ? dbToLin(makeupDb) : 1.0f;

    float inputPeak = 0.0f;
    float outputPeak = 0.0f;
    float worstGate = 0.0f;
    float worstDeEsser = 0.0f;
    float worstComp = 0.0f;
    float worstLimiter = 0.0f;

    for (int i = 0; i < count; ++i) {
        float x = samples[i] * inputGain;
        if (!std::isfinite(x)) {
            x = 0.0f;  // a driver that hands us a NaN must not poison every filter
        }
        inputPeak = std::max(inputPeak, std::abs(x));

        if (s.highPass.enabled) {
            x = highPass_.process(x);
        }

        // --- Gate -------------------------------------------------------------
        if (s.gate.enabled) {
            const float rectified = std::abs(x);
            const float coef = rectified > gateEnv_ ? gateAttackCoef_ : gateReleaseCoef_;
            gateEnv_ = rectified + coef * (gateEnv_ - rectified);
            const float envDb = linToDb(gateEnv_);

            // Two thresholds, not one: a single one makes the gate chatter on
            // every sample that sits exactly at it, which is audible as a buzz.
            const float openAt = s.gate.thresholdDb;
            const float closeAt = s.gate.thresholdDb - std::abs(s.gate.hysteresisDb);
            if (envDb > (gateIsOpen_ ? closeAt : openAt)) {
                gateIsOpen_ = true;
                gateHoldCounter_ = gateHoldSamples_;
            } else if (gateHoldCounter_ > 0) {
                --gateHoldCounter_;
            } else {
                gateIsOpen_ = false;
            }

            float targetDb = 0.0f;
            if (!gateIsOpen_) {
                targetDb = std::max((envDb - s.gate.thresholdDb) * (s.gate.ratio - 1.0f),
                                    kMaxGateReductionDb);
            }
            const float targetLin = dbToLin(targetDb);
            const float coefG = targetLin > gateGain_ ? gateAttackCoef_ : gateReleaseCoef_;
            gateGain_ = targetLin + coefG * (gateGain_ - targetLin);
            x *= gateGain_;
            worstGate = std::min(worstGate, linToDb(gateGain_));
        } else {
            gateGain_ = 1.0f;
            gateIsOpen_ = true;
        }

        // --- De-esser ---------------------------------------------------------
        if (s.deEsser.enabled) {
            // The detector listens to the sibilant band only; the gain it asks
            // for is applied to the whole signal, which is what keeps an S from
            // sounding filtered instead of quieter.
            const float band = std::abs(deEsserBand_.process(x));
            const float coef = band > deEsserEnv_ ? deEsserAttackCoef_ : deEsserReleaseCoef_;
            deEsserEnv_ = band + coef * (deEsserEnv_ - band);
            const float reductionDb = compressorGainDb(linToDb(deEsserEnv_),
                                                       s.deEsser.thresholdDb,
                                                       s.deEsser.ratio, 3.0f);
            deEsserGain_ = dbToLin(reductionDb);
            x *= deEsserGain_;
            worstDeEsser = std::min(worstDeEsser, reductionDb);
        } else {
            deEsserGain_ = 1.0f;
        }

        // --- Equalizer --------------------------------------------------------
        // After the de-esser and before the compressor: the compressor should
        // react to the voice as it will be heard, and an equalizer placed after
        // it would undo the levelling it just did.
        // No headroom compensation here, unlike the channel sinks. They attenuate
        // by whatever the curve boosts because nothing downstream protects them;
        // this chain ends in a limiter whose entire job is that nothing leaves
        // above the ceiling. Doing both meant a +10 dB band arrived as exactly
        // 0 dB at its centre frequency — the boost and the compensation cancelled,
        // which is what made the equalizer feel like it did nothing.
        if (eq_.enabled) {
            for (Biquad& band : eqBands_) {
                x = band.process(x);
            }
        }

        // --- Compressor -------------------------------------------------------
        if (s.compressor.enabled) {
            const float rectified = std::abs(x);
            const float coef = rectified > compEnv_ ? compAttackCoef_ : compReleaseCoef_;
            compEnv_ = rectified + coef * (compEnv_ - rectified);

            const float targetDb =
                compressorGainDb(linToDb(compEnv_), s.compressor.thresholdDb,
                                 s.compressor.ratio, s.compressor.kneeDb);
            // Smoothing in decibels rather than in linear gain: the ear hears
            // decibels, and a linear ramp between two gains is a curve in the
            // domain that matters.
            const float coefG = targetDb < compGainDb_ ? compAttackCoef_ : compReleaseCoef_;
            compGainDb_ = targetDb + coefG * (compGainDb_ - targetDb);
            x *= dbToLin(compGainDb_);
            worstComp = std::min(worstComp, compGainDb_);
        } else {
            compGainDb_ = 0.0f;
        }

        x *= makeupLin;
        x *= outputGain;

        // --- Limiter ----------------------------------------------------------
        if (s.limiter.enabled) {
            // The peak is read from the sample going in, the gain is applied to
            // the sample coming out of the delay — so by the time the loud one
            // arrives, the gain is already down. That is the whole point of
            // lookahead, and the reason this stage adds latency.
            const float required =
                std::abs(x) > ceilingLin ? ceilingLin / std::abs(x) : 1.0f;
            if (required < limiterGain_) {
                // Ramp down across the lookahead window rather than jumping: a
                // step in gain is itself a click.
                limiterGain_ += (required - limiterGain_) / static_cast<float>(lookaheadSamples_);
            } else {
                limiterGain_ = required + limiterReleaseCoef_ * (limiterGain_ - required);
            }

            const std::size_t w = static_cast<std::size_t>(lookaheadWrite_);
            const float delayed = lookahead_[w];
            lookahead_[w] = x;
            lookaheadWrite_ = (lookaheadWrite_ + 1) % lookaheadSamples_;

            x = delayed * limiterGain_;
            // The ramp can still be a fraction of a decibel behind a sample that
            // rises faster than the lookahead. Nothing leaves above the ceiling.
            x = std::clamp(x, -ceilingLin, ceilingLin);
            worstLimiter = std::min(worstLimiter, linToDb(limiterGain_));
        } else {
            limiterGain_ = 1.0f;
        }

        outputPeak = std::max(outputPeak, std::abs(x));
        samples[i] = x;
    }

    meters_.inputPeakDb = inputPeak > 0.0f ? linToDb(inputPeak) : kSilenceFloorDb;
    meters_.outputPeakDb = outputPeak > 0.0f ? linToDb(outputPeak) : kSilenceFloorDb;
    meters_.gateReductionDb = worstGate;
    meters_.deEsserReductionDb = worstDeEsser;
    meters_.compressorReductionDb = worstComp;
    meters_.limiterReductionDb = worstLimiter;
    meters_.gateOpen = gateIsOpen_;
}

// --- Presets ------------------------------------------------------------------

MicSettings presetSettings(MicPreset preset) {
    MicSettings s;
    switch (preset) {
        case MicPreset::Raw:
            // Every stage off. Not a "subtle" preset — a way to hear what the
            // microphone actually sounds like, and to rule Sonero out when
            // something sounds wrong.
            s.noiseSuppression = false;
            s.highPass.enabled = false;
            s.gate.enabled = false;
            s.deEsser.enabled = false;
            s.compressor.enabled = false;
            s.limiter.enabled = false;
            break;

        case MicPreset::Podcast:
            // A close microphone in a quiet room: gentle everything, and enough
            // compression to even out a person who leans in and out.
            s.highPass.frequencyHz = 80.0f;
            s.gate.thresholdDb = -45.0f;
            s.gate.ratio = 3.0f;
            s.gate.releaseMs = 250.0f;
            s.deEsser.thresholdDb = -28.0f;
            s.compressor.thresholdDb = -18.0f;
            s.compressor.ratio = 3.0f;
            s.compressor.attackMs = 10.0f;
            s.compressor.releaseMs = 150.0f;
            s.limiter.ceilingDb = -1.0f;
            break;

        case MicPreset::Streaming:
            // Louder and faster, and it has to survive a keyboard, a chair and a
            // room that was never treated. The gate works harder and the
            // compressor is quicker, at the cost of some dynamics.
            s.highPass.frequencyHz = 100.0f;
            s.gate.thresholdDb = -38.0f;
            s.gate.ratio = 5.0f;
            s.gate.holdMs = 80.0f;
            s.gate.releaseMs = 150.0f;
            s.deEsser.thresholdDb = -30.0f;
            s.deEsser.ratio = 4.0f;
            s.compressor.thresholdDb = -22.0f;
            s.compressor.ratio = 4.0f;
            s.compressor.attackMs = 5.0f;
            s.compressor.releaseMs = 100.0f;
            s.limiter.ceilingDb = -1.0f;
            break;

        case MicPreset::Meeting:
            // Intelligibility over everything: cut low, gate hard so the room and
            // the other end's echo stay out, compress firmly so a quiet speaker
            // is still heard on a laptop speaker.
            s.highPass.frequencyHz = 120.0f;
            s.gate.thresholdDb = -35.0f;
            s.gate.ratio = 6.0f;
            s.gate.holdMs = 60.0f;
            s.gate.releaseMs = 120.0f;
            s.deEsser.enabled = false;
            s.compressor.thresholdDb = -24.0f;
            s.compressor.ratio = 5.0f;
            s.compressor.attackMs = 5.0f;
            s.compressor.releaseMs = 90.0f;
            s.limiter.ceilingDb = -1.0f;
            break;

        case MicPreset::Custom:
            break;  // the defaults, which the user is about to change anyway
    }
    return s;
}

std::string_view micPresetName(MicPreset preset) {
    switch (preset) {
        case MicPreset::Raw: return "Raw";
        case MicPreset::Podcast: return "Podcast";
        case MicPreset::Streaming: return "Streaming";
        case MicPreset::Meeting: return "Meeting";
        case MicPreset::Custom: return "Custom";
    }
    return "Custom";
}

}  // namespace sonar::dsp
