#pragma once

#include <array>
#include <atomic>
#include <cstdint>
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
struct pw_node_info;
struct pw_stream;
struct spa_dict;

namespace sonar::audio {

// PipeWire backend.
//
// Responsibilities:
//   * own a threaded PipeWire main loop and a connection to the server;
//   * create one virtual sink per channel (via module-loopback) that apps can
//     target and that forwards to the default output;
//   * watch the registry for application streams and route them onto the
//     virtual sinks (via the "default" metadata target.object key).
//
// All libpipewire access happens on, or is synchronized against, the thread
// loop. Shared state read by the UI thread is guarded by a mutex. The class
// owns raw C resources and installs listeners pointing at `this`, so it is
// non-copyable and non-movable.
class PipeWireManager final : public IAudioBackend,
                              public IAppRouter,
                              public IChannelController,
                              public IEqualizerController,
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
    [[nodiscard]] float channelHeadroom(ChannelId id) const override;
    // --- IChannelController ---
    void setChannelVolume(ChannelId id, float volume) override;
    void setChannelMute(ChannelId id, bool muted) override;
    [[nodiscard]] ChannelLevel channelLevel(ChannelId id) override;
    void setChannelOutput(ChannelId id, const std::string& deviceName) override;
    [[nodiscard]] std::string channelOutput(ChannelId id) const override;

    // --- IEqualizerController ---
    void applyEqualizer(ChannelId id, const dsp::EqSettings& settings) override;

    // --- IDeviceFormats ---
    [[nodiscard]] DeviceFormats supportedFormats(const std::string& nameContains) override;

    // --- IAudioDevices ---
    [[nodiscard]] std::vector<AudioDevice> outputDevices() override;
    [[nodiscard]] std::uint64_t devicesRevision() const override { return devicesRevision_.load(); }
    [[nodiscard]] DeviceCodecs deviceCodecs(std::uint32_t deviceId) override;
    bool setDeviceProfile(std::uint32_t deviceId, std::uint32_t profileIndex) override;
    [[nodiscard]] float deviceVolume(std::uint32_t nodeId) override;
    void setDeviceVolume(std::uint32_t nodeId, float volume) override;

    // Number of biquad bands in the DSP filter-chain (fixed; any UI band count
    // is sampled onto these).
    static constexpr int kEqBands = 31;

    // Stable node name of a channel's virtual sink, e.g. "sonar_game".
    [[nodiscard]] static std::string nodeNameFor(ChannelId id);

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

    // Default-sink metadata guard: our virtual sinks must never become the
    // system default (that would funnel every app — and our own loopbacks — into
    // one channel). Runs on the loop thread.
    static int onMetadataProperty(void* data, std::uint32_t subject, const char* key,
                                  const char* type, const char* value);
    void onMetaProperty(std::uint32_t subject, const char* key, const char* value);

    struct ChannelIO;  // defined below; referenced by the helpers here

    // Metering stream process callback (real-time thread).
    static void onMeterProcess(void* data);
    // Sink-node ready callback; applies the desired volume/mute once (loop thread).
    static void onNodeInfo(void* data, const pw_node_info* info);

    bool doRoundtrip();       // blocks (with timeout) until the server answers
    void setupGraph();        // registry listener + virtual sinks (lock held)
    void createVirtualSinks();
    void teardown() noexcept;

    // Channel sink wiring (loop thread; caller holds the loop lock).
    static std::optional<ChannelId> channelForNodeName(std::string_view nodeName);
    void bindChannelSink(ChannelId channel, std::uint32_t nodeId,
                         const std::string& nodeName);
    void createMeterStream(ChannelId channel, const std::string& sinkName);
    static void applyVolume(ChannelIO& io);
    static void applyMute(ChannelIO& io);
    static void applyEq(ChannelIO& io);

    // Push a channel's pinned output target onto its `.out` stream via metadata.
    // Must be called with the thread loop held (registry callbacks already are).
    void applyChannelRouteLocked(ChannelId id);
    void applyAllRoutesLocked();  // re-resolve every channel (device (dis)connect)

    void bumpRevision() noexcept { revision_.fetch_add(1, std::memory_order_relaxed); }

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

    BackendState state_ = BackendState::Uninitialized;
    AudioServerInfo info_{};

    // Round-trip synchronization (loop thread).
    int pendingSeq_ = 0;
    bool roundtripDone_ = false;
    bool errored_ = false;
    std::string errorMessage_;

    // Shared graph state (guarded by stateMutex_).
    struct AppInfo {
        std::string name;
        std::uint64_t serial = 0;
    };
    mutable std::mutex stateMutex_;
    std::unordered_map<std::uint32_t, AppInfo> apps_;
    std::unordered_map<std::uint32_t, ChannelId> assignments_;
    std::unordered_map<std::string, std::uint64_t> sinkSerials_;  // node name -> serial
    std::unordered_map<std::uint32_t, AudioDevice> devices_;  // node id -> hw output device

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

    // Per-channel real I/O: the sink node (for volume/mute) and a monitor capture
    // stream (for metering). Bound lazily as sinks appear in the registry. The
    // atomics are written by the RT process callback and read by the UI thread.
    struct ChannelIO {
        pw_node* node = nullptr;
        // Attenuation (0..1] compensating the EQ's peak boost — see applyEqualizer.
        float eqHeadroom = 1.0f;
        // Per-channel trim (0..2, linear) set by the user, independent of volume.
        float desiredGain = 1.0f;
        spa_hook nodeListener{};
        pw_stream* meter = nullptr;
        spa_hook meterListener{};
        std::atomic<float> peakLeft{0.0f};
        std::atomic<float> peakRight{0.0f};
        std::string nodeName;                        // e.g. "sonar_game" (for EQ keys)
        std::array<float, kEqBands> eqGains{};       // desired biquad gains (dB)
        float desiredBalance = 0.0f;
        float desiredVolume = 1.0f;
        bool desiredMute = false;
        bool bound = false;
        bool infoApplied = false;  // desired values pushed once the node was ready
    };
    std::array<ChannelIO, kAllChannels.size()> io_{};
};

}  // namespace sonar::audio
