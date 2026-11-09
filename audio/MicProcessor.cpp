#include "audio/MicProcessor.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>

#include "audio/PwStream.h"
#include "dsp/DspMath.h"
#include "core/Log.h"

namespace sonar::audio {

namespace {

// 48 kHz, which the chain is built for and the noise model requires; PipeWire
// resamples a device that disagrees.
constexpr std::uint32_t kSampleRate = 48000;

// 10 ms. Short enough that someone monitoring themselves does not notice, long
// enough that the graph is not woken more often than it needs to be.
constexpr const char* kNodeLatency = "480/48000";

// About a third of a second — slack for when the two streams run on different drivers.
constexpr std::uint32_t kRingSize = 16384;

// Processing block. Bigger than any buffer PipeWire hands over at this latency,
// and on the stack rather than allocated, because this runs on the RT thread.
constexpr int kMaxBlock = 8192;

// The monitor runs on the output's clock, so past this backlog it skips ahead
// rather than let the delay on your own voice grow.
constexpr std::uint32_t kMonitorMaxBacklog = 1920;   // 40 ms
constexpr std::uint32_t kMonitorSkipTo = 960;        // 20 ms

}  // namespace

MicProcessor::MicProcessor()
    : scratch_(kMaxBlock, 0.0f), ring_(kRingSize), monitorRing_(kRingSize) {
    chain_.prepare(static_cast<float>(kSampleRate));
    noise_.prepare(static_cast<float>(kSampleRate));
    const dsp::MicSettings initial = dsp::presetSettings(dsp::MicPreset::Podcast);
    settings_.amend([&](dsp::MicSettings& s) { s = initial; });
    chain_.setSettings(initial);
    noiseEnabled_.store(initial.noiseSuppression, std::memory_order_relaxed);
}

MicProcessor::~MicProcessor() { stop(); }

// --- Settings handoff ---------------------------------------------------------

void MicProcessor::setSettings(const dsp::MicSettings& settings) {
    // The noise stage is an atomic, not part of the try_lock handoff: one bool that
    // should apply on the next block even mid-drag.
    noiseEnabled_.store(settings.noiseSuppression, std::memory_order_relaxed);
    settings_.set(settings);
}

dsp::MicSettings MicProcessor::settings() const { return settings_.get(); }
void MicProcessor::setEqualizer(const dsp::EqSettings& eq) { eq_.set(eq); }
dsp::EqSettings MicProcessor::equalizer() const { return eq_.get(); }

void MicProcessor::setNoiseSuppressionEnabled(bool on) {
    // Kept in the settings too, so a later setSettings() from the page does not
    // silently undo a toggle made through this path.
    settings_.amend([on](dsp::MicSettings& s) { s.noiseSuppression = on; });
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
        PW_KEY_NODE_LATENCY, kNodeLatency,
        // Both ends in one scheduling group: apart, the source side has no driver and
        // stalls the graph for every client.
        "node.group", group.c_str(),
        "node.link-group", group.c_str(),
        "node.virtual", "true",
        "node.want-driver", "true",
        nullptr);
    captureKeptRunning_ = live_ || monitor_ != nullptr;
    if (!captureKeptRunning_) {
        pw_properties_set(props, PW_KEY_NODE_PASSIVE, "true");
    }
    if (!captureTarget.empty()) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, captureTarget.c_str());
        // Fallback is PipeWireManager's decision; the session manager's would be the
        // default source, which can be Sonero Microphone itself.
        pw_properties_set(props, "node.dont-fallback", "true");
    }

    // Mono in: PipeWire folds a stereo device down for free. Autoconnect only with a
    // target — the default input may be a Bluetooth headset, and grabbing it forces HFP.
    capture_ = openF32Stream<&MicProcessor::onCaptureProcess>(
        core, name.c_str(), props, captureListener_, this, PW_DIRECTION_INPUT,
        rtStreamFlags(!captureTarget.empty()), kSampleRate, 1);
    if (capture_ == nullptr) {
        log::warn("Microphone: could not open the capture stream");
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(targetMutex_);
        captureTarget_ = captureTarget;
    }
    return true;
}

