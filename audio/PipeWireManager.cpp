#include "audio/PipeWireManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <set>

#include <pipewire/pipewire.h>
#include <pipewire/impl-module.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/format.h>
#include <spa/param/param.h>
#include <spa/param/profile.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>

#include "core/Log.h"
#include "dsp/Equalizer.h"

namespace sonar::audio {

namespace {
constexpr int kRoundtripTimeoutSeconds = 3;

// The DSP filter-chain is a fixed 31-band (1/3-octave) graphic EQ; any UI band
// count is sampled onto these frequencies.
constexpr float kEqQ = 4.318f;  // 1/3-octave Q

// Function-local static avoids a static-init-order dependency on Equalizer.cpp.
const std::vector<float>& dspFreqs() {
    static const std::vector<float> freqs = dsp::standardFrequencies(dsp::BandCount::Bands31);
    return freqs;
}

std::uint64_t serialOf(const spa_dict* props) {
    const char* s = spa_dict_lookup(props, "object.serial");
    return s != nullptr ? std::strtoull(s, nullptr, 10) : 0;
}

std::string displayNameOf(const spa_dict* props, std::uint32_t id) {
    const char* n = spa_dict_lookup(props, "application.name");
    if (n == nullptr) n = spa_dict_lookup(props, "media.name");
    if (n == nullptr) n = spa_dict_lookup(props, "node.name");
    return n != nullptr ? std::string(n) : ("Stream " + std::to_string(id));
}

bool startsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Infer the hardware class of an output from its node.name. PipeWire names sinks
// like alsa_output.usb-..., alsa_output.pci-..._hdmi..., bluez_output.XX_..., so
// the name alone is enough to badge the device in the UI.
DeviceType classifyDeviceType(const std::string& nodeName) {
    const auto has = [&](std::string_view needle) {
        return nodeName.find(needle) != std::string::npos;
    };
    if (startsWith(nodeName, "bluez")) return DeviceType::Bluetooth;
    if (has("hdmi") || has("HDMI")) return DeviceType::Hdmi;
    if (has("iec958") || has("spdif")) return DeviceType::Spdif;
    if (has("usb")) return DeviceType::Usb;
    if (startsWith(nodeName, "alsa_output") || has("analog") || has("pci")) {
        return DeviceType::Analog;
    }
    return DeviceType::Other;
}

// Build module-filter-chain args: a virtual Audio/Sink whose graph is a chain of
// bq_peaking biquads (one per band), forwarding to the default output. The band
// gains are controllable at runtime via the sink node's SPA_PROP_params.
std::string filterChainArgs(const std::string& node, const std::string& desc,
                            const std::vector<float>& freqs,
                            const std::vector<float>& gains, float q) {
    std::string nodes;
    std::string links;
    for (std::size_t i = 0; i < freqs.size(); ++i) {
        nodes += std::format(
            R"({{ type = builtin name = "{}_eq{}" label = bq_peaking )"
            R"(control = {{ "Freq" = {:.4f} "Q" = {:.4f} "Gain" = {:.4f} }} }} )",
            node, i, freqs[i], q, gains[i]);
        if (i + 1 < freqs.size()) {
            links += std::format(R"({{ output = "{}_eq{}:Out" input = "{}_eq{}:In" }} )",
                                 node, i, node, i + 1);
        }
    }
    return std::format(
        R"({{ node.description = "{0}" )"
        R"(filter.graph = {{ nodes = [ {1} ] links = [ {2} ] }} )"
        R"(capture.props = {{ node.name = "{3}" media.class = Audio/Sink )"
        R"(priority.session = 100 audio.position = [ FL FR ] }} )"
        R"(playback.props = {{ node.name = "{3}.out" node.passive = true )"
        R"(audio.position = [ FL FR ] }} }})",
        desc, nodes, links, node);
}

// Like filterChainArgs but exposes a virtual Audio/Source: the capture side
// pulls from the real microphone, the graph applies the EQ, and the playback
// side is the source apps select as their input.
std::string filterChainSourceArgs(const std::string& node, const std::string& desc,
                                  const std::vector<float>& freqs,
                                  const std::vector<float>& gains, float q,
                                  const std::string& captureTarget) {
    std::string nodes;
    std::string links;
    for (std::size_t i = 0; i < freqs.size(); ++i) {
        nodes += std::format(
            R"({{ type = builtin name = "{}_eq{}" label = bq_peaking )"
            R"(control = {{ "Freq" = {:.4f} "Q" = {:.4f} "Gain" = {:.4f} }} }} )",
            node, i, freqs[i], q, gains[i]);
        if (i + 1 < freqs.size()) {
            links += std::format(R"({{ output = "{}_eq{}:Out" input = "{}_eq{}:In" }} )",
                                 node, i, node, i + 1);
        }
    }
    // With no explicit target, disable autoconnect: otherwise the capture grabs
    // the default source, and if that is a Bluetooth headset it forces the card
    // into HFP (mono headset mode), collapsing A2DP music quality.
    const std::string target =
        captureTarget.empty() ? std::string("node.autoconnect = false ")
                              : std::format(R"(target.object = "{}" )", captureTarget);
    return std::format(
        R"({{ node.description = "{0}" )"
        R"(filter.graph = {{ nodes = [ {1} ] links = [ {2} ] }} )"
        R"(capture.props = {{ node.name = "{3}.input" node.passive = true {4})"
        R"(audio.position = [ FL FR ] }} )"
        R"(playback.props = {{ node.name = "{3}" media.class = Audio/Source )"
        R"(priority.session = 100 audio.position = [ FL FR ] }} }})",
        desc, nodes, links, node, target);
}

// Extract the sink name from a default metadata value like {"name":"foo"}.
std::string parseSinkName(const char* json) {
    if (json == nullptr) {
        return {};
    }
    const std::string s(json);
    std::size_t p = s.find("\"name\"");
    if (p == std::string::npos) return {};
    p = s.find(':', p);
    if (p == std::string::npos) return {};
    p = s.find('"', p);
    if (p == std::string::npos) return {};
    const std::size_t q = s.find('"', p + 1);
    if (q == std::string::npos) return {};
    return s.substr(p + 1, q - p - 1);
}
}  // namespace

std::string PipeWireManager::nodeNameFor(ChannelId id) {
    std::string suffix(channelName(id));
    std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return "sonero_" + suffix;
}

PipeWireManager::PipeWireManager() { pw_init(nullptr, nullptr); }

PipeWireManager::~PipeWireManager() {
    shutdown();
    pw_deinit();
}

bool PipeWireManager::initialize() {
    if (state_ != BackendState::Uninitialized) {
        return isAvailable();
    }

    loop_ = pw_thread_loop_new("sonero", nullptr);
    if (loop_ == nullptr) {
        log::error("PipeWire: could not create the thread loop");
        state_ = BackendState::Unavailable;
        return false;
    }

    if (pw_thread_loop_start(loop_) < 0) {
        log::error("PipeWire: could not start the thread loop");
        teardown();
        state_ = BackendState::Unavailable;
        return false;
    }

    pw_thread_loop_lock(loop_);

    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (context_ == nullptr) {
        log::error("PipeWire: could not create the context");
        pw_thread_loop_unlock(loop_);
        teardown();
        state_ = BackendState::Unavailable;
        return false;
    }

    core_ = pw_context_connect(context_, nullptr, 0);
    if (core_ == nullptr) {
        log::warn("PipeWire: no server available (is the daemon running?)");
        pw_thread_loop_unlock(loop_);
        teardown();
        state_ = BackendState::Unavailable;
        return false;
    }

    static const pw_core_events kCoreEvents = {
        .version = PW_VERSION_CORE_EVENTS,
        .info = &PipeWireManager::onCoreInfo,
        .done = &PipeWireManager::onCoreDone,
        .error = &PipeWireManager::onCoreError,
    };
    pw_core_add_listener(core_, &coreListener_, &kCoreEvents, this);

    const bool ok = doRoundtrip();
    if (ok) {
        setupGraph();  // still holding the loop lock
    }
    pw_thread_loop_unlock(loop_);

    if (!ok) {
        log::warn("PipeWire: handshake failed ({})",
                  errorMessage_.empty() ? "timeout" : errorMessage_);
        teardown();
        state_ = BackendState::Unavailable;
        return false;
    }

    state_ = BackendState::Available;
    log::info("PipeWire: connected to '{}' (version {}, {}@{})",
              info_.name, info_.version, info_.userName, info_.hostName);
    return true;
}

void PipeWireManager::setupGraph() {
    static const pw_registry_events kRegistryEvents = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = &PipeWireManager::onRegistryGlobal,
        .global_remove = &PipeWireManager::onRegistryGlobalRemove,
    };

    registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    if (registry_ == nullptr) {
        log::warn("PipeWire: could not obtain the registry; routing disabled");
        return;
    }
    pw_registry_add_listener(registry_, &registryListener_, &kRegistryEvents, this);

    // Enumerate existing objects first: this binds the "default" metadata and
    // captures the current (real) default sink, so createVirtualSinks() below
    // can keep the system default off our own sinks.
    doRoundtrip();
    doRoundtrip();
    createVirtualSinks();
}

void PipeWireManager::createVirtualSinks() {
    // Each channel sink is a 31-band filter-chain (EQ) whose gains start flat, so
    // it is a clean passthrough until the user edits the equalizer.
    const std::vector<float> gains(dspFreqs().size(), 0.0f);

    for (const ChannelId id : kAllChannels) {
        const std::string node = nodeNameFor(id);
        const std::string desc = "Sonero " + std::string(channelName(id));
        // The Microphone channel is a virtual SOURCE (capture the real mic → EQ →
        // Audio/Source); every other channel is an Audio/Sink.
        // Never bind the mic capture to a Bluetooth input: doing so forces the
        // headset into HFP and kills its A2DP music quality. A BT mic and A2DP
        // playback cannot coexist, so we keep A2DP and leave the capture idle.
        const std::string micTarget =
            startsWith(realDefaultSource_, "bluez") ? std::string() : realDefaultSource_;
        const std::string args =
            id == ChannelId::Microphone
                ? filterChainSourceArgs(node, desc, dspFreqs(), gains, kEqQ, micTarget)
                : filterChainArgs(node, desc, dspFreqs(), gains, kEqQ);

        pw_impl_module* module = pw_context_load_module(
            context_, "libpipewire-module-filter-chain", args.c_str(), nullptr);
        if (module != nullptr) {
            modules_.push_back(module);
        } else {
            log::warn("PipeWire: failed to create virtual sink '{}'", node);
        }
    }
    log::info("PipeWire: created {} virtual channel sinks", modules_.size());
}

void PipeWireManager::shutdown() {
    if (state_ == BackendState::Uninitialized && loop_ == nullptr) {
        return;
    }
    teardown();
    info_ = {};
    errorMessage_.clear();
    pendingSeq_ = 0;
    roundtripDone_ = false;
    errored_ = false;
    state_ = BackendState::Uninitialized;
    log::debug("PipeWire: shut down");
}

bool PipeWireManager::doRoundtrip() {
    roundtripDone_ = false;
    errored_ = false;
    pendingSeq_ = pw_core_sync(core_, PW_ID_CORE, 0);

    while (!roundtripDone_ && !errored_) {
        if (pw_thread_loop_timed_wait(loop_, kRoundtripTimeoutSeconds) != 0) {
            return false;  // timed out
        }
    }
    return roundtripDone_ && !errored_;
}

void PipeWireManager::teardown() noexcept {
    if (loop_ != nullptr) {
        pw_thread_loop_stop(loop_);  // join the thread; no callbacks run after this
    }

    for (ChannelIO& io : io_) {
        if (io.meter != nullptr) {
            pw_stream_destroy(io.meter);
            io.meter = nullptr;
        }
        if (io.node != nullptr) {
            spa_hook_remove(&io.nodeListener);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(io.node));
            io.node = nullptr;
        }
        io.bound = false;
        io.infoApplied = false;
        io.desiredVolume = 1.0f;
        io.desiredMute = false;
        io.peakLeft.store(0.0f, std::memory_order_relaxed);
        io.peakRight.store(0.0f, std::memory_order_relaxed);
    }

    for (pw_impl_module* module : modules_) {
        if (module != nullptr) {
            pw_impl_module_destroy(module);
        }
    }
    modules_.clear();

    if (metadata_ != nullptr) {
        spa_hook_remove(&metadataListener_);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(metadata_));
        metadata_ = nullptr;
    }
    realDefaultSink_.clear();
    if (registry_ != nullptr) {
        spa_hook_remove(&registryListener_);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
        registry_ = nullptr;
    }
    if (core_ != nullptr) {
        spa_hook_remove(&coreListener_);
        pw_core_disconnect(core_);
        core_ = nullptr;
    }
    if (context_ != nullptr) {
        pw_context_destroy(context_);
        context_ = nullptr;
    }
    if (loop_ != nullptr) {
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    apps_.clear();
    assignments_.clear();
    sinkSerials_.clear();
    devices_.clear();
}

