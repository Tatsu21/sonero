#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <spa/utils/hook.h>  // struct spa_hook (value member)

#include "dsp/ChannelChain.h"

struct pw_core;
struct pw_stream;

namespace sonar::audio {

// What the mixer draws for one channel, sampled from the realtime thread.
// Reading clears the peak holds, so a UI polling at 30 Hz sees every transient
// instead of whatever happened to be under the needle when it asked.
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

// One output channel, as a pair of native PipeWire nodes with dsp::ChannelChain
// running between them.
//
// This replaces module-filter-chain. The old arrangement needed three mechanisms
// to do one channel's work — the daemon's builtin biquads for the equalizer, a
// LADSPA plugin dlopened into the same graph for the harmonic bass, and the sink
// node's channelVolumes for the fader — and the bass could only be a plugin
// because a filter graph is where it had to live. Owning the node means owning
// the whole chain, so there is no plugin binary to find or install, no borrowed
// plugin ABI, and no round trip through node properties to move a slider.
//
// The node properties here are exact, and the reason is worth recording: an
// earlier attempt at this set node.group, node.link-group and node.want-driver on
// both ends, copied from MicProcessor. Those belong there, because the
// microphone's capture side attaches to a real device and that device drives the
// group. A virtual sink has no such device, so the group waits forever for a
// driver that never arrives — and PipeWire does not fail that quietly: it stops
// answering every client on the machine for as long as the node exists. None of
// those three properties is set below, which is also what module-filter-chain
// does, and what a standalone probe confirmed before any of this reached the app.
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

    // All three cross to the audio thread the same way: the caller parks a copy
    // under the mutex and the audio thread picks it up when it can. A thread
    // that cannot get the lock keeps running with what it already has rather
    // than waiting and missing its deadline.
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

    mutable std::mutex settingsMutex_;
    dsp::ChannelChainSettings pendingSettings_;
    dsp::EqSettings pendingEq_;
    dsp::BassStageSettings pendingBass_;
    std::atomic<bool> settingsDirty_{false};
    std::atomic<bool> eqDirty_{false};
    std::atomic<bool> bassDirty_{false};

    // The block the chain works in. A member rather than a local, because a
    // 64 KB array on the stack of a realtime callback is touched on every
    // wake-up for no reason.
    std::vector<float> scratch_;

    // Between the two nodes. One writer, one reader, no locks. Counted in frames
    // so a partial write can never split a stereo pair and swap the channels for
    // the rest of the session.
    std::vector<float> ring_;
    std::atomic<std::uint32_t> ringWrite_{0};
    std::atomic<std::uint32_t> ringRead_{0};

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
