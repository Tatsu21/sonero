#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonar::audio {

// Lock-free single-writer/single-reader ring of audio frames. Counted in frames so
// a partial write never splits a stereo pair. Indices run free and wrap at 2^32.
template <std::size_t Channels>
class FrameRing {
public:
    explicit FrameRing(std::uint32_t capacityFrames)
        : capacity_(capacityFrames), data_(static_cast<std::size_t>(capacityFrames) * Channels) {}

    // --- Writer ---

    // Append up to `frames` interleaved frames; whatever does not fit is dropped.
    // Returns how many went in.
    std::uint32_t push(const float* interleaved, std::uint32_t frames) {
        const std::uint32_t write = write_.load(std::memory_order_relaxed);
        const std::uint32_t read = read_.load(std::memory_order_acquire);
        const std::uint32_t room = capacity_ - (write - read) - 1;
        const std::uint32_t count = std::min(room, frames);
        for (std::uint32_t i = 0; i < count; ++i) {
            float* dst = slot(write + i);
            const float* src = interleaved + static_cast<std::size_t>(i) * Channels;
            for (std::size_t c = 0; c < Channels; ++c) {
                dst[c] = src[c];
            }
        }
        write_.store(write + count, std::memory_order_release);
        return count;
    }

    // --- Reader ---

    [[nodiscard]] std::uint32_t available() const {
        return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_relaxed);
    }

    // The i-th frame from the read position; i < available().
    [[nodiscard]] const float* frame(std::uint32_t i) const {
        return slot(read_.load(std::memory_order_relaxed) + i);
    }

    // Release `frames` frames back to the writer.
    void consume(std::uint32_t frames) {
        read_.store(read_.load(std::memory_order_relaxed) + frames, std::memory_order_release);
    }

    // --- Neither callback running ---

    void reset() {
        write_.store(0, std::memory_order_relaxed);
        read_.store(0, std::memory_order_relaxed);
    }

private:
    [[nodiscard]] float* slot(std::uint32_t index) {
        return data_.data() + static_cast<std::size_t>(index % capacity_) * Channels;
    }
    [[nodiscard]] const float* slot(std::uint32_t index) const {
        return data_.data() + static_cast<std::size_t>(index % capacity_) * Channels;
    }

    const std::uint32_t capacity_;
    std::vector<float> data_;
    std::atomic<std::uint32_t> write_{0};
    std::atomic<std::uint32_t> read_{0};
};

// Frames to produce into a pw_buffer this cycle: maxsize bounded by `maxFrames`,
// and by pw_buffer::requested where it exists (PipeWire >= 0.3.49; Ubuntu 22.04
// ships 0.3.48, hence the `requires`).
template <typename Buffer>
[[nodiscard]] std::uint32_t framesToProduce(const Buffer* buffer, std::uint32_t maxsizeBytes,
                                            std::uint32_t strideBytes, std::uint32_t maxFrames) {
    std::uint32_t frames = std::min(maxsizeBytes / strideBytes, maxFrames);
    if constexpr (requires { buffer->requested; }) {
        if (buffer->requested != 0) {
            frames = std::min(frames, static_cast<std::uint32_t>(buffer->requested));
        }
    }
    return frames;
}

}  // namespace sonar::audio
