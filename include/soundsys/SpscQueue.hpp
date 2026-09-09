// soundsys/SpscQueue.hpp - bounded wait-free single-producer/single-consumer ring.
//
// Producer: the simulator thread. Consumer: the audio callback thread.
// No allocation, no locks, no CAS - just two atomics with acquire/release.
#pragma once

#include <atomic>
#include <array>
#include <cstddef>

namespace soundsys {

#if defined(_MSC_VER)
#pragma warning(push)
// C4324: the type is padded by the alignas below. That is the entire point -
// the producer and consumer indices must not share a cache line.
#pragma warning(disable : 4324)
#endif

template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    // Producer side. Returns false if the queue is full (caller decides whether
    // to drop or retry; the audio module always drops - never blocks the sim).
    bool push(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & kMask;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false; // full
        }
        slots_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side.
    bool pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // empty
        }
        out = slots_[tail];
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    std::array<T, Capacity> slots_{};
    // Padded to avoid false sharing between producer and consumer cache lines.
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace soundsys