// --- IAppRouter --------------------------------------------------------------

std::vector<AppStream> PipeWireManager::applications() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::vector<AppStream> out;
    out.reserve(apps_.size());
    for (const auto& [id, info] : apps_) {
        AppStream app;
        app.id = id;
        app.name = info.name;
        app.channel = ChannelId::System;  // default if not routed
        if (const auto it = assignments_.find(id); it != assignments_.end()) {
            app.channel = it->second;
        }
        out.push_back(std::move(app));
    }
    std::sort(out.begin(), out.end(),
              [](const AppStream& a, const AppStream& b) { return a.id < b.id; });
    return out;
}

bool PipeWireManager::assign(std::uint32_t appId, ChannelId channel) {
    std::string serial;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const std::string node = nodeNameFor(channel);
        const auto it = sinkSerials_.find(node);
        if (it == sinkSerials_.end()) {
            log::warn("Router: virtual sink '{}' is not ready yet", node);
            return false;
        }
        serial = std::to_string(it->second);
        assignments_[appId] = channel;
    }

    if (metadata_ == nullptr) {
        log::warn("Router: default metadata not available; cannot route");
        return false;
    }

    pw_thread_loop_lock(loop_);
    pw_metadata_set_property(metadata_, appId, "target.object", nullptr, serial.c_str());
    pw_thread_loop_unlock(loop_);

    bumpRevision();
    log::info("Router: routed app {} to channel {}", appId, channelName(channel));
    return true;
}

// --- Registry handling -------------------------------------------------------

void PipeWireManager::onGlobal(std::uint32_t id, const char* type, const spa_dict* props) {
    if (type == nullptr || props == nullptr) {
        return;
    }

    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char* mediaClass = spa_dict_lookup(props, "media.class");
        if (mediaClass == nullptr) {
            return;
        }
        const char* rawName = spa_dict_lookup(props, "node.name");
        const std::string nodeName = rawName != nullptr ? rawName : "";

        if ((std::strcmp(mediaClass, "Audio/Sink") == 0 ||
             std::strcmp(mediaClass, "Audio/Source") == 0) &&
            startsWith(nodeName, "sonero_")) {  // one of our virtual channel nodes
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                sinkSerials_[nodeName] = serialOf(props);
            }
            if (const auto channel = channelForNodeName(nodeName)) {
                bindChannelSink(*channel, id, nodeName);
            }
        } else if (std::strcmp(mediaClass, "Audio/Sink") == 0 && !nodeName.empty()) {
            // A real hardware output — record name, friendly label, owning device,
            // and inferred type so the Devices page can list it live and
            // supportedFormats() can query it.
            AudioDevice dev;
            dev.nodeId = id;
            dev.name = nodeName;
            dev.serial = serialOf(props);
            const char* desc = spa_dict_lookup(props, "node.description");
            dev.description = (desc != nullptr && desc[0] != '\0') ? desc : nodeName;
            if (const char* d = spa_dict_lookup(props, "device.id"); d != nullptr) {
                dev.deviceId = static_cast<std::uint32_t>(std::strtoul(d, nullptr, 10));
            }
            dev.type = classifyDeviceType(nodeName);
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                devices_[id] = std::move(dev);
            }
            devicesRevision_.fetch_add(1, std::memory_order_relaxed);
            applyAllRoutesLocked();  // a (re)connected device may reclaim its channels
        } else if (std::strcmp(mediaClass, "Stream/Output/Audio") == 0) {
            if (startsWith(nodeName, "sonero_")) {
                // Our channel output streams (sonero_<channel>.out): track them so
                // each channel can be retargeted to a chosen device.
                if (endsWith(nodeName, ".out")) {
                    const std::string base = nodeName.substr(0, nodeName.size() - 4);
                    if (const auto ch = channelForNodeName(base)) {
                        {
                            std::lock_guard<std::mutex> lock(stateMutex_);
                            channelOut_[*ch] = {id, serialOf(props)};
                        }
                        applyChannelRouteLocked(*ch);
                    }
                }
                return;  // never treat our own streams as apps
            }
            AppInfo info{displayNameOf(props, id), serialOf(props)};
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                apps_[id] = std::move(info);
            }
            bumpRevision();
        }
    } else if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char* metaName = spa_dict_lookup(props, "metadata.name");
        if (metaName != nullptr && std::strcmp(metaName, "default") == 0 &&
            metadata_ == nullptr) {
            metadata_ = static_cast<pw_metadata*>(pw_registry_bind(
                registry_, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
            if (metadata_ != nullptr) {
                static const pw_metadata_events kMetaEvents = {
                    .version = PW_VERSION_METADATA_EVENTS,
                    .property = &PipeWireManager::onMetadataProperty,
                };
                pw_metadata_add_listener(metadata_, &metadataListener_, &kMetaEvents, this);
            }
        }
    }
}

void PipeWireManager::onGlobalRemove(std::uint32_t id) {
    bool changed = false;
    bool deviceRemoved = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        changed = apps_.erase(id) > 0;
        assignments_.erase(id);
        if (devices_.erase(id) > 0) {
            devicesRevision_.fetch_add(1, std::memory_order_relaxed);
            deviceRemoved = true;
        }
        for (auto it = channelOut_.begin(); it != channelOut_.end();) {
            it = it->second.id == id ? channelOut_.erase(it) : std::next(it);
        }
    }
    if (deviceRemoved) {
        applyAllRoutesLocked();  // channels pinned to it fall back to the default
    }
    if (changed) {
        bumpRevision();
    }
}

int PipeWireManager::onMetadataProperty(void* data, std::uint32_t subject, const char* key,
                                        const char* /*type*/, const char* value) {
    static_cast<PipeWireManager*>(data)->onMetaProperty(subject, key, value);
    return 0;
}

