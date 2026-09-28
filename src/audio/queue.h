// Bounded lock-free multi-producer multi-consumer queue (D. Vyukov's sequence-number ring).
// push/pop never block and never allocate; push fails when full, pop when empty.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audio {

template <class T, size_t N>
class MpmcQueue {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");

public:
    MpmcQueue() {
        for (size_t i = 0; i < N; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
        enq_.store(0, std::memory_order_relaxed);
        deq_.store(0, std::memory_order_relaxed);
    }
    MpmcQueue(const MpmcQueue&) = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    bool push(const T& v) {
        size_t pos = enq_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells_[pos & (N - 1)];
            size_t seq = c.seq.load(std::memory_order_acquire);
            intptr_t dif = intptr_t(seq) - intptr_t(pos);
            if (dif == 0) {
                if (enq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    c.data = v;
                    c.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (dif < 0) {
                return false;
            } else {
                pos = enq_.load(std::memory_order_relaxed);
            }
        }
    }

    bool pop(T& v) {
        size_t pos = deq_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& c = cells_[pos & (N - 1)];
            size_t seq = c.seq.load(std::memory_order_acquire);
            intptr_t dif = intptr_t(seq) - intptr_t(pos + 1);
            if (dif == 0) {
                if (deq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    v = c.data;
                    c.seq.store(pos + N, std::memory_order_release);
                    return true;
                }
            } else if (dif < 0) {
                return false;
            } else {
                pos = deq_.load(std::memory_order_relaxed);
            }
        }
    }

private:
    struct Cell {
        std::atomic<size_t> seq;
        T data;
    };
    alignas(64) Cell cells_[N];
    alignas(64) std::atomic<size_t> enq_;
    alignas(64) std::atomic<size_t> deq_;
};

}  // namespace audio
