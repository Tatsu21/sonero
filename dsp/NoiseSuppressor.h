#pragma once

#include <array>
#include <string_view>

struct DenoiseState;

namespace sonar::dsp {

// Learned noise suppression: the one stage of the microphone chain that is a
// model rather than arithmetic, and the reason a keyboard, a fan and a room stop
// being audible while the voice does not change.
//
// The model is RNNoise — a small recurrent network, trained on the standard
// speech/noise corpora, that decides per frequency band how much of each 10 ms
// frame is voice. It is not written here and it is not trained here: matching it
// from scratch needs a corpus measured in hundreds of gigabytes and weeks of GPU
// time, and the result would lose. What this class does is own the state, feed
// the model frames of the size and scale it expects, and keep the signal intact
// when no model is available.
//
// When the build has no RNNoise, `available()` is false and `process()` passes
// the audio through untouched — the UI then says the stage is unavailable rather
// than showing a switch that does nothing.
class NoiseSuppressor {
public:
    NoiseSuppressor();
    ~NoiseSuppressor();

    NoiseSuppressor(const NoiseSuppressor&) = delete;
    NoiseSuppressor& operator=(const NoiseSuppressor&) = delete;

    // Whether this build can suppress anything at all, and what it would use.
    [[nodiscard]] static bool available();
    [[nodiscard]] static std::string_view backendName();

    // RNNoise is a 48 kHz model, full stop — there is no resampling inside it.
    // At any other rate the stage reports itself unusable rather than quietly
    // processing at the wrong pitch.
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
    // RNNoise works in frames of 480 samples at 48 kHz, in the scale of 16-bit
    // integers rather than the -1..1 the rest of the chain uses. Both of those
    // are the model's, not ours.
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
