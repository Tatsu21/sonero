#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <spa/utils/hook.h>  // struct spa_hook (value member)

#include "audio/FrameRing.h"
#include "audio/Handoff.h"
#include "dsp/MicChain.h"
#include "dsp/NoiseSuppressor.h"

struct pw_core;
struct pw_stream;

namespace sonar::audio {

// What the microphone page draws. Peaks are "loudest since last read": reading
// clears them, so a 30 Hz poll sees every transient.
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

// The microphone node: a capture stream from the real input, the processing chain,
// and a second stream apps see as an Audio/Source (the module-filter-chain shape,
// with our own DSP, so meters come straight from the chain). Everything after
// start() runs on PipeWire's realtime thread; settings cross via Handoff.
class MicProcessor {
public:
    MicProcessor();
    ~MicProcessor();

    MicProcessor(const MicProcessor&) = delete;
    MicProcessor& operator=(const MicProcessor&) = delete;

    // Create the pair on an existing core, thread loop locked. An empty `captureTarget`
    // leaves the capture unconnected — deliberate for Bluetooth, which binding would
    // force into HFP.
    bool start(pw_core* core, const std::string& nodeName, const std::string& description,
               const std::string& captureTarget);

    // Tear the pair down. Also called by the destructor; safe to call twice.
    void stop();

    [[nodiscard]] bool running() const { return capture_ != nullptr; }

    // Re-point the capture side at another input; only that stream is rebuilt, so apps
    // listening to the source never see it vanish.
    bool setCaptureTarget(pw_core* core, const std::string& captureTarget);
    [[nodiscard]] std::string captureTarget() const;

    // Passive by default so an unused mic can suspend; either of these keeps it
    // running. Thread loop locked.
    void setLive(pw_core* core, bool live);
    // The chain's output on the default sink. Created and destroyed with the
    // switch, so nobody who never uses it has a stream in their graph.
    void setMonitor(pw_core* core, bool on, float level);

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

    // Delay the chain adds in ms (limiter lookahead + noise frame), shown to anyone
    // monitoring themselves.
    [[nodiscard]] float latencyMs() const;

private:
    static void onCaptureProcess(void* data);
    static void onPlaybackProcess(void* data);
    static void onMonitorProcess(void* data);
    void processCapture();
    void processPlayback();
    void processMonitor();

    bool createCapture(pw_core* core, const std::string& captureTarget);
    void destroyCapture();
    bool createMonitor(pw_core* core);
    void destroyMonitor();
    void applyKeepRunning(pw_core* core);  // rebuild the capture if it changed

    // The processing thread's own copies. Touched only there.
    dsp::MicChain chain_;
    dsp::NoiseSuppressor noise_;

    // What the UI asked for, picked up by the capture callback.
    Handoff<dsp::MicSettings> settings_;
    Handoff<dsp::EqSettings> eq_;
    std::atomic<bool> noiseEnabled_{true};

    // The block the chain works in, allocated once, to keep 32 KB off the realtime stack.
    std::vector<float> scratch_;

    // Between the two streams: the capture callback fills it, the source
    // callback drains it.
    FrameRing<1> ring_;

    // The same arrangement for the monitor, which is a separate ring because it
    // has a different reader: the output device's clock, not the source's.
    FrameRing<1> monitorRing_;
    std::atomic<bool> monitorOn_{false};
    std::atomic<float> monitorGain_{1.0f};

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
    pw_stream* monitor_ = nullptr;
    spa_hook captureListener_{};
    spa_hook playbackListener_{};
    spa_hook monitorListener_{};
    bool live_ = false;
    bool captureKeptRunning_ = false;  // what the current capture stream was made with
    std::string nodeName_;
    std::string description_;
    std::string captureTarget_;
    mutable std::mutex targetMutex_;
};

}  // namespace sonar::audio
