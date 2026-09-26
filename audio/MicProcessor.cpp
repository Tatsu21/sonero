#include "audio/MicProcessor.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <pipewire/pipewire.h>
#include <pipewire/version.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>

#include "core/Log.h"

namespace sonar::audio {

namespace {

// The chain is built for 48 kHz and the noise model refuses to run at anything
// else, so the streams ask for it and let PipeWire resample the device if it
// disagrees. Resampling a microphone once is cheaper than the alternative, which
// is a model quietly hearing every frequency in the wrong place.
constexpr std::uint32_t kSampleRate = 48000;

// 10 ms. Short enough that someone monitoring themselves does not notice, long
// enough that the graph is not woken more often than it needs to be.
constexpr const char* kNodeLatency = "480/48000";

// Room for about a third of a second. The two streams normally run in the same
// graph cycle and the ring stays nearly empty; the slack is for the case where
// they do not, which happens whenever the source and the microphone end up on
// different drivers.
constexpr std::uint32_t kRingSize = 16384;

// Processing block. Bigger than any buffer PipeWire hands over at this latency,
// and on the stack rather than allocated, because this runs on the RT thread.
constexpr int kMaxBlock = 8192;

}  // namespace

MicProcessor::MicProcessor() : ring_(kRingSize, 0.0f), scratch_(kMaxBlock, 0.0f) {
    chain_.prepare(static_cast<float>(kSampleRate));
    noise_.prepare(static_cast<float>(kSampleRate));
    pendingSettings_ = dsp::presetSettings(dsp::MicPreset::Podcast);
    chain_.setSettings(pendingSettings_);
    noiseEnabled_.store(pendingSettings_.noiseSuppression, std::memory_order_relaxed);
}

MicProcessor::~MicProcessor() { stop(); }

// --- Settings handoff ---------------------------------------------------------

void MicProcessor::setSettings(const dsp::MicSettings& settings) {
    {
        const std::lock_guard<std::mutex> lock(settingsMutex_);
        pendingSettings_ = settings;
    }
    // The noise stage is read from an atomic rather than from the settings the
    // audio thread picks up under try_lock: switching it is one bool, and it
    // should take effect on the next block even while a drag is holding the lock.
    noiseEnabled_.store(settings.noiseSuppression, std::memory_order_relaxed);
    settingsDirty_.store(true, std::memory_order_release);
}

dsp::MicSettings MicProcessor::settings() const {
    const std::lock_guard<std::mutex> lock(settingsMutex_);
    return pendingSettings_;
}

void MicProcessor::setEqualizer(const dsp::EqSettings& eq) {
    {
        const std::lock_guard<std::mutex> lock(settingsMutex_);
        pendingEq_ = eq;
    }
    eqDirty_.store(true, std::memory_order_release);
}

dsp::EqSettings MicProcessor::equalizer() const {
    const std::lock_guard<std::mutex> lock(settingsMutex_);
    return pendingEq_;
}

void MicProcessor::setNoiseSuppressionEnabled(bool on) {
    {
        // Kept in the settings too, so a later setSettings() from the page does
        // not silently undo a toggle made through this path.
        const std::lock_guard<std::mutex> lock(settingsMutex_);
        pendingSettings_.noiseSuppression = on;
    }
    noiseEnabled_.store(on, std::memory_order_relaxed);
}

bool MicProcessor::noiseSuppressionEnabled() const {
    return noiseEnabled_.load(std::memory_order_relaxed);
}

bool MicProcessor::noiseSuppressionAvailable() { return dsp::NoiseSuppressor::available(); }

std::string_view MicProcessor::noiseSuppressionBackend() {
    return dsp::NoiseSuppressor::backendName();
}

float MicProcessor::latencyMs() const {
    const int samples = chain_.latencySamples() + noise_.latencySamples();
    return 1000.0f * static_cast<float>(samples) / static_cast<float>(kSampleRate);
}

MicLevels MicProcessor::takeLevels() {
    MicLevels levels;
    levels.inputPeak = inputPeak_.exchange(0.0f, std::memory_order_relaxed);
    levels.outputPeak = outputPeak_.exchange(0.0f, std::memory_order_relaxed);
    levels.gateReductionDb = gateReduction_.load(std::memory_order_relaxed);
    levels.deEsserReductionDb = deEsserReduction_.load(std::memory_order_relaxed);
    levels.compressorReductionDb = compressorReduction_.load(std::memory_order_relaxed);
    levels.limiterReductionDb = limiterReduction_.load(std::memory_order_relaxed);
    levels.noiseReductionDb = noiseReduction_.load(std::memory_order_relaxed);
    levels.speechProbability = speechProbability_.load(std::memory_order_relaxed);
    levels.gateOpen = gateOpen_.load(std::memory_order_relaxed);
    levels.active = capture_ != nullptr && sawAudio_.load(std::memory_order_relaxed);
    return levels;
}

std::string MicProcessor::captureTarget() const {
    const std::lock_guard<std::mutex> lock(targetMutex_);
    return captureTarget_;
}

// --- Streams ------------------------------------------------------------------

bool MicProcessor::createCapture(pw_core* core, const std::string& captureTarget) {
    const std::string name = nodeName_ + ".input";
    const std::string group = nodeName_ + "-chain";

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Communication",
        PW_KEY_NODE_NAME, name.c_str(),
        PW_KEY_NODE_DESCRIPTION, "Sonero microphone input",
        PW_KEY_NODE_PASSIVE, "true",
        PW_KEY_NODE_LATENCY, kNodeLatency,
        // Both ends of the chain belong to one scheduling group. Without this
        // they are two unrelated islands in the graph: the capture side gets the
        // microphone's clock, and the source side gets none at all — which does
        // not merely stay silent, it stalls the graph for everyone, because
        // PipeWire keeps looking for a driver that will never appear.
        "node.group", group.c_str(),
        "node.link-group", group.c_str(),
        "node.virtual", "true",
        "node.want-driver", "true",
        nullptr);
    if (!captureTarget.empty()) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, captureTarget.c_str());
    }

    capture_ = pw_stream_new(core, name.c_str(), props);
    if (capture_ == nullptr) {
        log::warn("Microphone: could not create the capture stream");
        return false;
    }

    static const pw_stream_events kCaptureEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .process = &MicProcessor::onCaptureProcess,
    };
    pw_stream_add_listener(capture_, &captureListener_, &kCaptureEvents, this);

    // Mono in: a microphone is one signal, and letting PipeWire's converter fold
    // a stereo device down is both correct and free. The chain then has one
    // stream to process instead of two that only differ by noise.
    std::uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw format = {};
    format.format = SPA_AUDIO_FORMAT_F32;
    format.rate = kSampleRate;
    format.channels = 1;
    format.position[0] = SPA_AUDIO_CHANNEL_MONO;

    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &format);

    // Autoconnect only with a target. Without one the capture would grab
    // whatever the system default input is, and if that is a Bluetooth headset
    // it drags the whole card into HFP — mono, 8 kHz, and the music stops.
    auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS |
                                              PW_STREAM_FLAG_RT_PROCESS);
    if (!captureTarget.empty()) {
        flags = static_cast<pw_stream_flags>(flags | PW_STREAM_FLAG_AUTOCONNECT);
    }
    if (pw_stream_connect(capture_, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1) < 0) {
        log::warn("Microphone: could not connect the capture stream");
        destroyCapture();
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(targetMutex_);
        captureTarget_ = captureTarget;
    }
    return true;
}

