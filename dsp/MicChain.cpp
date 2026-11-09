#include "dsp/MicChain.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "dsp/DspMath.h"

namespace sonar::dsp {

namespace {
// Everything below this is silence as far as the meters and detectors are
// concerned. Also the floor for the logarithm, which has no answer at zero.
constexpr float kSilenceFloorDb = -120.0f;

// How far the gate may pull down; a high-ratio expander otherwise reaches -200 dB.
constexpr float kMaxGateReductionDb = -60.0f;

// The level auto-makeup restores: where sensibly trimmed speech peaks.
constexpr float kAutoMakeupReferenceDb = -10.0f;

// A soft-knee compressor's static curve in dB: the (non-positive) gain to apply.
[[nodiscard]] float compressorGainDb(float levelDb, float thresholdDb, float ratio,
                                     float kneeDb) {
    const float over = levelDb - thresholdDb;
    if (2.0f * over < -kneeDb) return 0.0f;
    if (kneeDb > 0.0f && 2.0f * std::abs(over) <= kneeDb) {
        const float x = over + kneeDb * 0.5f;
        return (1.0f / ratio - 1.0f) * x * x / (2.0f * kneeDb);
    }
    return over * (1.0f / ratio - 1.0f);
}

}  // namespace

// --- Construction -------------------------------------------------------------

MicChain::MicChain() { prepare(48000.0f); }

void MicChain::prepare(float sampleRate) {
    sampleRate_ = sampleRate > 0.0f ? sampleRate : 48000.0f;
    // Its 2 ms lookahead is fixed: latency the user pays for, and long enough to
    // catch a plosive's rise.
    limiter_.prepare(sampleRate_);
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
    limiter_.reset();
    meters_ = MicMeters{};
}

int MicChain::latencySamples() const {
    return settings_.limiter.enabled ? limiter_.lookaheadSamples() : 0;
}

void MicChain::rebuildCoefficients() {
    const float sr = sampleRate_;
    const float twoPi = 2.0f * std::numbers::pi_v<float>;

    // High-pass with Butterworth Q: a resonance above the corner would add the boom
    // it exists to remove.
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

    // Band-pass (Q = 2) over the sibilant range, for the de-esser's detector only.
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
    limiter_.setEnabled(settings_.limiter.enabled);
    limiter_.setCeilingDb(settings_.limiter.ceilingDb);
    limiter_.setReleaseMs(settings_.limiter.releaseMs);
    rebuildEqualizer();
}

void MicChain::rebuildEqualizer() {
    // Runs on the audio thread on every slider move, so it must not allocate:
    // prepare() reserved room for the largest band count.
    const std::vector<float>& freqs = standardFrequenciesRef(eq_.bandCount);
    // Resized, not reassigned: a band that was already running keeps its state,
    // so moving one slider does not click every other filter in the cascade.
    if (eqBands_.size() != freqs.size()) {
        eqBands_.resize(freqs.size());
    }

    const float sr = sampleRate_;
    const float twoPi = 2.0f * std::numbers::pi_v<float>;
    // Q follows the band spacing — a third-octave Q on octave-spaced bands leaves holes.
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
    if (samples == nullptr || count <= 0) return;

    const MicSettings& s = settings_;

    if (s.muted) {
        std::fill(samples, samples + count, 0.0f);
        meters_ = MicMeters{};
        return;
    }

    const float inputGain = dbToLin(s.inputGainDb);
    const float outputGain = dbToLin(s.outputGainDb);

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
            // The detector hears only the sibilant band; the gain applies to the whole signal,
            // so an S sounds quieter, not filtered.
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
        // Before the compressor, so it reacts to the voice as heard. No headroom
        // compensation: the limiter at the end already guards the ceiling, and both
        // together cancelled every boost.
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
            // Smoothed in decibels, the domain the ear hears.
            const float coefG = targetDb < compGainDb_ ? compAttackCoef_ : compReleaseCoef_;
            compGainDb_ = targetDb + coefG * (compGainDb_ - targetDb);
            x *= dbToLin(compGainDb_);
            worstComp = std::min(worstComp, compGainDb_);
        } else {
            compGainDb_ = 0.0f;
        }

        samples[i] = x * makeupLin * outputGain;
    }

    // --- Limiter, last: nothing leaves above the ceiling. In place is safe, it
    // reads each sample before writing it.
    limiter_.process(samples, samples, count);
    for (int i = 0; i < count; ++i) {
        outputPeak = std::max(outputPeak, std::abs(samples[i]));
    }

    meters_.inputPeakDb = inputPeak > 0.0f ? linToDb(inputPeak) : kSilenceFloorDb;
    meters_.outputPeakDb = outputPeak > 0.0f ? linToDb(outputPeak) : kSilenceFloorDb;
    meters_.gateReductionDb = worstGate;
    meters_.deEsserReductionDb = worstDeEsser;
    meters_.compressorReductionDb = worstComp;
    meters_.limiterReductionDb = limiter_.reductionDb();
    meters_.gateOpen = gateIsOpen_;
}

// --- Presets ------------------------------------------------------------------

MicSettings presetSettings(MicPreset preset) {
    MicSettings s;
    switch (preset) {
        case MicPreset::Raw:
            // Every stage off: what the microphone really sounds like.
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
            // Louder and faster, for an untreated room with a keyboard: harder gate, quicker
            // compressor.
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
            // Intelligibility first: cut low, gate hard, compress firmly.
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