void MicProcessor::destroyCapture() { closeStream(capture_, captureListener_); }

bool MicProcessor::start(pw_core* core, const std::string& nodeName,
                         const std::string& description, const std::string& captureTarget) {
    if (core == nullptr) return false;
    stop();
    core_ = core;
    nodeName_ = nodeName;
    description_ = description;
    const std::string group = nodeName_ + "-chain";

    // The source side first, so a microphone that fails to connect still leaves apps
    // a (silent) Sonero Microphone. No media.category, as on module-loopback's source.
    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CLASS, "Audio/Source",
        PW_KEY_NODE_NAME, nodeName.c_str(),
        PW_KEY_NODE_DESCRIPTION, description.c_str(),
        PW_KEY_NODE_LATENCY, kNodeLatency,
        "audio.position", "[ FL, FR ]",
        // Same group as the capture side and module-loopback's flags. want-driver, not
        // driver: this node does not own a clock, and claiming one wedged the graph.
        "node.group", group.c_str(),
        "node.link-group", group.c_str(),
        "node.virtual", "true",
        "node.want-driver", "true",
        nullptr);

    // AUTOCONNECT even though nothing links out of a source: it is how the session
    // manager assigns it a driver. Without one the cycle never completes and PipeWire
    // stops answering every client.
    playback_ = openF32Stream<&MicProcessor::onPlaybackProcess>(
        core, nodeName.c_str(), props, playbackListener_, this, PW_DIRECTION_OUTPUT,
        rtStreamFlags(true), kSampleRate, 2);
    if (playback_ == nullptr) {
        log::warn("Microphone: could not open the source stream");
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
    if (core == nullptr || playback_ == nullptr) return false;
    destroyCapture();
    // The chain carries the previous room's envelopes and the previous device's
    // DC offset. Neither belongs to the new one.
    chain_.reset();
    noise_.reset();
    return createCapture(core, captureTarget);
}

void MicProcessor::setLive(pw_core* core, bool live) {
    live_ = live;
    applyKeepRunning(core);
}

void MicProcessor::applyKeepRunning(pw_core* core) {
    // Unlike a device change this leaves the chain's state alone: it is the same
    // microphone in the same room, and only the stream around it is new.
    const bool wanted = live_ || monitor_ != nullptr;
    if (core == nullptr || playback_ == nullptr || capture_ == nullptr ||
        wanted == captureKeptRunning_) {
        return;
    }
    const std::string target = captureTarget();
    destroyCapture();
    createCapture(core, target);
}

bool MicProcessor::createMonitor(pw_core* core) {
    const std::string name = nodeName_ + ".monitor";
    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_NODE_NAME, name.c_str(),
        PW_KEY_NODE_DESCRIPTION, "Sonero microphone monitor",
        PW_KEY_NODE_LATENCY, kNodeLatency,
        // Virtual for the same reason as a channel's output stream: a screen
        // share would otherwise send your own voice a second time.
        "node.virtual", "true",
        nullptr);

    monitor_ = openF32Stream<&MicProcessor::onMonitorProcess>(
        core, name.c_str(), props, monitorListener_, this, PW_DIRECTION_OUTPUT,
        rtStreamFlags(true), kSampleRate, 1);
    if (monitor_ == nullptr) {
        log::warn("Microphone: could not open the monitor stream");
        return false;
    }
    return true;
}

void MicProcessor::destroyMonitor() {
    monitorOn_.store(false, std::memory_order_relaxed);
    closeStream(monitor_, monitorListener_);
}

void MicProcessor::setMonitor(pw_core* core, bool on, float level) {
    monitorGain_.store(std::clamp(level, 0.0f, 1.0f), std::memory_order_relaxed);
    if (core == nullptr || playback_ == nullptr || on == (monitor_ != nullptr)) return;
    if (on) {
        if (createMonitor(core)) {
            monitorOn_.store(true, std::memory_order_relaxed);
        }
    } else {
        destroyMonitor();
    }
    applyKeepRunning(core);
}

