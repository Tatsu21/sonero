#pragma once

#include <array>
#include <string_view>

struct DenoiseState;

namespace sonar::dsp {

// Learned noise suppression with RNNoise, which decides per band how much of each
// 10 ms frame is voice. This class owns the state and feeds frames of the size and
// scale the model expects. Without RNNoise in the build, `available()` is false and
// `process()` passes audio through.
class NoiseSuppressor {
public:
    NoiseSuppressor();
    ~NoiseSuppressor();

    NoiseSuppressor(const NoiseSuppressor&) = delete;
    NoiseSuppressor& operator=(const NoiseSuppressor&) = delete;

    // Whether this build can suppress anything at all, and what it would use.
    [[nodiscard]] static bool available();
    [[nodiscard]] static std::string_view backendName();

    // RNNoise runs only at 48 kHz; at any other rate the stage reports itself unusable.
    void prepare(float sampleRate);
    [[nodiscard]] bool usable() const { return usable_; }

    void setEnabled(bool on) { enabled_ = on; }
    [[nodiscard]] bool enabled() const { return enabled_; }

    // Mono, in place. Allocation-free; safe on the realtime thread.
    void process(float* samples, int count);

    void reset();

    // One frame of delay while active — the model cannot answer for a frame it
    // has not seen all of. Zero when off or unusable.
    [[nodiscard]] int latencySamples() const;

    // How much the model removed from the last frame, in decibels (never
    // positive), and its own estimate that the frame contained speech.
    [[nodiscard]] float reductionDb() const { return reductionDb_; }
    [[nodiscard]] float speechProbability() const { return speechProbability_; }

private:
    // RNNoise's frame (480 samples at 48 kHz) and scale (16-bit integers, not -1..1).
    static constexpr int kFrameSize = 480;
    static constexpr float kInt16Scale = 32768.0f;

    DenoiseState* state_ = nullptr;
    bool enabled_ = true;
    bool usable_ = false;
    bool primed_ = false;  // false until the first full frame has been processed

    std::array<float, kFrameSize> inFrame_{};
    std::array<float, kFrameSize> outFrame_{};
    int fill_ = 0;
    int outPos_ = 0;

    float reductionDb_ = 0.0f;
    float speechProbability_ = 0.0f;
};

}  // namespace sonar::dsp