void MicProcessor::destroyCapture() {
    if (capture_ != nullptr) {
        spa_hook_remove(&captureListener_);
        pw_stream_destroy(capture_);
        capture_ = nullptr;
    }
}

bool MicProcessor::start(pw_core* core, const std::string& nodeName,
                         const std::string& description, const std::string& captureTarget) {
    if (core == nullptr) {
        return false;
    }
    stop();
    core_ = core;
    nodeName_ = nodeName;
    description_ = description;
    const std::string group = nodeName_ + "-chain";

    // The source side first, so that a microphone which fails to connect still
    // leaves applications with a (silent) Sonero Microphone to select rather
    // than an input device that vanished.
    // No media.category here. The category tells pw_stream what kind of client
    // this is, and "Playback" would describe a node that feeds a sink — the
    // opposite of what media.class says this is. module-loopback's source side,
    // which is the working example in this very process, sets the class and
    // nothing else.
    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CLASS, "Audio/Source",
        PW_KEY_NODE_NAME, nodeName.c_str(),
        PW_KEY_NODE_DESCRIPTION, description.c_str(),
        PW_KEY_NODE_LATENCY, kNodeLatency,
        "audio.position", "[ FL, FR ]",
        // The same group the capture side joins, and the same three flags
        // module-loopback sets on its own source — which is the working example
        // running in this very process. want-driver, not driver: this node
        // produces samples, it does not own a clock, and saying otherwise is
        // what wedged the graph.
        "node.group", group.c_str(),
        "node.link-group", group.c_str(),
        "node.virtual", "true",
        "node.want-driver", "true",
        nullptr);

    playback_ = pw_stream_new(core, nodeName.c_str(), props);
    if (playback_ == nullptr) {
        log::warn("Microphone: could not create the source stream");
        return false;
    }

    static const pw_stream_events kPlaybackEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .process = &MicProcessor::onPlaybackProcess,
    };
    pw_stream_add_listener(playback_, &playbackListener_, &kPlaybackEvents, this);

    std::uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw format = {};
    format.format = SPA_AUDIO_FORMAT_F32;
    format.rate = kSampleRate;
    format.channels = 2;
    format.position[0] = SPA_AUDIO_CHANNEL_FL;
    format.position[1] = SPA_AUDIO_CHANNEL_FR;

    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &format);

    // AUTOCONNECT even though nothing links *out* of a source. It is not about
    // links: it is how the session manager adopts the node and gives it a driver.
    // Without it the node sits there marked node.trigger — waiting to be
    // triggered by a driver it was never assigned — and the graph cycle it
    // belongs to never completes. That does not fail loudly: it stops PipeWire
    // answering anybody, new clients included, for as long as Sonero runs.
    if (pw_stream_connect(playback_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                       PW_STREAM_FLAG_MAP_BUFFERS |
                                                       PW_STREAM_FLAG_RT_PROCESS),
                          params, 1) < 0) {
        log::warn("Microphone: could not connect the source stream");
        stop();
        return false;
    }

    if (!createCapture(core, captureTarget)) {
        // Keep the source alive anyway — see above.
        log::warn("Microphone: running with no input device attached");
    }

    log::info("Microphone: node '{}' up (noise suppression: {})", nodeName,
              std::string(noiseSuppressionBackend()));
    return true;
}

