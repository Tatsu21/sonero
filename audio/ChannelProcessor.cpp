#include "audio/ChannelProcessor.h"

#include <algorithm>
#include <cmath>

#include <pipewire/pipewire.h>
#include <pipewire/version.h>
#include <spa/param/audio/format-utils.h>

#include "core/Log.h"

namespace sonar::audio {

namespace {

// The chain is built for 48 kHz and the nodes ask for it, letting PipeWire
// resample a device that disagrees. The crossover and the equalizer's band
// frequencies are frequencies: a chain told 48 kHz while running at 44.1 would
// put every filter in the wrong place.
constexpr std::uint32_t kSampleRate = 48000;

// 10 ms. Short enough not to matter on playback, long enough that the graph is
// not woken more often than it needs to be.
constexpr const char* kNodeLatency = "480/48000";

// Room for about a third of a second of stereo. The two nodes normally run in
// the same graph cycle and the ring stays nearly empty; the slack is for when
// they do not, which happens whenever the sink and the output device end up on
// different drivers.
constexpr std::uint32_t kRingFrames = 16384;

// Processing block, in frames.
constexpr int kMaxFrames = 4096;

}  // namespace

ChannelProcessor::ChannelProcessor()
    : scratch_(static_cast<std::size_t>(kMaxFrames) * 2, 0.0f),
      ring_(static_cast<std::size_t>(kRingFrames) * 2, 0.0f) {
    chain_.prepare(static_cast<float>(kSampleRate));
}

ChannelProcessor::~ChannelProcessor() { stop(); }

// --- Settings handoff ---------------------------------------------------------

void ChannelProcessor::setSettings(const dsp::ChannelChainSettings& settings) {
    {
        const std::lock_guard<std::mutex> lock(settingsMutex_);
        pendingSettings_ = settings;
    }
    settingsDirty_.store(true, std::memory_order_release);
}

dsp::ChannelChainSettings ChannelProcessor::settings() const {
    const std::lock_guard<std::mutex> lock(settingsMutex_);
    return pendingSettings_;
}

void ChannelProcessor::setEqualizer(const dsp::EqSettings& eq) {
    {
        const std::lock_guard<std::mutex> lock(settingsMutex_);
        pendingEq_ = eq;
    }
    eqDirty_.store(true, std::memory_order_release);
}

dsp::EqSettings ChannelProcessor::equalizer() const {
    const std::lock_guard<std::mutex> lock(settingsMutex_);
    return pendingEq_;
}

void ChannelProcessor::setBass(const dsp::BassStageSettings& bass) {
    {
        const std::lock_guard<std::mutex> lock(settingsMutex_);
        pendingBass_ = bass;
    }
    bassDirty_.store(true, std::memory_order_release);
}

dsp::BassStageSettings ChannelProcessor::bass() const {
    const std::lock_guard<std::mutex> lock(settingsMutex_);
    return pendingBass_;
}

float ChannelProcessor::latencyMs() const {
    // The lookahead is fixed by prepare() and never written again, so reading it
    // from here races with nothing.
    const std::lock_guard<std::mutex> lock(settingsMutex_);
    if (!pendingBass_.limiterEnabled) {
        return 0.0f;
    }
    return 1000.0f * static_cast<float>(chain_.latencySamples()) /
           static_cast<float>(kSampleRate);
}

ChannelProcessorLevels ChannelProcessor::takeLevels() {
    ChannelProcessorLevels levels;
    levels.peakLeft = peakLeft_.exchange(0.0f, std::memory_order_relaxed);
    levels.peakRight = peakRight_.exchange(0.0f, std::memory_order_relaxed);
    levels.inputPeak = inputPeak_.exchange(0.0f, std::memory_order_relaxed);
    levels.limiterReductionDb = limiterReduction_.load(std::memory_order_relaxed);
    levels.clippedSamples = clipped_.exchange(0, std::memory_order_relaxed);
    levels.bassLowDb = bassLowDb_.load(std::memory_order_relaxed);
    levels.bassHarmonicsDb = bassHarmonicsDb_.load(std::memory_order_relaxed);
    levels.bassActive = bassActive_.load(std::memory_order_relaxed);
    levels.active = sink_ != nullptr && sawAudio_.load(std::memory_order_relaxed);
    return levels;
}

// --- Nodes --------------------------------------------------------------------

bool ChannelProcessor::start(pw_core* core, const std::string& nodeName,
                             const std::string& description) {
    if (core == nullptr) {
        return false;
    }
    stop();
    core_ = core;
    nodeName_ = nodeName;
    description_ = description;
    const std::string outName = nodeName_ + ".out";

    std::uint8_t buffer[1024];
    spa_audio_info_raw format = {};
    format.format = SPA_AUDIO_FORMAT_F32;
    format.rate = kSampleRate;
    format.channels = 2;
    format.position[0] = SPA_AUDIO_CHANNEL_FL;
    format.position[1] = SPA_AUDIO_CHANNEL_FR;

    // The sink side first, so that an output which fails to connect still leaves
    // applications with a (silent) channel to route into rather than a device
    // that vanished from the list while they were using it.
    //
    // Deliberately absent: node.group, node.link-group and node.want-driver. See
    // the class comment — setting them on a pair with no real device in it is
    // what stalled the whole graph when this was first attempted.
    pw_properties* sinkProps = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CLASS, "Audio/Sink",
        PW_KEY_NODE_NAME, nodeName.c_str(),
        PW_KEY_NODE_DESCRIPTION, description.c_str(),
        PW_KEY_NODE_LATENCY, kNodeLatency,
        "audio.position", "[ FL, FR ]",
        // Same ranking module-filter-chain's sinks carried, so the session
        // manager treats these the way it treated those.
        "priority.session", "100",
        "node.virtual", "true",
        nullptr);

