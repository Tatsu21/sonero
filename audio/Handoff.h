#pragma once

#include <atomic>
#include <mutex>

namespace sonar::audio {

// A value the UI writes and the realtime thread picks up when it can. The reader
// only try_locks: missing one block of a drag is invisible, blocking is a dropout.
template <typename T>
class Handoff {
public:
    explicit Handoff(T initial = {}) : pending_(std::move(initial)) {}

    void set(const T& value) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            pending_ = value;
        }
        dirty_.store(true, std::memory_order_release);
    }

    // Change the parked value without asking the reader to pick it up.
    template <typename Change>
    void amend(Change&& change) {
        const std::lock_guard<std::mutex> lock(mutex_);
        change(pending_);
    }

    [[nodiscard]] T get() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return pending_;
    }

    // Realtime thread: when a new value is waiting and the lock is free, hand it
    // to `apply` (by reference, so nothing is copied or allocated here).
    template <typename Apply>
    void take(Apply&& apply) {
        if (!dirty_.load(std::memory_order_acquire)) return;
        const std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (lock.owns_lock() && dirty_.exchange(false, std::memory_order_acq_rel)) {
            apply(pending_);
        }
    }

private:
    mutable std::mutex mutex_;
    T pending_;
    std::atomic<bool> dirty_{false};
};

}  // namespace sonar::audio