void PipeWireManager::onMetaProperty(std::uint32_t subject, const char* key,
                                     const char* value) {
    if (subject != 0 || key == nullptr || value == nullptr) {
        return;  // only the global default.audio.* entries matter here
    }

    // Remember the real hardware mic so the virtual mic captures from it.
    if (std::strcmp(key, "default.audio.source") == 0) {
        const std::string src = parseSinkName(value);
        if (!src.empty() && !startsWith(src, "sonero_")) {
            realDefaultSource_ = src;
        }
        return;
    }

    if (std::strcmp(key, "default.audio.sink") != 0) {
        return;
    }

    const std::string name = parseSinkName(value);
    if (name.empty()) {
        return;
    }

    if (startsWith(name, "sonero_")) {
        // A virtual sink was made default; put it back to the real one.
        if (!realDefaultSink_.empty() && metadata_ != nullptr) {
            const std::string json = "{\"name\":\"" + realDefaultSink_ + "\"}";
            pw_metadata_set_property(metadata_, 0, "default.audio.sink",
                                     "Spa:String:JSON", json.c_str());
            log::info("PipeWire: default sink kept off '{}' (restored '{}')", name,
                      realDefaultSink_);
        }
    } else {
        realDefaultSink_ = name;  // remember the real default to restore to
    }
}

// --- IChannelController: real volume / mute / metering -----------------------

std::optional<ChannelId> PipeWireManager::channelForNodeName(std::string_view nodeName) {
    for (const ChannelId id : kAllChannels) {
        if (nodeNameFor(id) == nodeName) {
            return id;
        }
    }
    return std::nullopt;
}

void PipeWireManager::bindChannelSink(ChannelId channel, std::uint32_t nodeId,
                                      const std::string& nodeName) {
    ChannelIO& io = io_[channelIndex(channel)];
    if (io.bound) {
        return;
    }
    io.bound = true;
    io.nodeName = nodeName;  // used to address the EQ control ports

    // Bind the sink node so we can drive its volume/mute. The desired values are
    // applied from onNodeInfo, i.e. once the node proxy is actually ready — doing
    // it immediately after binding races with the proxy becoming usable.
    io.node = static_cast<pw_node*>(pw_registry_bind(
        registry_, nodeId, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (io.node != nullptr) {
        static const pw_node_events kNodeEvents = {
            .version = PW_VERSION_NODE_EVENTS,
            .info = &PipeWireManager::onNodeInfo,
        };
        pw_node_add_listener(io.node, &io.nodeListener, &kNodeEvents, &io);
    }

    createMeterStream(channel, nodeName);
}

void PipeWireManager::onNodeInfo(void* data, const pw_node_info* /*info*/) {
    auto* io = static_cast<ChannelIO*>(data);
    if (io->infoApplied) {
        return;  // one-shot: avoid re-asserting on every info update
    }
    io->infoApplied = true;
    applyVolume(*io);
    applyMute(*io);
    applyEq(*io);
}

void PipeWireManager::createMeterStream(ChannelId channel, const std::string& sinkName) {
    ChannelIO& io = io_[channelIndex(channel)];
    const std::string meterName = sinkName + ".meter";

    // Meter the *output* of the channel, not its input. The sink's monitor taps
    // the signal before the filter graph and before the fader, so a meter there
    // shows what an application sent us rather than what we send onwards — and an
    // automatic gain driven from it would be blind to the EQ's own boost.
    // `<sink>.out` is the filter chain's playback side: post-EQ, post-volume.
    const std::string meterTarget =
        channel == ChannelId::Microphone ? sinkName : sinkName + ".out";

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_NODE_NAME, meterName.c_str(),
        PW_KEY_NODE_PASSIVE, "true",
        PW_KEY_TARGET_OBJECT, meterTarget.c_str(),
        nullptr);

    io.meter = pw_stream_new(core_, meterName.c_str(), props);
    if (io.meter == nullptr) {
        log::warn("PipeWire: could not create meter stream for '{}'", sinkName);
        return;
    }

    static const pw_stream_events kMeterEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .process = &PipeWireManager::onMeterProcess,
    };
    pw_stream_add_listener(io.meter, &io.meterListener, &kMeterEvents, &io);

    std::uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw format = {};
    format.format = SPA_AUDIO_FORMAT_F32;
    format.channels = 2;
    format.position[0] = SPA_AUDIO_CHANNEL_FL;
    format.position[1] = SPA_AUDIO_CHANNEL_FR;

    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &format);

    pw_stream_connect(
        io.meter, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                     PW_STREAM_FLAG_MAP_BUFFERS |
                                     PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
}

void PipeWireManager::applyVolume(ChannelIO& io) {
    if (io.node == nullptr) {
        return;
    }
    std::uint8_t buffer[512];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    // Volume and balance both drive channelVolumes, so they must be computed
    // together — otherwise setting one would wipe the other. Balance pans by
    // attenuating one side.
    // Three factors multiply into the level actually sent to the sink:
    //   desiredVolume  what the user set on the channel fader
    //   gain           the channel's trim, for taming a loud or quiet source
    //   eqHeadroom     compensation for however much the EQ boosts
    // plus a small fixed reserve: even a flat biquad cascade overshoots slightly,
    // and a track mastered to full scale then clips on conversion to the device's
    // integer format. One dB is inaudible and removes that whole class of grit.
    constexpr float kFilterHeadroom = 0.891f;  // -1 dB
    const float level = io.desiredVolume * io.desiredGain * io.eqHeadroom * kFilterHeadroom;
    float left = level;
    float right = level;
    if (io.desiredBalance > 0.0f) {
        left *= 1.0f - io.desiredBalance;    // pan right -> attenuate left
    } else if (io.desiredBalance < 0.0f) {
        right *= 1.0f + io.desiredBalance;   // pan left -> attenuate right
    }
    float volumes[2] = {left, right};
    auto* pod = static_cast<spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_channelVolumes,
        SPA_POD_Array(sizeof(float), SPA_TYPE_Float, 2, volumes)));
    pw_node_set_param(io.node, SPA_PARAM_Props, 0, pod);
}

void PipeWireManager::applyMute(ChannelIO& io) {
    if (io.node == nullptr) {
        return;
    }
    std::uint8_t buffer[512];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    auto* pod = static_cast<spa_pod*>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute,
        SPA_POD_Bool(io.desiredMute)));
    pw_node_set_param(io.node, SPA_PARAM_Props, 0, pod);
}

