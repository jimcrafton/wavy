#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace wavy {

// Wait-free single-producer single-consumer ring of N slots (N a power of two); push and pop never allocate or lock.
template <class T, size_t N>
class SpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");

public:
    bool push(const T& value) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) == N) return false;
        slots_[head & (N - 1)] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& value) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        value = slots_[tail & (N - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Producer side only.
    bool full() const { return head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire) == N; }

private:
    std::array<T, N> slots_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

}  // namespace wavy
