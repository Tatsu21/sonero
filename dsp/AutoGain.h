#pragma once

namespace sonar::dsp {

// Automatic gain for one channel: take as much level as the material allows, and
// give it back the moment the peaks say otherwise.
//
// This rides a gain from outside the audio path — it reads a meter and writes a
// volume — so it learns about a peak only after that peak has already played. No
// amount of reaction speed fixes that; the only defence is margin. So the climb
// deliberately stops well short of full scale, and what it remembers about the
// loudest passage fades slowly, on the order of a track rather than a bar:
//
//   below climbTargetPeak   room to grow      climb, slowly
//   in between              leave it alone    the dead band that keeps it still
//   above ceilingPeak       about to clip     duck now, with a margin on top
//
// Between the two thresholds nothing moves at all, which is what stops the gain
// breathing with the music. It never normalises: material that is already loud is
// left where it is rather than pulled down to some target, and material that is
// quiet is lifted only as far as `maxGainDb` allows.
//
// Everything is judged on the *measured output* of the channel — after the
// equalizer, after the fader, after the fixed filter reserve — because that is the
// signal that reaches the device. It also closes the loop: the measurement already
// contains the gain in force, so each decision is a correction to it.
class AutoGain {
public:
    struct Config {
        // Where the climb aims. Well below full scale on purpose: this is the
        // headroom that absorbs the passage that turns out to be louder than
        // anything heard so far, which the loop cannot see coming.
        float climbTargetPeak = 0.5f;  // -6 dBFS

        // Above this the output is close enough to full scale to be in danger.
        float ceilingPeak = 0.89f;  // -1 dBFS

        // When ducking, go this much further than strictly needed, so the next
        // transient of the same size has somewhere to land.
        float duckMarginDb = 1.0f;

        float minGainDb = -20.0f;
        float maxGainDb = 6.0f;  // as much boost as the channel gain allows

        // Down: as good as instantaneous. A gain that slides down over 150 ms
        // leaves every crest in that window flat-topped, which is the clipping you
        // hear; the step itself is far less objectionable than the distortion, and
        // it only ever happens on material that was about to clip anyway.
        float duckDbPerSec = 400.0f;
        float climbDbPerSec = 1.0f;   // up: slow enough to be inaudible
        float climbDelaySeconds = 2.0f;

        // How long the loudest passage is remembered while deciding whether there
        // is room to climb. Long, because a chorus must still count during the
        // verse that follows it.
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
    // The gain a peak demanded. Held until it is actually reached: the ramp has to
    // outlive the peak that caused it, or the margin below the ceiling would never
    // be applied — the overshoot stops the moment the level dips back under.
    float duckTarget_ = 6.0f;
    float sinceDuck_ = 0.0f;   // seconds since the last reduction
    float silentFor_ = 0.0f;   // seconds of continuous silence
};

}  // namespace sonar::dsp