    sink_ = pw_stream_new(core, nodeName.c_str(), sinkProps);
    if (sink_ == nullptr) {
        log::warn("Channel '{}': could not create the sink node", nodeName);
        return false;
    }

    static const pw_stream_events kSinkEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .process = &ChannelProcessor::onSinkProcess,
    };
    pw_stream_add_listener(sink_, &sinkListener_, &kSinkEvents, this);

    spa_pod_builder sinkBuilder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod* sinkParams[1] = {
        spa_format_audio_raw_build(&sinkBuilder, SPA_PARAM_EnumFormat, &format)};

    // No AUTOCONNECT on the sink: applications link into it, and asking the
    // session manager to connect it as well makes it try to treat a destination
    // as a stream.
    if (pw_stream_connect(sink_, PW_DIRECTION_INPUT, PW_ID_ANY,
                          static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS |
                                                       PW_STREAM_FLAG_RT_PROCESS),
                          sinkParams, 1) < 0) {
        log::warn("Channel '{}': could not connect the sink node", nodeName);
        stop();
        return false;
    }

    // The output side. Named "<node>.out" because that is the name
    // applyChannelRouteLocked() looks for when retargeting a channel to a
    // device, and what the stream-mix send attaches to. Keeping the convention
    // is what lets everything built around the filter-chain channels carry on
    // working unchanged.
    std::uint8_t outBuffer[1024];
    spa_pod_builder outBuilder = SPA_POD_BUILDER_INIT(outBuffer, sizeof(outBuffer));
    const spa_pod* outParams[1] = {
        spa_format_audio_raw_build(&outBuilder, SPA_PARAM_EnumFormat, &format)};

    pw_properties* outProps = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_NODE_NAME, outName.c_str(),
        PW_KEY_NODE_DESCRIPTION, (description + " output").c_str(),
        PW_KEY_NODE_LATENCY, kNodeLatency,
        "audio.position", "[ FL, FR ]",
        nullptr);

    out_ = pw_stream_new(core, outName.c_str(), outProps);
    if (out_ == nullptr) {
        log::warn("Channel '{}': could not create the output node", nodeName);
        stop();
        return false;
    }

    static const pw_stream_events kOutEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .process = &ChannelProcessor::onOutProcess,
    };
    pw_stream_add_listener(out_, &outListener_, &kOutEvents, this);

    // AUTOCONNECT here, so the session manager links it to the default output —
    // and then honours the target.object the router sets on it. Not passive: a
    // passive output never activates the device it feeds, and with no other node
    // in this pair to do so the chain would never be scheduled.
    if (pw_stream_connect(out_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                       PW_STREAM_FLAG_MAP_BUFFERS |
                                                       PW_STREAM_FLAG_RT_PROCESS),
                          outParams, 1) < 0) {
        log::warn("Channel '{}': could not connect the output node", nodeName);
        stop();
        return false;
    }

    return true;
}

