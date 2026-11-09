#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <spa/utils/hook.h>  // struct spa_hook (value member)

#include "audio/FrameRing.h"
#include "audio/Handoff.h"
#include "dsp/ChannelChain.h"

struct pw_core;
struct pw_stream;

namespace sonar::audio {

// What the mixer draws for one channel. Reading clears the peak holds, so a
// 30 Hz poll sees every transient.
struct ChannelProcessorLevels {
    float peakLeft = 0.0f;
    float peakRight = 0.0f;
    float inputPeak = 0.0f;
    float limiterReductionDb = 0.0f;
    int clippedSamples = 0;
    float bassLowDb = -120.0f;
    float bassHarmonicsDb = -120.0f;
    bool bassActive = false;
    bool active = false;  // the node pair exists and audio is flowing through it
};

// One output channel: a pair of native PipeWire nodes with dsp::ChannelChain
// between them (replacing module-filter-chain + a LADSPA plugin + channelVolumes).
// Node properties are exact: node.want-driver stalled the whole graph (a virtual
// sink has no device to drive it), so it stays off; node.group/link-group are set
// so the sink and the output share the output device's clock; the output is
// passive, so the pair sleeps while nothing plays into it.
class ChannelProcessor {
public:
    ChannelProcessor();
    ~ChannelProcessor();

    ChannelProcessor(const ChannelProcessor&) = delete;
    ChannelProcessor& operator=(const ChannelProcessor&) = delete;

    // Create the pair on an existing core. Call with the PipeWire thread loop
    // locked. Returns false with nothing left behind if either node fails.
    bool start(pw_core* core, const std::string& nodeName, const std::string& description);

    // Tear the pair down. Also called by the destructor; safe to call twice.
    void stop();

    [[nodiscard]] bool running() const { return sink_ != nullptr; }
    [[nodiscard]] std::string nodeName() const { return nodeName_; }

    // All three cross to the audio thread through a Handoff.
    void setSettings(const dsp::ChannelChainSettings& settings);
    [[nodiscard]] dsp::ChannelChainSettings settings() const;

    void setEqualizer(const dsp::EqSettings& eq);
    [[nodiscard]] dsp::EqSettings equalizer() const;

    void setBass(const dsp::BassStageSettings& bass);
    [[nodiscard]] dsp::BassStageSettings bass() const;

    [[nodiscard]] ChannelProcessorLevels takeLevels();

    // Delay the chain adds, in milliseconds — the limiter's lookahead, fixed at
    // prepare() time and so safe to read from another thread.
    [[nodiscard]] float latencyMs() const;

private:
    static void onSinkProcess(void* data);
    static void onOutProcess(void* data);
    void processSink();
    void processOut();

    // The processing thread's own copy. Touched only there.
    dsp::ChannelChain chain_;

    // What the UI asked for, picked up by the sink callback.
    Handoff<dsp::ChannelChainSettings> settings_;
    Handoff<dsp::EqSettings> eq_;
    Handoff<dsp::BassStageSettings> bass_;

    // The block the chain works in; a member, to keep 64 KB off the realtime stack.
    std::vector<float> scratch_;

    // Between the two nodes: the sink side writes, the output side reads.
    FrameRing<2> ring_;
    bool primed_ = false;  // output side only: playing, or still filling up

    std::atomic<float> peakLeft_{0.0f};
    std::atomic<float> peakRight_{0.0f};
    std::atomic<float> inputPeak_{0.0f};
    std::atomic<float> limiterReduction_{0.0f};
    std::atomic<int> clipped_{0};
    std::atomic<float> bassLowDb_{-120.0f};
    std::atomic<float> bassHarmonicsDb_{-120.0f};
    std::atomic<bool> bassActive_{false};
    std::atomic<bool> sawAudio_{false};

    pw_core* core_ = nullptr;
    pw_stream* sink_ = nullptr;  // Audio/Sink: what applications play into
    pw_stream* out_ = nullptr;   // the playback side, routed to a real device
    spa_hook sinkListener_{};
    spa_hook outListener_{};
    std::string nodeName_;
    std::string description_;
};

}  // namespace sonar::audio
