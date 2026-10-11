// Internal to the macOS layer: which of its staging buffers the CPU may write. Header-only, for the
// unit tests (tests/staging_ring_tests.cpp).
//
// present() (platform_cocoa.mm) copies each frame into a staging buffer in shared memory, then a
// command buffer blits it into the layer's next drawable. A buffer is busy from the commit of the
// command buffer that reads it until that command buffer completes; the CPU must not write it
// meanwhile. A present that submits nothing (no drawable: nextDrawable may return nil, no command
// buffer) leaves its buffer free, and the rotation where it was.
//
// The caller holds one of N semaphore tokens while it writes, and a completed handler frees its
// slot before it gives its token back: at most N - 1 other slots are then busy, and a free one is
// always there, whatever order the command buffers complete in.
#pragma once
#include <atomic>

namespace plat {

template <int N>
class StagingRing {
public:
    StagingRing() {
        for (std::atomic<bool>& b : busy_) b.store(false, std::memory_order_relaxed);
    }
    // The slot to write: the first free one from the rotation's place; -1 when all are busy (never
    // while the caller holds a token).
    int acquire() const {
        for (int k = 0; k < N; ++k) {
            const int i = (next_ + k) % N;
            if (!busy_[i].load(std::memory_order_acquire)) return i;
        }
        return -1;
    }
    // Its command buffer is about to be committed (before the commit: it may complete at once). The
    // rotation moves past it.
    void submitted(int slot) {
        busy_[slot].store(true, std::memory_order_relaxed);
        next_ = (slot + 1) % N;
    }
    // From the command buffer's completed handler (any thread), before its token is given back.
    void completed(int slot) { busy_[slot].store(false, std::memory_order_release); }
    bool busy(int slot) const { return busy_[slot].load(std::memory_order_acquire); }

private:
    std::atomic<bool> busy_[N];
    int next_ = 0;  // the presenting thread's only
};

}  // namespace plat