void ChannelProcessor::stop() {
    if (out_ != nullptr) {
        spa_hook_remove(&outListener_);
        pw_stream_destroy(out_);
        out_ = nullptr;
    }
    if (sink_ != nullptr) {
        spa_hook_remove(&sinkListener_);
        pw_stream_destroy(sink_);
        sink_ = nullptr;
    }
    core_ = nullptr;
    ringWrite_.store(0, std::memory_order_relaxed);
    ringRead_.store(0, std::memory_order_relaxed);
    sawAudio_.store(false, std::memory_order_relaxed);
}

// --- Realtime -----------------------------------------------------------------

void ChannelProcessor::onSinkProcess(void* data) {
    static_cast<ChannelProcessor*>(data)->processSink();
}

void ChannelProcessor::onOutProcess(void* data) {
    static_cast<ChannelProcessor*>(data)->processOut();
}

void ChannelProcessor::processSink() {
    pw_buffer* buffer = pw_stream_dequeue_buffer(sink_);
    if (buffer == nullptr) {
        return;
    }

    // Pick up whatever the UI parked. try_lock, never lock: missing one block of
    // a slider drag is invisible, and blocking here is a dropout.
    if (settingsDirty_.load(std::memory_order_acquire) ||
        eqDirty_.load(std::memory_order_acquire) ||
        bassDirty_.load(std::memory_order_acquire)) {
        if (settingsMutex_.try_lock()) {
            if (settingsDirty_.exchange(false, std::memory_order_acq_rel)) {
                chain_.setSettings(pendingSettings_);
            }
            if (eqDirty_.exchange(false, std::memory_order_acq_rel)) {
                chain_.setEqualizer(pendingEq_);
            }
            if (bassDirty_.exchange(false, std::memory_order_acq_rel)) {
                chain_.setBass(pendingBass_);
            }
            settingsMutex_.unlock();
        }
    }

    const spa_data& d = buffer->buffer->datas[0];
    if (d.data != nullptr && d.chunk != nullptr && d.chunk->size > 0) {
        constexpr std::uint32_t kStride = sizeof(float) * 2;
        const auto* base = static_cast<const std::uint8_t*>(d.data) + d.chunk->offset;
        const auto* samples = reinterpret_cast<const float*>(base);
        int frames = static_cast<int>(d.chunk->size / kStride);

        while (frames > 0) {
            const int block = std::min(frames, kMaxFrames);
            const std::size_t count = static_cast<std::size_t>(block) * 2;
            std::copy_n(samples, count, scratch_.begin());

            chain_.process(scratch_.data(), block);

            const dsp::ChannelMeters m = chain_.meters();
            if (m.peakLeft > peakLeft_.load(std::memory_order_relaxed)) {
                peakLeft_.store(m.peakLeft, std::memory_order_relaxed);
            }
            if (m.peakRight > peakRight_.load(std::memory_order_relaxed)) {
                peakRight_.store(m.peakRight, std::memory_order_relaxed);
            }
            if (m.inputPeak > inputPeak_.load(std::memory_order_relaxed)) {
                inputPeak_.store(m.inputPeak, std::memory_order_relaxed);
            }
            limiterReduction_.store(m.limiterReductionDb, std::memory_order_relaxed);
            bassLowDb_.store(m.bass.lowBandDb, std::memory_order_relaxed);
            bassHarmonicsDb_.store(m.bass.harmonicsDb, std::memory_order_relaxed);
            bassActive_.store(m.bass.active, std::memory_order_relaxed);
            if (m.clippedSamples > 0) {
                clipped_.fetch_add(m.clippedSamples, std::memory_order_relaxed);
            }

            const std::uint32_t write = ringWrite_.load(std::memory_order_relaxed);
            const std::uint32_t read = ringRead_.load(std::memory_order_acquire);
            const std::uint32_t freeFrames = kRingFrames - (write - read) - 1;
            const auto writable = static_cast<int>(
                std::min<std::uint32_t>(freeFrames, static_cast<std::uint32_t>(block)));
            for (int i = 0; i < writable; ++i) {
                const std::size_t src = static_cast<std::size_t>(i) * 2;
                const std::size_t dst =
                    static_cast<std::size_t>(
                        (write + static_cast<std::uint32_t>(i)) % kRingFrames) * 2;
                ring_[dst] = scratch_[src];
                ring_[dst + 1] = scratch_[src + 1];
            }
            ringWrite_.store(write + static_cast<std::uint32_t>(writable),
                             std::memory_order_release);

            samples += count;
            frames -= block;
        }
        sawAudio_.store(true, std::memory_order_relaxed);
    }

    pw_stream_queue_buffer(sink_, buffer);
}