void PipeWireManager::applyEq(ChannelIO& io) {
    if (io.node == nullptr || io.nodeName.empty()) {
        return;
    }
    // Props { params = [ "<node>_eqN:Gain" <gainDb> ... ] } drives the biquads.
    std::uint8_t buffer[4096];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_pod_frame frame[2];
    spa_pod_builder_push_object(&b, &frame[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&b, SPA_PROP_params, 0);
    spa_pod_builder_push_struct(&b, &frame[1]);
    for (int i = 0; i < kEqBands; ++i) {
        const std::string key = io.nodeName + "_eq" + std::to_string(i) + ":Gain";
        spa_pod_builder_string(&b, key.c_str());
        spa_pod_builder_float(&b, io.eqGains[static_cast<std::size_t>(i)]);
    }
    spa_pod_builder_pop(&b, &frame[1]);
    auto* pod = static_cast<spa_pod*>(spa_pod_builder_pop(&b, &frame[0]));
    pw_node_set_param(io.node, SPA_PARAM_Props, 0, pod);
}

void PipeWireManager::applyEqualizer(ChannelId id, const dsp::EqSettings& settings) {
    if (loop_ == nullptr) {
        return;
    }
    pw_thread_loop_lock(loop_);
    ChannelIO& io = io_[channelIndex(id)];

    // Only send the bands whose gain actually changed: re-configuring every
    // biquad on each update makes the filters click ("zipper" noise).
    int changed[kEqBands];
    int count = 0;
    for (int i = 0; i < kEqBands; ++i) {
        // Each biquad gets its OWN band gain. Using the summed curve here would
        // re-apply the overlap between neighbouring bands, because the biquads are
        // chained in series and their responses already add.
        float g = settings.enabled
                      ? dsp::bandGainAt(settings, dspFreqs()[static_cast<std::size_t>(i)])
                      : 0.0f;
        g = std::clamp(g, dsp::kMinGainDb, dsp::kMaxGainDb);
        if (std::abs(g - io.eqGains[static_cast<std::size_t>(i)]) > 0.01f) {
            io.eqGains[static_cast<std::size_t>(i)] = g;
            changed[count++] = i;
        }
    }

    if (count > 0 && io.node != nullptr) {
        std::uint8_t buffer[4096];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        spa_pod_frame frame[2];
        spa_pod_builder_push_object(&b, &frame[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
        spa_pod_builder_prop(&b, SPA_PROP_params, 0);
        spa_pod_builder_push_struct(&b, &frame[1]);
        for (int k = 0; k < count; ++k) {
            const int i = changed[k];
            const std::string key = io.nodeName + "_eq" + std::to_string(i) + ":Gain";
            spa_pod_builder_string(&b, key.c_str());
            spa_pod_builder_float(&b, io.eqGains[static_cast<std::size_t>(i)]);
        }
        spa_pod_builder_pop(&b, &frame[1]);
        auto* pod = static_cast<spa_pod*>(spa_pod_builder_pop(&b, &frame[0]));
        pw_node_set_param(io.node, SPA_PARAM_Props, 0, pod);
    }

    // Headroom. A boosting EQ multiplies the signal, and music is already mastered
    // close to full scale, so +6 dB of bass turns into hard clipping the moment the
    // float graph is converted to the device's integer format — audible as gritty
    // distortion rather than "too loud". Attenuate ahead of the filters by exactly
    // the peak boost, so the loudest EQ'd frequency lands back at unity and the
    // volume slider stays usable across its whole range.
    const float headroom = settings.enabled ? dsp::peakBoostDb(settings) : 0.0f;
    const float scale = std::pow(10.0f, -headroom / 20.0f);
    if (std::abs(scale - io.eqHeadroom) > 0.001f) {
        io.eqHeadroom = scale;
        applyVolume(io);  // re-send channelVolumes with the new headroom folded in
    }
    pw_thread_loop_unlock(loop_);
}

// --- IDeviceFormats: enumerate a hardware sink's supported rates / depths -----

namespace {
// Map a concrete SPA audio format id to a PCM bit depth we expose (0 = ignore,
// e.g. float or planar DSP formats).
int bitDepthForSpaFormat(std::uint32_t fmt) {
    switch (fmt) {
        case SPA_AUDIO_FORMAT_S16_LE:
        case SPA_AUDIO_FORMAT_S16_BE:
            return 16;
        case SPA_AUDIO_FORMAT_S24_LE:
        case SPA_AUDIO_FORMAT_S24_BE:
        case SPA_AUDIO_FORMAT_S24_32_LE:
        case SPA_AUDIO_FORMAT_S24_32_BE:
            return 24;
        case SPA_AUDIO_FORMAT_S32_LE:
        case SPA_AUDIO_FORMAT_S32_BE:
            return 32;
        default:
            return 0;
    }
}

// Standard rates surfaced when a device advertises a continuous range.
constexpr int kStdRates[] = {44100, 48000, 88200, 96000, 176400, 192000};

void collectRates(const spa_pod* val, std::set<int>& out) {
    if (spa_pod_is_int(val)) {
        std::int32_t v = 0;
        spa_pod_get_int(val, &v);
        if (v > 0) out.insert(v);
        return;
    }
    if (!spa_pod_is_choice(val)) {
        return;
    }
    const std::uint32_t n = SPA_POD_CHOICE_N_VALUES(val);
    const auto* vals = static_cast<const std::int32_t*>(SPA_POD_CHOICE_VALUES(val));
    if (n == 0 || vals == nullptr) {
        return;
    }
    if (SPA_POD_CHOICE_TYPE(val) == SPA_CHOICE_Range && n >= 3) {
        const int lo = vals[1];  // [default, min, max]
        const int hi = vals[2];
        for (const int r : kStdRates) {
            if (r >= lo && r <= hi) out.insert(r);
        }
        if (vals[0] > 0) out.insert(vals[0]);
    } else {  // None / Enum / Step: every listed value is a valid choice
        for (std::uint32_t i = 0; i < n; ++i) {
            if (vals[i] > 0) out.insert(vals[i]);
        }
    }
}

void collectDepths(const spa_pod* val, std::set<int>& out) {
    const auto add = [&](std::uint32_t id) {
        if (const int b = bitDepthForSpaFormat(id)) out.insert(b);
    };
    if (spa_pod_is_id(val)) {
        std::uint32_t v = 0;
        spa_pod_get_id(val, &v);
        add(v);
        return;
    }
    if (!spa_pod_is_choice(val)) {
        return;
    }
    const std::uint32_t n = SPA_POD_CHOICE_N_VALUES(val);
    const auto* vals = static_cast<const std::uint32_t*>(SPA_POD_CHOICE_VALUES(val));
    for (std::uint32_t i = 0; vals != nullptr && i < n; ++i) {
        add(vals[i]);
    }
}

struct FormatCollector {
    std::set<int> rates;
    std::set<int> depths;
};

void onEnumFormat(void* data, int /*seq*/, std::uint32_t id, std::uint32_t /*index*/,
                  std::uint32_t /*next*/, const spa_pod* param) {
    if (param == nullptr || id != SPA_PARAM_EnumFormat) {
        return;
    }
    std::uint32_t mediaType = 0;
    std::uint32_t mediaSubtype = 0;
    if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 ||
        mediaType != SPA_MEDIA_TYPE_audio || mediaSubtype != SPA_MEDIA_SUBTYPE_raw) {
        return;
    }
    auto* c = static_cast<FormatCollector*>(data);
    const auto* obj = reinterpret_cast<const spa_pod_object*>(param);
    const spa_pod_prop* prop = nullptr;
    SPA_POD_OBJECT_FOREACH(obj, prop) {
        if (prop->key == SPA_FORMAT_AUDIO_rate) {
            collectRates(&prop->value, c->rates);
        } else if (prop->key == SPA_FORMAT_AUDIO_format) {
            collectDepths(&prop->value, c->depths);
        }
    }
}
}  // namespace

DeviceFormats PipeWireManager::supportedFormats(const std::string& nameContains) {
    DeviceFormats out;
    if (loop_ == nullptr || registry_ == nullptr) {
        return out;
    }

    pw_thread_loop_lock(loop_);
    std::uint32_t nodeId = 0;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        for (const auto& [id, dev] : devices_) {
            if (dev.name.find(nameContains) != std::string::npos) {
                nodeId = id;
                break;
            }
        }
    }
    if (nodeId == 0) {
        pw_thread_loop_unlock(loop_);
        return out;
    }

    auto* node = static_cast<pw_node*>(pw_registry_bind(
        registry_, nodeId, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (node == nullptr) {
        pw_thread_loop_unlock(loop_);
        return out;
    }

    FormatCollector collector;
    spa_hook hook{};
    static const pw_node_events kEnumEvents = {
        .version = PW_VERSION_NODE_EVENTS,
        .param = &onEnumFormat,
    };
    pw_node_add_listener(node, &hook, &kEnumEvents, &collector);
    pw_node_enum_params(node, 0, SPA_PARAM_EnumFormat, 0, UINT32_MAX, nullptr);
    doRoundtrip();  // let the param events arrive, then the sync completes
    doRoundtrip();

    spa_hook_remove(&hook);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(node));
    pw_thread_loop_unlock(loop_);

    out.rates.assign(collector.rates.begin(), collector.rates.end());
    out.depths.assign(collector.depths.begin(), collector.depths.end());
    out.valid = !out.rates.empty() && !out.depths.empty();
    return out;
}

std::vector<AudioDevice> PipeWireManager::outputDevices() {
    std::vector<AudioDevice> out;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        out.reserve(devices_.size());
        for (const auto& [id, dev] : devices_) {
            out.push_back(dev);
        }
    }
    std::sort(out.begin(), out.end(), [](const AudioDevice& a, const AudioDevice& b) {
        if (a.type != b.type) return a.type < b.type;
        return a.description < b.description;
    });
    return out;
}

// --- IAudioDevices: Bluetooth A2DP codecs (device card profiles) -------------

namespace {
// Pull the human codec name out of a profile description like
// "High Fidelity Playback (A2DP Sink, codec aptX HD)".
std::string codecLabel(const std::string& desc, const std::string& name) {
    const auto p = desc.find("codec ");
    if (p != std::string::npos) {
        const auto s = p + 6;
        const auto e = desc.find(')', s);
        return desc.substr(s, e == std::string::npos ? std::string::npos : e - s);
    }
    return name;
}

struct ProfileCollector {
    std::vector<DeviceCodec> codecs;
    std::uint32_t current = 0;
};

void onDeviceParam(void* data, int /*seq*/, std::uint32_t id, std::uint32_t /*index*/,
                   std::uint32_t /*next*/, const spa_pod* param) {
    if (param == nullptr) {
        return;
    }
    auto* c = static_cast<ProfileCollector*>(data);
    std::int32_t idx = -1;
    const char* name = nullptr;
    const char* desc = nullptr;
    // index/name/description are reliably Int/String; parse only those strictly.
    if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamProfile, nullptr,
                             SPA_PARAM_PROFILE_index, SPA_POD_Int(&idx),
                             SPA_PARAM_PROFILE_name, SPA_POD_OPT_String(&name),
                             SPA_PARAM_PROFILE_description, SPA_POD_OPT_String(&desc)) < 0) {
        return;
    }
    if (id == SPA_PARAM_Profile) {  // the active profile carries only its index
        c->current = static_cast<std::uint32_t>(idx);
        return;
    }
    if (id != SPA_PARAM_EnumProfile) {
        return;
    }
    const std::string n = name != nullptr ? name : "";
    const std::string d = desc != nullptr ? desc : "";
    if (n.find("a2dp") == std::string::npos && d.find("A2DP") == std::string::npos) {
        return;  // only A2DP playback codecs, not HSP/HFP or "off"
    }
    DeviceCodec dc;
    dc.profileIndex = static_cast<std::uint32_t>(idx);
    dc.name = n;
    dc.codec = codecLabel(d, n);
    // Availability is Id or Int depending on the version; read it tolerantly.
    const auto* obj = reinterpret_cast<const spa_pod_object*>(param);
    if (const spa_pod_prop* ap =
            spa_pod_object_find_prop(obj, nullptr, SPA_PARAM_PROFILE_available)) {
        std::uint32_t a = SPA_PARAM_AVAILABILITY_unknown;
        std::int32_t ai = 0;
        if (spa_pod_get_id(&ap->value, &a) >= 0) {
            dc.available = a != SPA_PARAM_AVAILABILITY_no;
        } else if (spa_pod_get_int(&ap->value, &ai) >= 0) {
            dc.available = ai != SPA_PARAM_AVAILABILITY_no;
        }
    }
    c->codecs.push_back(std::move(dc));
}
}  // namespace

