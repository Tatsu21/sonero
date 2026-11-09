#include "audio/ChannelProcessor.h"

#include <algorithm>
#include <cmath>

#include "audio/PwStream.h"
#include "dsp/DspMath.h"
#include "core/Log.h"

namespace sonar::audio {

namespace {

// The chain is built for 48 kHz and the nodes ask for it, so PipeWire resamples a
// device that disagrees rather than every filter landing in the wrong place.
constexpr std::uint32_t kSampleRate = 48000;

// 10 ms. Short enough not to matter on playback, long enough that the graph is
// not woken more often than it needs to be.
constexpr const char* kNodeLatency = "480/48000";

// About a third of a second of stereo — slack for when the two nodes run on
// different drivers.
constexpr std::uint32_t kRingFrames = 16384;

// Processing block, in frames.
constexpr int kMaxFrames = 4096;

// Held back on top of one cycle before playing, and again after an underrun:
// nothing orders the two nodes within a cycle, and without this margin every flip
// is a hole mid-buffer (the crackle on non-default outputs).
constexpr std::uint32_t kPrefillFrames = 256;  // ~5 ms

// Past this much queued on top of one cycle, skip ahead to the prefill level:
// bounds the latency if the two sides ever end up on different clocks.
constexpr std::uint32_t kMaxBacklogFrames = 2048;  // ~43 ms

}  // namespace

ChannelProcessor::ChannelProcessor()
    : scratch_(static_cast<std::size_t>(kMaxFrames) * 2, 0.0f),
      ring_(kRingFrames) {
    chain_.prepare(static_cast<float>(kSampleRate));
}

ChannelProcessor::~ChannelProcessor() { stop(); }

// --- Settings handoff ---------------------------------------------------------

void ChannelProcessor::setSettings(const dsp::ChannelChainSettings& s) { settings_.set(s); }
dsp::ChannelChainSettings ChannelProcessor::settings() const { return settings_.get(); }
void ChannelProcessor::setEqualizer(const dsp::EqSettings& eq) { eq_.set(eq); }
dsp::EqSettings ChannelProcessor::equalizer() const { return eq_.get(); }
void ChannelProcessor::setBass(const dsp::BassStageSettings& bass) { bass_.set(bass); }
dsp::BassStageSettings ChannelProcessor::bass() const { return bass_.get(); }

float ChannelProcessor::latencyMs() const {
    // The lookahead is fixed by prepare() and never written again, so reading it
    // from here races with nothing.
    if (!bass_.get().limiterEnabled) return 0.0f;
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
    if (core == nullptr) return false;
    stop();
    core_ = core;
    nodeName_ = nodeName;
    description_ = description;
    const std::string outName = nodeName_ + ".out";

    // Both nodes in one scheduling group so PipeWire gives them one driver (the
    // output's device); apart, they ran on two clocks and the ring crackled.
    // link-group too, so the output is never linked back into its own sink.
    const std::string group = nodeName_ + "-chain";

    // The sink side first, so a failed output still leaves apps a (silent) channel.
    // Never node.want-driver here — see the class comment.
    pw_properties* sinkProps = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CLASS, "Audio/Sink",
        PW_KEY_NODE_NAME, nodeName.c_str(),
        PW_KEY_NODE_DESCRIPTION, description.c_str(),
        PW_KEY_NODE_LATENCY, kNodeLatency,
        "audio.position", "[ FL, FR ]",
        "node.group", group.c_str(),
        "node.link-group", group.c_str(),
        // Same ranking module-filter-chain's sinks carried, so the session
        // manager treats these the way it treated those.
        "priority.session", "100",
        "node.virtual", "true",
        nullptr);

    // No AUTOCONNECT on the sink: apps link into it, and the session manager would
    // treat it as a stream.
    sink_ = openF32Stream<&ChannelProcessor::onSinkProcess>(
        core, nodeName.c_str(), sinkProps, sinkListener_, this, PW_DIRECTION_INPUT,
        rtStreamFlags(false), kSampleRate, 2);
    if (sink_ == nullptr) {
        log::warn("Channel '{}': could not open the sink node", nodeName);
        stop();
        return false;
    }