void ChannelProcessor::processOut() {
    pw_buffer* buffer = pw_stream_dequeue_buffer(out_);
    if (buffer == nullptr) {
        return;
    }

    spa_data& d = buffer->buffer->datas[0];
    auto* out = static_cast<float*>(d.data);
    // chunk is checked on the sink side too. A buffer without one is not
    // something PipeWire hands over in practice, but writing through it is the
    // difference between silence and a crash inside the audio thread.
    if (out != nullptr && d.chunk != nullptr) {
        constexpr std::uint32_t kStride = sizeof(float) * 2;
        // maxsize is the shared buffer's size, which can be far larger than one
        // graph quantum. Filling all of it every cycle is work nobody asked for,
        // on the one thread that must never run long.
        std::uint32_t frames = std::min<std::uint32_t>(d.maxsize / kStride, kMaxFrames);
#if PW_CHECK_VERSION(0, 3, 49)
        // pw_buffer::requested says how much the consumer actually wants. It
        // arrived in 0.3.49, and Ubuntu 22.04 — a target, because its glibc is
        // what the portable AppImage is built against — ships 0.3.48. Without it
        // the bound above stands: more work than needed, never a wrong result.
        if (buffer->requested != 0) {
            frames = std::min<std::uint32_t>(
                frames, static_cast<std::uint32_t>(buffer->requested));
        }
#endif

        const std::uint32_t read = ringRead_.load(std::memory_order_relaxed);
        const std::uint32_t write = ringWrite_.load(std::memory_order_acquire);
        const std::uint32_t available = write - read;
        const std::uint32_t taken = std::min(frames, available);

        for (std::uint32_t i = 0; i < frames; ++i) {
            // Silence on underrun rather than the last frame held: a repeated
            // sample is a tone, and a tone is far more noticeable than a gap of
            // the same length.
            float l = 0.0f;
            float r = 0.0f;
            if (i < taken) {
                const std::size_t src =
                    static_cast<std::size_t>((read + i) % kRingFrames) * 2;
                l = ring_[src];
                r = ring_[src + 1];
            }
            out[i * 2] = l;
            out[i * 2 + 1] = r;
        }
        ringRead_.store(read + taken, std::memory_order_release);

        d.chunk->offset = 0;
        d.chunk->stride = static_cast<std::int32_t>(kStride);
        d.chunk->size = frames * kStride;
    }

    pw_stream_queue_buffer(out_, buffer);
}

}  // namespace sonar::audio
