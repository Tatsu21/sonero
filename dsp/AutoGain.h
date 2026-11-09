#pragma once

namespace sonar::dsp {

// Automatic gain for one channel. It reads a meter and writes a gain, so it hears a
// peak only after it played; margin is the only defence. Judged on the channel's
// measured output (post-EQ, post-fader):
//   below climbTargetPeak   climb, slowly
//   in between              leave it alone (the dead band that stops breathing)
//   above ceilingPeak       duck now, with a margin on top
// It never normalises: loud material stays loud, quiet is lifted up to `maxGainDb`.
class AutoGain {
public:
    struct Config {
        // Where the climb aims: well below full scale, as headroom for a louder passage
        // the loop cannot see coming.
        float climbTargetPeak = 0.5f;  // -6 dBFS

        // Above this the output is close enough to full scale to be in danger.
        float ceilingPeak = 0.89f;  // -1 dBFS

        // When ducking, go this much further than strictly needed, so the next
        // transient of the same size has somewhere to land.
        float duckMarginDb = 1.0f;

        float minGainDb = -20.0f;
        float maxGainDb = 6.0f;  // as much boost as the channel gain allows

        // Down: instantaneous. A slow slide down flat-tops every crest in the window,
        // which is the clipping you hear.
        float duckDbPerSec = 400.0f;
        float climbDbPerSec = 1.0f;   // up: slow enough to be inaudible
        float climbDelaySeconds = 2.0f;

        // How long the loudest passage is remembered: a chorus must still count during
        // the next verse.
        float peakMemoryHalfLife = 30.0f;

        float silenceFloor = 0.003f;  // ≈ -50 dBFS: says nothing, so change nothing
        // Silence this long means the material is over — forget the old peaks so
        // the next track is judged on its own.
        float memoryResetSeconds = 3.0f;
    };

    AutoGain() = default;
    explicit AutoGain(Config config) : config_(config) {}

    // Feed the peak the meter saw on the channel *output* over the last
    // `dtSeconds`; returns the gain in dB.
    float update(float outputPeak, float dtSeconds);

    void reset(float gainDb = 0.0f);
    [[nodiscard]] float gainDb() const { return gainDb_; }

private:
    Config config_;
    float gainDb_ = 0.0f;
    float peakMemory_ = 0.0f;  // slowly decaying peak hold, gates the climb
    // The gain a peak demanded, held until reached, so the margin below the ceiling
    // is actually applied.
    float duckTarget_ = 6.0f;
    float sinceDuck_ = 0.0f;   // seconds since the last reduction
    float silentFor_ = 0.0f;   // seconds of continuous silence
};

}  // namespace sonar::dsp