bool MicProcessor::setCaptureTarget(pw_core* core, const std::string& captureTarget) {
    if (core == nullptr || playback_ == nullptr) {
        return false;
    }
    destroyCapture();
    // The chain carries the previous room's envelopes and the previous device's
    // DC offset. Neither belongs to the new one.
    chain_.reset();
    noise_.reset();
    return createCapture(core, captureTarget);
}

void MicProcessor::stop() {
    destroyCapture();
    if (playback_ != nullptr) {
        spa_hook_remove(&playbackListener_);
        pw_stream_destroy(playback_);
        playback_ = nullptr;
    }
    core_ = nullptr;
    ringWrite_.store(0, std::memory_order_relaxed);
    ringRead_.store(0, std::memory_order_relaxed);
    sawAudio_.store(false, std::memory_order_relaxed);
}

// --- Realtime -----------------------------------------------------------------

void MicProcessor::onCaptureProcess(void* data) {
    static_cast<MicProcessor*>(data)->processCapture();
}

void MicProcessor::onPlaybackProcess(void* data) {
    static_cast<MicProcessor*>(data)->processPlayback();
}

void MicProcessor::processCapture() {
    pw_buffer* buffer = pw_stream_dequeue_buffer(capture_);
    if (buffer == nullptr) {
        return;
    }

    // Pick up whatever the UI parked for us. try_lock, never lock: missing one
    // block of a slider drag is invisible, and blocking here is a dropout.
    if (settingsDirty_.load(std::memory_order_acquire) ||
        eqDirty_.load(std::memory_order_acquire)) {
        if (settingsMutex_.try_lock()) {
            if (settingsDirty_.exchange(false, std::memory_order_acq_rel)) {
                chain_.setSettings(pendingSettings_);
            }
            if (eqDirty_.exchange(false, std::memory_order_acq_rel)) {
                chain_.setEqualizer(pendingEq_);
            }
            settingsMutex_.unlock();
        }
    }
    noise_.setEnabled(noiseEnabled_.load(std::memory_order_relaxed));

    const spa_data& d = buffer->buffer->datas[0];
    if (d.data != nullptr && d.chunk != nullptr && d.chunk->size > 0) {
        const auto* base = static_cast<const std::uint8_t*>(d.data) + d.chunk->offset;
        const auto* samples = reinterpret_cast<const float*>(base);
        int count = static_cast<int>(d.chunk->size / sizeof(float));

        while (count > 0) {
            const int block = std::min(count, kMaxBlock);
            std::copy_n(samples, block, scratch_.begin());

            float inPeak = 0.0f;
            for (int i = 0; i < block; ++i) {
                inPeak = std::max(inPeak, std::abs(scratch_[static_cast<std::size_t>(i)]));
            }
            if (inPeak > inputPeak_.load(std::memory_order_relaxed)) {
                inputPeak_.store(inPeak, std::memory_order_relaxed);
            }

            // The model first, then the chain: the gate should be deciding
            // against a floor that has already had the room taken out of it,
            // otherwise its threshold is set against noise rather than silence.
            noise_.process(scratch_.data(), block);
            chain_.process(scratch_.data(), block);

            const dsp::MicMeters meters = chain_.meters();
            gateReduction_.store(meters.gateReductionDb, std::memory_order_relaxed);
            deEsserReduction_.store(meters.deEsserReductionDb, std::memory_order_relaxed);
            compressorReduction_.store(meters.compressorReductionDb, std::memory_order_relaxed);
            limiterReduction_.store(meters.limiterReductionDb, std::memory_order_relaxed);
            gateOpen_.store(meters.gateOpen, std::memory_order_relaxed);
            noiseReduction_.store(noise_.reductionDb(), std::memory_order_relaxed);
            speechProbability_.store(noise_.speechProbability(), std::memory_order_relaxed);

            float outPeak = 0.0f;
            const std::uint32_t write = ringWrite_.load(std::memory_order_relaxed);
            const std::uint32_t read = ringRead_.load(std::memory_order_acquire);
            const std::uint32_t free = kRingSize - (write - read) - 1;
            const auto writable = static_cast<int>(std::min<std::uint32_t>(
                free, static_cast<std::uint32_t>(block)));
            for (int i = 0; i < writable; ++i) {
                const float sample = scratch_[static_cast<std::size_t>(i)];
                outPeak = std::max(outPeak, std::abs(sample));
                ring_[(write + static_cast<std::uint32_t>(i)) % kRingSize] = sample;
            }
            ringWrite_.store(write + static_cast<std::uint32_t>(writable),
                             std::memory_order_release);
            if (outPeak > outputPeak_.load(std::memory_order_relaxed)) {
                outputPeak_.store(outPeak, std::memory_order_relaxed);
            }

            samples += block;
            count -= block;
        }
        sawAudio_.store(true, std::memory_order_relaxed);
    }

    pw_stream_queue_buffer(capture_, buffer);
}

