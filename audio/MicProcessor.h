#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <spa/utils/hook.h>  // struct spa_hook (value member)

#include "dsp/MicChain.h"
#include "dsp/NoiseSuppressor.h"

struct pw_core;
struct pw_stream;

namespace sonar::audio {

// What the microphone page draws, sampled from the realtime thread.
//
// Peaks are "loudest since you last looked": reading them clears them, so a UI
// polling at 30 Hz sees every transient instead of whatever happened to be
// under the needle at the moment it asked.
struct MicLevels {
    float inputPeak = 0.0f;   // linear, before any processing
    float outputPeak = 0.0f;  // linear, what applications receive
    float gateReductionDb = 0.0f;
    float deEsserReductionDb = 0.0f;
    float compressorReductionDb = 0.0f;
    float limiterReductionDb = 0.0f;
    float noiseReductionDb = 0.0f;
    float speechProbability = 0.0f;  // the noise model's own estimate, 0..1
    bool gateOpen = false;
    bool active = false;  // the node pair exists and audio is flowing through it
};

// The microphone node: a capture stream pulling from the real input device, the
// processing chain, and a second stream that applications see as an ordinary
// Audio/Source.
//
// This is the same two-node shape PipeWire's own module-filter-chain builds —
// `sonero_microphone.input` feeding `sonero_microphone` — rebuilt here so the
// processing between them is ours. That matters for more than the DSP: the meters
// come straight out of the chain that produced them, so the page can show gain
// reduction per stage instead of a level and a guess.
//
// Everything after start() happens on PipeWire's realtime thread. Settings cross
// over without ever blocking it: the UI parks a copy, the audio thread picks it
// up when it can, and a thread that cannot get the lock keeps running with what
// it already has rather than waiting and missing its deadline.
class MicProcessor {
public:
    MicProcessor();
    ~MicProcessor();

    MicProcessor(const MicProcessor&) = delete;
    MicProcessor& operator=(const MicProcessor&) = delete;

    // Create the pair on an existing core. Call with the PipeWire thread loop
    // locked. `captureTarget` is the real input device to pull from; empty leaves
    // the capture side unconnected, which is deliberate for Bluetooth — binding a
    // BT microphone forces the headset into HFP and destroys A2DP playback.
    bool start(pw_core* core, const std::string& nodeName, const std::string& description,
               const std::string& captureTarget);

    // Tear the pair down. Also called by the destructor; safe to call twice.
    void stop();

    [[nodiscard]] bool running() const { return capture_ != nullptr; }

    // Re-point the capture side at a different input device. Rebuilds only the
    // capture stream, so applications listening to the source do not see it
    // disappear and re-appear.
    bool setCaptureTarget(pw_core* core, const std::string& captureTarget);
    [[nodiscard]] std::string captureTarget() const;

    void setSettings(const dsp::MicSettings& settings);
    [[nodiscard]] dsp::MicSettings settings() const;

    void setEqualizer(const dsp::EqSettings& eq);
    [[nodiscard]] dsp::EqSettings equalizer() const;

    void setNoiseSuppressionEnabled(bool on);
    [[nodiscard]] bool noiseSuppressionEnabled() const;
    [[nodiscard]] static bool noiseSuppressionAvailable();
    [[nodiscard]] static std::string_view noiseSuppressionBackend();

    // Reading clears the peak holds.
    [[nodiscard]] MicLevels takeLevels();

    // Delay the chain adds, in milliseconds — the limiter's lookahead plus the
    // noise model's frame. Shown in the UI, because someone monitoring themselves
    // needs to know why they are hearing themselves late.
    [[nodiscard]] float latencyMs() const;

private:
    static void onCaptureProcess(void* data);
    static void onPlaybackProcess(void* data);
    void processCapture();
    void processPlayback();

    bool createCapture(pw_core* core, const std::string& captureTarget);
    void destroyCapture();

    // The processing thread's own copies. Touched only there.
    dsp::MicChain chain_;
    dsp::NoiseSuppressor noise_;

    // The handoff. `pending*` is written by the UI under the mutex and read by
    // the audio thread with try_lock.
    mutable std::mutex settingsMutex_;
    dsp::MicSettings pendingSettings_;
    dsp::EqSettings pendingEq_;
    std::atomic<bool> settingsDirty_{false};
    std::atomic<bool> eqDirty_{false};
    std::atomic<bool> noiseEnabled_{true};

    // Between the two streams. One writer, one reader, no locks: the capture
    // callback fills it, the source callback drains it.
    // The block the chain works in. A member rather than a local, because a
    // 32 KB array on the stack of the realtime callback is zeroed on every
    // wake-up for no reason — this one is allocated once, at construction.
    std::vector<float> scratch_;

    std::vector<float> ring_;
    std::atomic<std::uint32_t> ringWrite_{0};
    std::atomic<std::uint32_t> ringRead_{0};

    std::atomic<float> inputPeak_{0.0f};
    std::atomic<float> outputPeak_{0.0f};
    std::atomic<float> gateReduction_{0.0f};
    std::atomic<float> deEsserReduction_{0.0f};
    std::atomic<float> compressorReduction_{0.0f};
    std::atomic<float> limiterReduction_{0.0f};
    std::atomic<float> noiseReduction_{0.0f};
    std::atomic<float> speechProbability_{0.0f};
    std::atomic<bool> gateOpen_{false};
    std::atomic<bool> sawAudio_{false};

    pw_core* core_ = nullptr;
    pw_stream* capture_ = nullptr;
    pw_stream* playback_ = nullptr;
    spa_hook captureListener_{};
    spa_hook playbackListener_{};
    std::string nodeName_;
    std::string description_;
    std::string captureTarget_;
    mutable std::mutex targetMutex_;
};

}  // namespace sonar::audio