DeviceCodecs PipeWireManager::deviceCodecs(std::uint32_t deviceId) {
    DeviceCodecs out;
    if (loop_ == nullptr || registry_ == nullptr || deviceId == 0) {
        return out;
    }
    pw_thread_loop_lock(loop_);
    auto* dev = static_cast<pw_device*>(pw_registry_bind(
        registry_, deviceId, PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE, 0));
    if (dev == nullptr) {
        pw_thread_loop_unlock(loop_);
        return out;
    }
    ProfileCollector collector;
    spa_hook hook{};
    static const pw_device_events kEvents = {
        .version = PW_VERSION_DEVICE_EVENTS,
        .param = &onDeviceParam,
    };
    pw_device_add_listener(dev, &hook, &kEvents, &collector);
    pw_device_enum_params(dev, 0, SPA_PARAM_EnumProfile, 0, UINT32_MAX, nullptr);
    pw_device_enum_params(dev, 0, SPA_PARAM_Profile, 0, UINT32_MAX, nullptr);
    doRoundtrip();
    doRoundtrip();
    spa_hook_remove(&hook);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(dev));
    pw_thread_loop_unlock(loop_);

    out.codecs = std::move(collector.codecs);
    out.currentIndex = collector.current;
    out.valid = !out.codecs.empty();
    return out;
}