void MicProcessor::processPlayback() {
    pw_buffer* buffer = pw_stream_dequeue_buffer(playback_);
    if (buffer == nullptr) {
        return;
    }

    spa_data& d = buffer->buffer->datas[0];
    auto* out = static_cast<float*>(d.data);
    // chunk is checked on the capture side too. A buffer without one is not
    // something PipeWire hands over in practice, but writing through it is the
    // difference between silence and a crash inside the audio thread.
    if (out != nullptr && d.chunk != nullptr) {
        constexpr std::uint32_t kStride = sizeof(float) * 2;
        // maxsize is the size of the shared buffer, which can be far larger than
        // one graph quantum. Filling all of it every cycle is work nobody asked
        // for, on the one thread that must never run long.
        std::uint32_t frames = std::min<std::uint32_t>(d.maxsize / kStride, kMaxBlock);
#if PW_CHECK_VERSION(0, 3, 49)
        // pw_buffer::requested says how much the consumer actually wants, which
        // is less than the buffer can hold for most of a graph cycle. It arrived
        // in 0.3.49, and Ubuntu 22.04 — a target, because its glibc is what the
        // portable AppImage is built against — ships 0.3.48. Without it the block
        // above is the bound: more work than needed, never a wrong result.
        if (buffer->requested != 0) {
            frames = std::min<std::uint32_t>(frames, static_cast<std::uint32_t>(buffer->requested));
        }
#endif

        const std::uint32_t read = ringRead_.load(std::memory_order_relaxed);
        const std::uint32_t write = ringWrite_.load(std::memory_order_acquire);
        const std::uint32_t available = write - read;
        const std::uint32_t taken = std::min(frames, available);

        for (std::uint32_t i = 0; i < frames; ++i) {
            // Silence on underrun rather than the last sample held: a repeated
            // sample is a tone, and a tone is far more noticeable on a call than
            // a gap of the same length.
            const float sample = i < taken ? ring_[(read + i) % kRingSize] : 0.0f;
            out[i * 2] = sample;
            out[i * 2 + 1] = sample;
        }
        ringRead_.store(read + taken, std::memory_order_release);

        d.chunk->offset = 0;
        d.chunk->stride = static_cast<std::int32_t>(kStride);
        d.chunk->size = frames * kStride;
    }

    pw_stream_queue_buffer(playback_, buffer);
}

}  // namespace sonar::audio
