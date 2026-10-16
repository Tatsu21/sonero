#include "audio/PipeWireManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>

#include <pipewire/pipewire.h>
#include <pipewire/impl-module.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/format.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>

#include "core/Format.h"
#include "core/Log.h"
#include "dsp/Equalizer.h"

namespace sonar::audio {

namespace {
constexpr int kRoundtripTimeoutSeconds = 3;

// Node name of the stream mix; its monitor is what OBS captures.
constexpr const char* kStreamMixNode = "sonero_stream";



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

// Expose the stream mix as a real capture device, so it appears in a recorder's
// input list next to the microphone rather than hiding as a sink monitor — which
// is where OBS's "Audio Input Capture" looks.
std::string streamSourceArgs(const std::string& mixNode, const std::string& sourceNode) {
    return fmt::format(
        R"({{ capture.props = {{ node.name = "{1}.capture" target.object = "{0}" )"
        R"(stream.capture.sink = true node.passive = true audio.position = [ FL FR ] }} )"
        R"(playback.props = {{ node.name = "{1}" media.class = Audio/Source )"
        R"(node.description = "Sonero Stream" audio.position = [ FL FR ] }} }})",
        mixNode, sourceNode);
}

// One send per channel: tap the channel's monitor and feed the stream mix, with a
// level of its own. A loopback rather than a second filter-chain output, because
// the send must be attenuated independently of what the user hears.
std::string streamSendArgs(const std::string& channelNode, const std::string& mixNode) {
    return fmt::format(
        R"({{ capture.props = {{ node.name = "{0}.stream" target.object = "{0}" )"
        R"(stream.capture.sink = true node.passive = true audio.position = [ FL FR ] }} )"
        R"(playback.props = {{ node.name = "{0}.stream.out" target.object = "{1}" )"
        R"(node.passive = true audio.position = [ FL FR ] }} }})",
        channelNode, mixNode);
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
    for (const ChannelId id : kAllChannels) {
        const std::string node = nodeNameFor(id);
        const std::string desc = "Sonero " + std::string(channelName(id));

        // The Microphone channel is a virtual SOURCE (capture the real mic → the
        // voice chain → Audio/Source); every other channel is an Audio/Sink.
        // Never bind the mic capture to a Bluetooth input: doing so forces the
        // headset into HFP and kills its A2DP music quality. A BT mic and A2DP
        // playback cannot coexist, so we keep A2DP and leave the capture idle.
        if (id == ChannelId::Microphone) {
            const std::string micTarget =
                startsWith(realDefaultSource_, "bluez") ? std::string() : realDefaultSource_;
            if (!mic_.start(core_, node, desc, micTarget)) {
                log::warn("PipeWire: the microphone node could not be created");
            }
            continue;
        }

        // Every output channel is a pair of native nodes with dsp::ChannelChain
        // between them — equalizer, harmonic bass, limiter and fader, all ours.
        // This used to be module-filter-chain plus a LADSPA plugin dlopened into
        // it plus the sink's own channelVolumes; see audio/ChannelProcessor.h for
        // why one chain in our own code replaced all three.
        if (!processor_[channelIndex(id)].start(core_, node, desc)) {
            log::warn("PipeWire: channel '{}' could not be created", node);
        }
    }
    // Every channel but the microphone, which is a source and runs through mic_.
    log::info("PipeWire: {} output channels up, each with its own DSP chain",
              kAllChannels.size() - 1);

    // The stream mix and its per-channel sends. "adapter" is a factory the daemon
    // already provides, not a module we can load with arguments — creating the node
    // through it is what `pw-cli create-node adapter` does.
    pw_properties* mixProps = pw_properties_new(
        "factory.name", "support.null-audio-sink",
        PW_KEY_NODE_NAME, kStreamMixNode,
        PW_KEY_NODE_DESCRIPTION, "Sonero Stream",
        PW_KEY_MEDIA_CLASS, "Audio/Sink",
        "audio.position", "[ FL, FR ]",
        "monitor.channel-volumes", "true",
        "object.linger", "false",
        nullptr);
    streamMix_ = static_cast<pw_proxy*>(pw_core_create_object(
        core_, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
        &mixProps->dict, 0));
    pw_properties_free(mixProps);

    if (streamMix_ != nullptr) {
        // The capture-side face of the mix, which is what a recorder selects.
        if (pw_impl_module* src = pw_context_load_module(
                context_, "libpipewire-module-loopback",
                streamSourceArgs(kStreamMixNode, "sonero_stream_source").c_str(), nullptr)) {
            modules_.push_back(src);
        } else {
            log::warn("PipeWire: could not expose the stream mix as a source");
        }

        for (const ChannelId id : kAllChannels) {
            if (id == ChannelId::Microphone) {
                continue;  // the mic is already an input; it needs no send
            }
            const std::string args = streamSendArgs(nodeNameFor(id), kStreamMixNode);
            if (pw_impl_module* send = pw_context_load_module(
                    context_, "libpipewire-module-loopback", args.c_str(), nullptr)) {
                modules_.push_back(send);
            } else {
                log::warn("PipeWire: no stream send for '{}'", nodeNameFor(id));
            }
        }
    } else {
        log::warn("PipeWire: could not create the stream mix");
    }

    // modules_ is the stream mix and its per-channel sends now — the channel
    // sinks are native nodes and are not loaded as modules at all. The old
    // message called this number the channel sink count, which it no longer is.
    log::info("PipeWire: stream mix up ({} loopback modules)", modules_.size());
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

    // The microphone's streams go first, and here rather than in MicProcessor's
    // own destructor: members are destroyed after this body has run, by which
    // point the core and the loop they belong to are already gone, and
    // pw_stream_destroy on a freed core is a crash — which is exactly how this
    // was found, in PipeWireManagerTest.
    mic_.stop();

    // ...and the output channels' node pairs, for the same reason.
    for (ChannelProcessor& processor : processor_) {
        processor.stop();
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
                // The microphone, when MicProcessor owns it, is not a channel sink
                // and must not be treated as one. Binding it would apply the
                // channel's volume, mute and — worse — the filter-chain equalizer
                // control ports ("<node>_eq0:Gain") to a plain pw_stream that has
                // no such params. The audioadapter answers by enumerating its
                // properties again, which produces another info event, which
                // applies them again: a loop that spins our PipeWire thread loop
                // forever, so we stop answering the daemon and every other client
                // on the machine hangs with us.
                //
                // Nothing to bind: every channel's gain, mute, equalizer and
                // metering lives in the node's own chain — MicChain for the
                // microphone, dsp::ChannelChain for the outputs — rather than in
                // node properties we would have to set from out here.
                return;
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
        } else if (std::strcmp(mediaClass, "Stream/Input/Audio") == 0 &&
                   startsWith(nodeName, "sonero_") && endsWith(nodeName, ".stream")) {
            // The capture side of a channel's stream send. Remember its id so the
            // send level can be set later; the level itself is applied by binding
            // briefly, exactly as device volumes are.
            const std::string base = nodeName.substr(0, nodeName.size() - 7);
            if (const auto ch = channelForNodeName(base)) {
                std::lock_guard<std::mutex> lock(stateMutex_);
                streamSendNode_[static_cast<int>(*ch)] = id;
            }
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


void PipeWireManager::applyEqualizer(ChannelId id, const dsp::EqSettings& settings) {
    // The microphone's equalizer is a stage of its own chain, which takes the
    // whole curve at once; applying this page's copy to it would overwrite that.
    if (id == ChannelId::Microphone) {
        if (mic_.running()) {
            mic_.setEqualizer(settings);
        }
        return;
    }
    // No clamping, no headroom arithmetic and no property round trip here:
    // dsp::ChannelChain does all three, which is also what makes them testable
    // without a running graph.
    processor_[channelIndex(id)].setEqualizer(settings);
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
    ChannelProcessor& p = processor_[channelIndex(id)];
    dsp::ChannelChainSettings s = p.settings();
    s.volume = std::clamp(volume, 0.0f, 1.0f);
    p.setSettings(s);
}

void PipeWireManager::setChannelBalance(ChannelId id, float balance) {
    ChannelProcessor& p = processor_[channelIndex(id)];
    dsp::ChannelChainSettings s = p.settings();
    s.balance = std::clamp(balance, -1.0f, 1.0f);
    p.setSettings(s);
}

void PipeWireManager::setChannelGain(ChannelId id, float gain) {
    // The UI hands this over as a linear factor (0..2, i.e. up to +6 dB); the
    // chain wants decibels, which is what the slider was showing all along.
    ChannelProcessor& p = processor_[channelIndex(id)];
    dsp::ChannelChainSettings s = p.settings();
    const float clamped = std::clamp(gain, 0.01f, 2.0f);
    s.trimDb = 20.0f * std::log10(clamped);
    p.setSettings(s);
}

void PipeWireManager::setStreamLevel(ChannelId id, float level) {
    level = std::clamp(level, 0.0f, 1.0f);
    std::uint32_t nodeId = 0;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        streamLevel_[static_cast<int>(id)] = level;
        const auto it = streamSendNode_.find(static_cast<int>(id));
        if (it != streamSendNode_.end()) {
            nodeId = it->second;
        }
    }
    if (nodeId != 0) {
        setDeviceVolume(nodeId, level);  // same mechanism: bind, set, release
    }
}

float PipeWireManager::streamLevel(ChannelId id) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto it = streamLevel_.find(static_cast<int>(id));
    return it != streamLevel_.end() ? it->second : 1.0f;
}

void PipeWireManager::setChannelMute(ChannelId id, bool muted) {
    ChannelProcessor& p = processor_[channelIndex(id)];
    dsp::ChannelChainSettings s = p.settings();
    s.muted = muted;
    p.setSettings(s);
}

ChannelLevel PipeWireManager::channelLevel(ChannelId id) {
    // Straight out of the chain that produced it, rather than from a separate
    // capture stream tapping the node afterwards. That is six fewer nodes in the
    // graph, and the numbers are post-fader because that is where the chain
    // measures them — which is what someone looking at a mixer expects.
    const ChannelProcessorLevels levels = processor_[channelIndex(id)].takeLevels();
    ChannelLevel out;
    out.peakLeft = levels.peakLeft;
    out.peakRight = levels.peakRight;
    return out;
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


// --- IMicrophoneController ----------------------------------------------------
//
// Thin by design: MicProcessor already does its own thread handoff, so there is
// nothing here to lock and nothing to translate.

void PipeWireManager::setMicSettings(const dsp::MicSettings& settings) {
    mic_.setSettings(settings);
}

dsp::MicSettings PipeWireManager::micSettings() const { return mic_.settings(); }

MicLevels PipeWireManager::micLevels() { return mic_.takeLevels(); }

void PipeWireManager::setMicNoiseSuppression(bool on) { mic_.setNoiseSuppressionEnabled(on); }

bool PipeWireManager::micNoiseSuppression() const { return mic_.noiseSuppressionEnabled(); }

bool PipeWireManager::micNoiseSuppressionAvailable() const {
    return MicProcessor::noiseSuppressionAvailable();
}

std::string_view PipeWireManager::micNoiseSuppressionBackend() const {
    return MicProcessor::noiseSuppressionBackend();
}

bool PipeWireManager::setMicCaptureDevice(const std::string& nodeName) {
    if (loop_ == nullptr || core_ == nullptr) {
        return false;
    }
    // Rebuilding a stream touches the graph, so it happens with the loop held —
    // the same rule every other node change in this file follows.
    pw_thread_loop_lock(loop_);
    const bool ok = mic_.setCaptureTarget(core_, nodeName);
    pw_thread_loop_unlock(loop_);
    if (!ok) {
        log::warn("PipeWire: the microphone could not attach to '{}'", nodeName);
    }
    return ok;
}

std::string PipeWireManager::micCaptureDevice() const { return mic_.captureTarget(); }

float PipeWireManager::micLatencyMs() const { return mic_.latencyMs(); }

// --- Harmonic bass ------------------------------------------------------------

void PipeWireManager::applyBassStage(ChannelId id, const dsp::BassStageSettings& settings) {
    if (id == ChannelId::Microphone) {
        return;  // a source has no bass stage
    }
    processor_[channelIndex(id)].setBass(settings);
}

}  // namespace sonar::audio