void MicProcessor::stop() {
    destroyMonitor();
    destroyCapture();
    closeStream(playback_, playbackListener_);
    core_ = nullptr;
    ring_.reset();
    sawAudio_.store(false, std::memory_order_relaxed);
}

// --- Realtime -----------------------------------------------------------------

void MicProcessor::onCaptureProcess(void* data) {
    static_cast<MicProcessor*>(data)->processCapture();
}

void MicProcessor::onPlaybackProcess(void* data) {
    static_cast<MicProcessor*>(data)->processPlayback();
}

void MicProcessor::onMonitorProcess(void* data) {
    static_cast<MicProcessor*>(data)->processMonitor();
}

void MicProcessor::processCapture() {
    pw_buffer* buffer = pw_stream_dequeue_buffer(capture_);
    if (buffer == nullptr) return;
    const dsp::ScopedFlushDenormals ftz;

    // Pick up whatever the UI parked for us.
    settings_.take([this](const auto& s) { chain_.setSettings(s); });
    eq_.take([this](const auto& eq) { chain_.setEqualizer(eq); });
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

            // The model first, so the gate judges a floor with the room already removed.
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

            // The output meter reads what actually went into the ring.
            const std::uint32_t written =
                ring_.push(scratch_.data(), static_cast<std::uint32_t>(block));
            float outPeak = 0.0f;
            for (std::uint32_t i = 0; i < written; ++i) {
                outPeak = std::max(outPeak, std::abs(scratch_[i]));
            }
            if (outPeak > outputPeak_.load(std::memory_order_relaxed)) {
                outputPeak_.store(outPeak, std::memory_order_relaxed);
            }

            if (monitorOn_.load(std::memory_order_relaxed)) {
                monitorRing_.push(scratch_.data(), static_cast<std::uint32_t>(block));
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
    if (buffer == nullptr) return;

    spa_data& d = buffer->buffer->datas[0];
    auto* out = static_cast<float*>(d.data);
    // Checking chunk turns an unexpected buffer into silence instead of a crash.
    if (out != nullptr && d.chunk != nullptr) {
        constexpr std::uint32_t kStride = sizeof(float) * 2;
        const std::uint32_t frames = framesToProduce(buffer, d.maxsize, kStride, kMaxBlock);
        const std::uint32_t taken = std::min(frames, ring_.available());

        // Silence on underrun, not the last sample held: a repeated sample is a tone.
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float sample = i < taken ? *ring_.frame(i) : 0.0f;
            out[i * 2] = sample;
            out[i * 2 + 1] = sample;
        }
        ring_.consume(taken);

        d.chunk->offset = 0;
        d.chunk->stride = static_cast<std::int32_t>(kStride);
        d.chunk->size = frames * kStride;
    }

    pw_stream_queue_buffer(playback_, buffer);
}

void MicProcessor::processMonitor() {
    pw_buffer* buffer = pw_stream_dequeue_buffer(monitor_);
    if (buffer == nullptr) return;

    spa_data& d = buffer->buffer->datas[0];
    auto* out = static_cast<float*>(d.data);
    if (out != nullptr && d.chunk != nullptr) {
        const std::uint32_t frames =
            framesToProduce(buffer, d.maxsize, sizeof(float), kMaxBlock);
        std::uint32_t available = monitorRing_.available();
        if (available > kMonitorMaxBacklog) {
            monitorRing_.consume(available - kMonitorSkipTo);
            available = kMonitorSkipTo;
        }
        const std::uint32_t taken = std::min(frames, available);
        const float gain = monitorGain_.load(std::memory_order_relaxed);
        for (std::uint32_t i = 0; i < frames; ++i) {
            out[i] = i < taken ? *monitorRing_.frame(i) * gain : 0.0f;
        }
        monitorRing_.consume(taken);

        d.chunk->offset = 0;
        d.chunk->stride = sizeof(float);
        d.chunk->size = frames * sizeof(float);
    }

    pw_stream_queue_buffer(monitor_, buffer);
}

}  // namespace sonar::audio
