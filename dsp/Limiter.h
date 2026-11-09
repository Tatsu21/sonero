#pragma once

#include <vector>

namespace sonar::dsp {

// A brick-wall lookahead limiter, mono, one per channel side. The channel EQ has no
// gain ceiling and harmonic bass adds signal, so either can push mastered material
// past full scale. Bounds what leaves the chain, never what the EQ may do;
// switchable, with `clipped()` reporting what got through unprotected.

class Limiter {
public:
    Limiter();

    // Sample rate in Hz. Sizes the lookahead window and clears the state, so it
    // is a graph change rather than something to call per block.
    void prepare(float sampleRate);

    // `ceilingDb` is where the wall is (0 dB or below); `releaseMs` how fast the
    // gain comes back after a peak has passed.
    void setCeilingDb(float ceilingDb);
    void setReleaseMs(float releaseMs);
    void setEnabled(bool on) { enabled_ = on; }
    [[nodiscard]] bool enabled() const { return enabled_; }

    void process(const float* in, float* out, int count);

    void reset();

    // How far the limiter is holding the signal down right now, in decibels
    // (never positive), from the most recently processed block.
    [[nodiscard]] float reductionDb() const { return reductionDb_; }

    // Samples that left past full scale: zero while enabled, counted (never fixed) when off.
    [[nodiscard]] int clipped() const { return clipped_; }

    // The delay the lookahead costs, in samples. Fixed by prepare() and never
    // written again, so it is safe to read from another thread.
    [[nodiscard]] int latencySamples() const { return enabled_ ? lookahead_ : 0; }
    [[nodiscard]] int lookaheadSamples() const { return lookahead_; }

private:
    float sampleRate_ = 48000.0f;
    bool enabled_ = true;
    float ceilingDb_ = -1.0f;
    float ceilingLin_ = 0.891f;
    float releaseMs_ = 50.0f;
    float releaseCoef_ = 0.0f;

    std::vector<float> delay_;
    int lookahead_ = 0;
    int write_ = 0;
    float gain_ = 1.0f;

    float reductionDb_ = 0.0f;
    int clipped_ = 0;
};

}  // namespace sonar::dsp