bool PipeWireManager::setDeviceProfile(std::uint32_t deviceId, std::uint32_t profileIndex) {
    if (loop_ == nullptr || registry_ == nullptr || deviceId == 0) {
        return false;
    }
    pw_thread_loop_lock(loop_);
    auto* dev = static_cast<pw_device*>(pw_registry_bind(
        registry_, deviceId, PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE, 0));
    bool ok = false;
    if (dev != nullptr) {
        std::uint8_t buffer[512];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        auto* pod = static_cast<spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile, SPA_PARAM_PROFILE_index,
            SPA_POD_Int(static_cast<std::int32_t>(profileIndex))));
        pw_device_set_param(dev, SPA_PARAM_Profile, 0, pod);
        doRoundtrip();
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(dev));
        ok = true;
        log::info("BT codec: device {} -> profile index {}", deviceId, profileIndex);
    }
    pw_thread_loop_unlock(loop_);
    return ok;
}

namespace {
struct VolumeCollector {
    float volume = -1.0f;
};

void onNodeVolumeParam(void* data, int /*seq*/, std::uint32_t id, std::uint32_t /*index*/,
                       std::uint32_t /*next*/, const spa_pod* param) {
    if (param == nullptr || id != SPA_PARAM_Props) {
        return;
    }
    auto* c = static_cast<VolumeCollector*>(data);
    const auto* obj = reinterpret_cast<const spa_pod_object*>(param);
    const spa_pod_prop* prop = spa_pod_object_find_prop(obj, nullptr, SPA_PROP_channelVolumes);
    if (prop != nullptr) {
        std::uint32_t n = 0;
        const auto* vals = static_cast<const float*>(spa_pod_get_array(&prop->value, &n));
        if (vals != nullptr && n > 0) {
            c->volume = vals[0];  // first channel is representative
            return;
        }
    }
    if (const spa_pod_prop* v = spa_pod_object_find_prop(obj, nullptr, SPA_PROP_volume)) {
        float f = 0.0f;
        if (spa_pod_get_float(&v->value, &f) >= 0) {
            c->volume = f;
        }
    }
}
}  // namespace

float PipeWireManager::deviceVolume(std::uint32_t nodeId) {
    if (loop_ == nullptr || registry_ == nullptr || nodeId == 0) {
        return 1.0f;
    }
    pw_thread_loop_lock(loop_);
    auto* node = static_cast<pw_node*>(
        pw_registry_bind(registry_, nodeId, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (node == nullptr) {
        pw_thread_loop_unlock(loop_);
        return 1.0f;
    }
    VolumeCollector collector;
    spa_hook hook{};
    static const pw_node_events kVolEvents = {
        .version = PW_VERSION_NODE_EVENTS,
        .param = &onNodeVolumeParam,
    };
    pw_node_add_listener(node, &hook, &kVolEvents, &collector);
    pw_node_enum_params(node, 0, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);
    doRoundtrip();
    doRoundtrip();
    spa_hook_remove(&hook);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(node));
    pw_thread_loop_unlock(loop_);
    return collector.volume >= 0.0f ? collector.volume : 1.0f;
}

void PipeWireManager::setDeviceVolume(std::uint32_t nodeId, float volume) {
    if (loop_ == nullptr || registry_ == nullptr || nodeId == 0) {
        return;
    }
    volume = std::clamp(volume, 0.0f, 1.0f);
    pw_thread_loop_lock(loop_);
    auto* node = static_cast<pw_node*>(
        pw_registry_bind(registry_, nodeId, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (node != nullptr) {
        std::uint8_t buffer[512];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        float volumes[2] = {volume, volume};
        auto* pod = static_cast<spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_channelVolumes,
            SPA_POD_Array(sizeof(float), SPA_TYPE_Float, 2, volumes)));
        pw_node_set_param(node, SPA_PARAM_Props, 0, pod);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(node));
    }
    pw_thread_loop_unlock(loop_);
}

void PipeWireManager::setChannelVolume(ChannelId id, float volume) {
    volume = std::clamp(volume, 0.0f, 1.0f);
    if (loop_ == nullptr) {
        return;
    }
    pw_thread_loop_lock(loop_);
    ChannelIO& io = io_[channelIndex(id)];
    io.desiredVolume = volume;
    applyVolume(io);
    pw_thread_loop_unlock(loop_);
}

void PipeWireManager::setChannelBalance(ChannelId id, float balance) {
    if (loop_ == nullptr) {
        return;
    }
    pw_thread_loop_lock(loop_);
    ChannelIO& io = io_[channelIndex(id)];
    io.desiredBalance = std::clamp(balance, -1.0f, 1.0f);
    applyVolume(io);
    pw_thread_loop_unlock(loop_);
}

