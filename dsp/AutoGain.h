#pragma once

namespace sonar::dsp {

// Automatic gain for one channel, built the way normalisation is done in practice
// rather than as a compressor: it listens for a few seconds, commits to a single
// gain, and then leaves it alone.
//
// A loop that keeps correcting is audible — the level breathes with the music,
// which is the "pumping" everyone recognises. So this one has two phases:
//
//   Observing  collect the loudest peaks of the material, without moving.
//   Holding    apply one constant gain, and stay there.
//
// It leaves Holding only when the material really has changed: the level must sit
// far from the target for several seconds, not merely spike. A loud chorus does
// not retrigger it; the next track does.
//
// One thing does act immediately — if a peak comes close to full scale the gain is
// pulled down at once, because the alternative is audible clipping. That is a
// safety limiter, not the normaliser, and it is the only fast movement here.
//
// The peak it is fed comes from the channel's *output*, after the equalizer and
// after this gain, so what it measures is what actually leaves the channel.
class AutoGain {
public:
    struct Config {
        float targetPeak = 0.7f;         // ≈ -3 dBFS
        float minGainDb = -20.0f;
        // Boost is allowed, but kept modest: the measurement is divided by the
        // channel's headroom before it gets here, so the loop judges the source
        // rather than the already-attenuated output. Without that correction a
        // peak target would ask for boost on nearly everything and simply park
        // at this ceiling.
        float maxGainDb = 6.0f;

        float observeSeconds = 3.0f;     // listen this long before committing
        float rampDbPerSec = 3.0f;       // how fast we move to a newly chosen gain

        // Re-evaluate only when the level is this far off for this long.
        float reevaluateDeltaDb = 5.0f;
        float reevaluateSeconds = 4.0f;

        // Safety limiter: above this, duck immediately at this rate.
        float ceilingPeak = 0.97f;
        float duckDbPerSec = 30.0f;

        float silenceFloor = 0.003f;     // ≈ -50 dBFS: ignore, do not adapt
        float envelopeHalfLife = 0.4f;
    };

    enum class Phase { Observing, Holding };

    AutoGain() = default;
    explicit AutoGain(Config config) : config_(config) {}

    // Feed the peak measured over the last `dtSeconds`; returns the gain in dB.
    float update(float peak, float dtSeconds);

    void reset(float gainDb = 0.0f);
    [[nodiscard]] float gainDb() const { return gainDb_; }
    [[nodiscard]] Phase phase() const { return phase_; }
    [[nodiscard]] bool settled() const { return phase_ == Phase::Holding; }

private:
    void commit();  // turn what we observed into the gain we will hold

    Config config_;
    Phase phase_ = Phase::Observing;
    float gainDb_ = 0.0f;
    float targetGainDb_ = 0.0f;   // what we are ramping towards
    float envelope_ = 0.0f;
    float observedPeak_ = 0.0f;   // loudest envelope seen while observing
    float observedFor_ = 0.0f;    // seconds of actual signal observed
    float offTargetFor_ = 0.0f;   // seconds the level has been far from target
};

}  // namespace sonar::dsp
