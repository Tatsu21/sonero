#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <spa/utils/hook.h>  // struct spa_hook (value member)

#include "audio/IAppRouter.h"
#include "audio/IAudioBackend.h"
#include "audio/IAudioDevices.h"
#include "audio/IChannelController.h"
#include "audio/IDeviceFormats.h"
#include "audio/IEqualizerController.h"
#include "audio/IMicrophoneController.h"
#include "audio/ChannelProcessor.h"
#include "audio/MicProcessor.h"
#include "dsp/BassStage.h"

// Opaque libpipewire types — forward declared so this header stays lightweight.
struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_core_info;
struct pw_registry;
struct pw_metadata;
struct pw_impl_module;
struct pw_proxy;
struct pw_node;
struct pw_device;
struct pw_node_info;
struct spa_dict;
struct spa_pod;

namespace sonar::audio {

// PipeWire backend: owns the thread loop and connection, one ChannelProcessor per
// channel (a virtual sink forwarding to the default or a pinned device), and routes
// application streams onto them via the "default" metadata target.object key.
// All libpipewire access happens on, or under, the thread loop; state the UI reads
// is guarded by stateMutex_. Non-copyable: listeners point at `this`.
class PipeWireManager final : public IAudioBackend,
                              public IAppRouter,
                              public IChannelController,
                              public IEqualizerController,
                              public IMicrophoneController,
                              public IDeviceFormats,
                              public IAudioDevices {
public:
    PipeWireManager();
    ~PipeWireManager() override;

    PipeWireManager(const PipeWireManager&) = delete;
    PipeWireManager& operator=(const PipeWireManager&) = delete;
    PipeWireManager(PipeWireManager&&) = delete;
    PipeWireManager& operator=(PipeWireManager&&) = delete;

    // --- IAudioBackend ---
    bool initialize() override;
    void shutdown() override;
    [[nodiscard]] BackendState state() const noexcept override { return state_; }
    [[nodiscard]] bool isAvailable() const noexcept override {
        return state_ == BackendState::Available;
    }
    [[nodiscard]] std::string_view name() const noexcept override { return "PipeWire"; }
    [[nodiscard]] AudioServerInfo serverInfo() const override { return info_; }

    // --- IAppRouter ---
    [[nodiscard]] std::vector<AppStream> applications() const override;
    bool assign(std::uint32_t appId, ChannelId channel) override;
    [[nodiscard]] std::uint64_t revision() const override { return revision_.load(); }

    void setChannelBalance(ChannelId id, float balance) override;
    void setChannelGain(ChannelId id, float gain) override;
    void setStreamLevel(ChannelId id, float level) override;
    [[nodiscard]] float streamLevel(ChannelId id) const override;
    // --- IChannelController ---
    void setChannelVolume(ChannelId id, float volume) override;
    void setChannelMute(ChannelId id, bool muted) override;
    [[nodiscard]] ChannelLevel channelLevel(ChannelId id) override;
    void setChannelOutput(ChannelId id, const std::string& deviceName) override;
    [[nodiscard]] std::string channelOutput(ChannelId id) const override;

    // --- IEqualizerController ---
    void applyEqualizer(ChannelId id, const dsp::EqSettings& settings) override;
    void applyBassStage(ChannelId id, const dsp::BassStageSettings& settings) override;

    // --- IMicrophoneController ---
    void setMicSettings(const dsp::MicSettings& settings) override;
    [[nodiscard]] dsp::MicSettings micSettings() const override;
    [[nodiscard]] MicLevels micLevels() override;
    void setMicNoiseSuppression(bool on) override;
    [[nodiscard]] bool micNoiseSuppression() const override;
    [[nodiscard]] bool micNoiseSuppressionAvailable() const override;
    [[nodiscard]] std::string_view micNoiseSuppressionBackend() const override;
    [[nodiscard]] std::vector<MicInputDevice> micInputDevices() const override;
    [[nodiscard]] std::uint64_t micInputRevision() const override {
        return micInputRevision_.load();
    }
    bool setMicCaptureDevice(const std::string& nodeName) override;
    [[nodiscard]] std::string micCaptureDevice() const override;
    [[nodiscard]] std::string micActiveCaptureDevice() const override;
    [[nodiscard]] std::string micDefaultInput() const override;
    void setMicLive(bool live) override;
    void setMicMonitor(bool on, float level) override;
    void setMicEchoCancellation(bool on, const std::string& speakers) override;
    [[nodiscard]] bool micEchoCancellationActive() const override {
        return echoActive_.load();
    }
    [[nodiscard]] std::string micEchoReference() const override;
    [[nodiscard]] std::vector<AudioDevice> micEchoOutputs() override { return outputDevices(); }
    [[nodiscard]] std::string micDefaultOutput() const override;
    [[nodiscard]] float micLatencyMs() const override;

    // --- IDeviceFormats ---
    [[nodiscard]] DeviceFormats supportedFormats(const std::string& nameContains) override;

    // --- IAudioDevices ---
    [[nodiscard]] std::vector<AudioDevice> outputDevices() override;
    [[nodiscard]] std::uint64_t devicesRevision() const override { return devicesRevision_.load(); }
    [[nodiscard]] DeviceCodecs deviceCodecs(std::uint32_t deviceId) override;
    bool setDeviceProfile(std::uint32_t deviceId, std::uint32_t profileIndex) override;
    [[nodiscard]] float deviceVolume(std::uint32_t nodeId) override;
    void setDeviceVolume(std::uint32_t nodeId, float volume) override;

    // Stable node name of a channel's virtual sink, e.g. "sonero_game".
    [[nodiscard]] static std::string nodeNameFor(ChannelId id);

    // Which input the microphone captures: the chosen device while connected, else the
    // system default — unless that is Bluetooth (binding it forces HFP). "" = nothing.
    [[nodiscard]] static std::string resolveMicTarget(
        const std::string& chosen, const std::string& systemDefault,
        const std::vector<std::string>& connected);

private:
    // Core listener callbacks (loop thread).
    static void onCoreInfo(void* data, const pw_core_info* info);
    static void onCoreDone(void* data, std::uint32_t id, int seq);
    static void onCoreError(void* data, std::uint32_t id, int seq, int res,
                            const char* message);

    // Registry listener callbacks (loop thread).
    static void onRegistryGlobal(void* data, std::uint32_t id, std::uint32_t permissions,
                                 const char* type, std::uint32_t version,
                                 const spa_dict* props);
    static void onRegistryGlobalRemove(void* data, std::uint32_t id);

    void onGlobal(std::uint32_t id, const char* type, const spa_dict* props);
    void onGlobalRemove(std::uint32_t id);

    // Default-sink guard: our virtual sinks must never become the system default.
    // Loop thread.
    static int onMetadataProperty(void* data, std::uint32_t subject, const char* key,
                                  const char* type, const char* value);
    void onMetaProperty(std::uint32_t subject, const char* key, const char* value);

    // --- Bound proxies (all: thread loop held) ---
    using ParamCallback = void (*)(void* data, int seq, std::uint32_t id, std::uint32_t index,
                                   std::uint32_t next, const spa_pod* param);
    [[nodiscard]] pw_proxy* bindLocked(std::uint32_t id, const char* type, std::uint32_t version);
    // Enumerate `params` on a bound node or device and wait until every answer
    // has been delivered to `onParam`.
    void enumParamsLocked(pw_proxy* proxy, bool isDevice,
                          std::initializer_list<std::uint32_t> params,
                          ParamCallback onParam, void* data);
    // A plain node's own volume, linear. For our streams, which have no route.
    void setNodeVolumeLocked(std::uint32_t nodeId, float volume);
    // The output route behind a hardware sink. On success `device` is a bound
    // proxy the caller destroys.
    struct RouteState {
        pw_device* device = nullptr;
        std::int32_t index = 0;
        std::int32_t routeDevice = 0;
        std::vector<float> volumes;
    };
    bool readRouteLocked(std::uint32_t nodeId, RouteState& out);

    // Read-modify-write of one output channel's chain settings.
    template <typename Change>
    void updateChannelSettings(ChannelId id, Change&& change);

    bool doRoundtrip();       // blocks (with timeout) until the server answers
    void setupGraph();        // registry listener + virtual sinks (lock held)
    void createVirtualSinks();
    void teardown() noexcept;

    // Channel sink wiring (loop thread; caller holds the loop lock).
    static std::optional<ChannelId> channelForNodeName(std::string_view nodeName);

    // Push a channel's pinned output target onto its `.out` stream via metadata.
    // Must be called with the thread loop held (registry callbacks already are).
    void applyChannelRouteLocked(ChannelId id);
    void applyAllRoutesLocked();  // re-resolve every channel (device (dis)connect)
    [[nodiscard]] std::string micTarget() const;
    void applyMicRouteLocked();   // re-attach the microphone if its target moved
    // Load, reload or drop the echo canceller in front of the microphone, and
    // return what the chain should capture from. Loop thread / lock held.
    [[nodiscard]] std::string applyEchoCancelLocked(const std::string& micDevice);
    void destroyEchoCancelLocked();

    void bumpRevision() noexcept { revision_.fetch_add(1, std::memory_order_relaxed); }

    // A copy of one field guarded by stateMutex_.
    template <typename T>
    [[nodiscard]] T readLocked(const T& field) const {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        return field;
    }

    // The microphone's own node pair and DSP. Not a filter-chain module like the
    // other channels: the chain it runs is ours, and the meters come out of it.
    MicProcessor mic_;

    // One per channel, indexed by channelIndex(); the Microphone slot is unused (mic_).
    std::array<ChannelProcessor, kAllChannels.size()> processor_;

    // PipeWire connection.
    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_proxy* streamMix_ = nullptr;
    // Per-channel send into the stream mix: node id (from the registry) and level.
    std::unordered_map<int, std::uint32_t> streamSendNode_;
    std::unordered_map<int, float> streamLevel_;  // the sink OBS captures (see createVirtualSinks)
    spa_hook coreListener_{};

    // Graph objects.
    pw_registry* registry_ = nullptr;
    spa_hook registryListener_{};
    pw_metadata* metadata_ = nullptr;              // the "default" metadata store
    spa_hook metadataListener_{};
    std::string realDefaultSink_;                  // last non-virtual default (loop thread)
    std::string realDefaultSource_;                // real hardware mic, for the mic capture target
    std::vector<pw_impl_module*> modules_;         // loaded loopback modules
    // The echo canceller, when on, and the microphone and output it was built
    // for: either changing means building it again.
    pw_impl_module* echoModule_ = nullptr;
    std::string echoMic_;
    std::string echoReference_;
    std::atomic<bool> echoActive_{false};

    BackendState state_ = BackendState::Uninitialized;
    AudioServerInfo info_{};

    // Round-trip synchronization (loop thread).
    int pendingSeq_ = 0;
    bool roundtripDone_ = false;
    bool errored_ = false;
    std::string errorMessage_;

    // Shared graph state (guarded by stateMutex_).
    mutable std::mutex stateMutex_;
    std::unordered_map<std::uint32_t, std::string> apps_;  // node id -> display name
    std::unordered_map<std::uint32_t, ChannelId> assignments_;
    std::unordered_map<std::string, std::uint64_t> sinkSerials_;  // node name -> serial
    std::unordered_map<std::uint32_t, AudioDevice> devices_;  // node id -> hw output device
    std::unordered_map<std::uint32_t, MicInputDevice> inputs_;  // node id -> hw input device
    std::string micChosen_;  // node.name the user picked ("" = follow the default)
    std::string micDevice_;  // the real input the chain is fed from, echo canceller or not
    bool micEchoCancel_ = false;
    std::string micEchoSpeakers_;  // output the speakers are on ("" = the default)
    std::string realDefaultSinkShared_;  // realDefaultSink_, readable off the loop thread

    // Per-channel output routing: the channel's `.out` playback stream (id+serial)
    // and the device node.name the user pinned it to ("" = follow the default).
    struct OutNode {
        std::uint32_t id = 0;
        std::uint64_t serial = 0;
    };
    std::unordered_map<ChannelId, OutNode> channelOut_;
    std::unordered_map<ChannelId, std::string> channelDeviceName_;

    std::atomic<std::uint64_t> revision_{0};
    std::atomic<std::uint64_t> devicesRevision_{0};
    std::atomic<std::uint64_t> micInputRevision_{0};
};

}  // namespace sonar::audio