void PipeWireManager::setChannelGain(ChannelId id, float gain) {
    if (loop_ == nullptr) {
        return;
    }
    pw_thread_loop_lock(loop_);
    ChannelIO& io = io_[channelIndex(id)];
    // Up to +6 dB, so a quiet source can be lifted as well as tamed.
    io.desiredGain = std::clamp(gain, 0.0f, 2.0f);
    applyVolume(io);
    pw_thread_loop_unlock(loop_);
}

float PipeWireManager::channelHeadroom(ChannelId id) const {
    // kFilterHeadroom mirrors applyVolume; eqHeadroom is whatever the current EQ
    // curve required. Read without the loop lock: it is a single float that only
    // ever moves between plausible values, and a metering client can tolerate
    // reading the previous one.
    constexpr float kFilterHeadroom = 0.891f;
    return io_[channelIndex(id)].eqHeadroom * kFilterHeadroom;
}

void PipeWireManager::setChannelMute(ChannelId id, bool muted) {
    if (loop_ == nullptr) {
        return;
    }
    pw_thread_loop_lock(loop_);
    ChannelIO& io = io_[channelIndex(id)];
    io.desiredMute = muted;
    applyMute(io);
    pw_thread_loop_unlock(loop_);
}

ChannelLevel PipeWireManager::channelLevel(ChannelId id) {
    ChannelIO& io = io_[channelIndex(id)];
    ChannelLevel level;
    level.peakLeft = io.peakLeft.exchange(0.0f, std::memory_order_relaxed);
    level.peakRight = io.peakRight.exchange(0.0f, std::memory_order_relaxed);
    return level;
}

void PipeWireManager::setChannelOutput(ChannelId id, const std::string& deviceName) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        channelDeviceName_[id] = deviceName;
    }
    if (loop_ != nullptr) {
        pw_thread_loop_lock(loop_);
        applyChannelRouteLocked(id);
        pw_thread_loop_unlock(loop_);
    }
    log::info("Router: channel {} output -> {}", channelName(id),
              deviceName.empty() ? "default" : deviceName);
}

std::string PipeWireManager::channelOutput(ChannelId id) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto it = channelDeviceName_.find(id);
    return it != channelDeviceName_.end() ? it->second : std::string();
}

void PipeWireManager::applyChannelRouteLocked(ChannelId id) {
    if (metadata_ == nullptr) {
        return;
    }
    std::uint32_t outId = 0;
    std::string targetSerial;  // empty -> clear the target (follow the default)
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        const auto out = channelOut_.find(id);
        if (out == channelOut_.end()) {
            return;  // the channel's .out stream is not in the graph yet
        }
        outId = out->second.id;
        const auto name = channelDeviceName_.find(id);
        if (name != channelDeviceName_.end() && !name->second.empty()) {
            for (const auto& [devId, dev] : devices_) {
                if (dev.name == name->second) {
                    targetSerial = std::to_string(dev.serial);
                    break;
                }
            }
        }
    }
    pw_metadata_set_property(metadata_, outId, "target.object", nullptr,
                            targetSerial.empty() ? nullptr : targetSerial.c_str());
}

void PipeWireManager::applyAllRoutesLocked() {
    for (const ChannelId ch : kAllChannels) {
        if (ch == ChannelId::Microphone) {
            continue;  // the mic is a source, not a routable output
        }
        applyChannelRouteLocked(ch);
    }
}

void PipeWireManager::onMeterProcess(void* data) {
    auto* io = static_cast<ChannelIO*>(data);
    pw_buffer* buffer = pw_stream_dequeue_buffer(io->meter);
    if (buffer == nullptr) {
        return;
    }

    const spa_data& d = buffer->buffer->datas[0];
    const auto* samples = static_cast<const float*>(d.data);
    if (samples != nullptr && d.chunk != nullptr) {
        const std::uint32_t count = d.chunk->size / sizeof(float);
        float peakLeft = 0.0f;
        float peakRight = 0.0f;
        for (std::uint32_t i = 0; i + 1 < count; i += 2) {  // interleaved stereo
            peakLeft = std::max(peakLeft, std::fabs(samples[i]));
            peakRight = std::max(peakRight, std::fabs(samples[i + 1]));
        }
        // Keep the max seen since the UI last read (which resets via exchange).
        if (peakLeft > io->peakLeft.load(std::memory_order_relaxed)) {
            io->peakLeft.store(peakLeft, std::memory_order_relaxed);
        }
        if (peakRight > io->peakRight.load(std::memory_order_relaxed)) {
            io->peakRight.store(peakRight, std::memory_order_relaxed);
        }
    }

    pw_stream_queue_buffer(io->meter, buffer);
}

// --- Static libpipewire trampolines ------------------------------------------

void PipeWireManager::onCoreInfo(void* data, const pw_core_info* info) {
    auto* self = static_cast<PipeWireManager*>(data);
    if (info == nullptr) {
        return;
    }
    self->info_.version  = info->version   != nullptr ? info->version   : "";
    self->info_.name     = info->name      != nullptr ? info->name      : "";
    self->info_.userName = info->user_name != nullptr ? info->user_name : "";
    self->info_.hostName = info->host_name != nullptr ? info->host_name : "";
}

void PipeWireManager::onCoreDone(void* data, std::uint32_t id, int seq) {
    auto* self = static_cast<PipeWireManager*>(data);
    if (id == PW_ID_CORE && seq == self->pendingSeq_) {
        self->roundtripDone_ = true;
        pw_thread_loop_signal(self->loop_, false);
    }
}

void PipeWireManager::onCoreError(void* data, std::uint32_t id, int /*seq*/, int /*res*/,
                                  const char* message) {
    auto* self = static_cast<PipeWireManager*>(data);
    if (id == PW_ID_CORE) {
        self->errored_ = true;
        self->errorMessage_ = message != nullptr ? message : "unknown error";
        pw_thread_loop_signal(self->loop_, false);
    }
}

void PipeWireManager::onRegistryGlobal(void* data, std::uint32_t id, std::uint32_t /*perm*/,
                                       const char* type, std::uint32_t /*version*/,
                                       const spa_dict* props) {
    static_cast<PipeWireManager*>(data)->onGlobal(id, type, props);
}

void PipeWireManager::onRegistryGlobalRemove(void* data, std::uint32_t id) {
    static_cast<PipeWireManager*>(data)->onGlobalRemove(id);
}

}  // namespace sonar::audio