    // "<node>.out" is the name channel routing and the stream-mix send look for.
    pw_properties* outProps = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_NODE_NAME, outName.c_str(),
        PW_KEY_NODE_DESCRIPTION, (description + " output").c_str(),
        PW_KEY_NODE_LATENCY, kNodeLatency,
        "audio.position", "[ FL, FR ]",
        "node.group", group.c_str(),
        "node.link-group", group.c_str(),
        // Passive: the pair runs only while an application plays into the sink,
        // which wakes the shared group; an idle channel costs no CPU at all.
        PW_KEY_NODE_PASSIVE, "true",
        // Virtual, so a screen share does not capture it — Discord otherwise sent the call
        // back to itself and every other app twice.
        "node.virtual", "true",
        nullptr);

    // AUTOCONNECT so the session manager links it (and honours target.object).
    out_ = openF32Stream<&ChannelProcessor::onOutProcess>(
        core, outName.c_str(), outProps, outListener_, this, PW_DIRECTION_OUTPUT,
        rtStreamFlags(true), kSampleRate, 2);
    if (out_ == nullptr) {
        log::warn("Channel '{}': could not open the output node", nodeName);
        stop();
        return false;
    }

    return true;
}

void ChannelProcessor::stop() {
    closeStream(out_, outListener_);
    closeStream(sink_, sinkListener_);
    core_ = nullptr;
    ring_.reset();
    primed_ = false;
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
    if (buffer == nullptr) return;
    const dsp::ScopedFlushDenormals ftz;

    // Pick up whatever the UI parked.
    settings_.take([this](const auto& s) { chain_.setSettings(s); });
    eq_.take([this](const auto& eq) { chain_.setEqualizer(eq); });
    bass_.take([this](const auto& bass) { chain_.setBass(bass); });

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

            ring_.push(scratch_.data(), static_cast<std::uint32_t>(block));

            samples += count;
            frames -= block;
        }
        sawAudio_.store(true, std::memory_order_relaxed);
    }

    pw_stream_queue_buffer(sink_, buffer);
}

void ChannelProcessor::processOut() {
    pw_buffer* buffer = pw_stream_dequeue_buffer(out_);
    if (buffer == nullptr) return;

    spa_data& d = buffer->buffer->datas[0];
    auto* out = static_cast<float*>(d.data);
    // Checking chunk turns an unexpected buffer into silence instead of a crash.
    if (out != nullptr && d.chunk != nullptr) {
        constexpr std::uint32_t kStride = sizeof(float) * 2;
        const std::uint32_t frames = framesToProduce(buffer, d.maxsize, kStride, kMaxFrames);
        const std::uint32_t available = ring_.available();

        // Wait for a cycle plus the margin, and again after an underrun: one clean gap
        // instead of a short read every cycle.
        if (!primed_ && available >= frames + kPrefillFrames) {
            primed_ = true;
        } else if (primed_ && available < frames) {
            primed_ = false;
        }
        if (primed_ && available > frames + kMaxBacklogFrames) {
            ring_.consume(available - (frames + kPrefillFrames));
        }
        const std::uint32_t taken = primed_ ? frames : 0;

        // Silence on underrun, not the last frame held: a repeated sample is a tone.
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float* f = i < taken ? ring_.frame(i) : nullptr;
            out[i * 2] = f != nullptr ? f[0] : 0.0f;
            out[i * 2 + 1] = f != nullptr ? f[1] : 0.0f;
        }
        ring_.consume(taken);

        d.chunk->offset = 0;
        d.chunk->stride = static_cast<std::int32_t>(kStride);
        d.chunk->size = frames * kStride;
    }

    pw_stream_queue_buffer(out_, buffer);
}

}  // namespace sonar::audio
