// The macOS layer's staging buffers (src/platform/staging_ring.h): the CPU never writes a buffer a
// command buffer in flight still reads, when a present submits nothing (no drawable) and whatever
// order the command buffers complete in. The Metal calls themselves need a Mac; this is the
// bookkeeping present() follows, with a semaphore of N tokens as in platform_cocoa.mm.
#include "test.h"
#include "platform/staging_ring.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

using plat::StagingRing;

namespace {
constexpr int kN = 3;

// present() step by step, without the GPU: the slots of the command buffers in flight.
struct Presenter {
    StagingRing<kN> ring;
    int tokens = kN;
    std::deque<int> inFlight;
    bool overwrote = false;  // the CPU wrote a slot that a command buffer in flight reads

    // A frame: the token, the slot written, then submitted or not (no drawable). The slot, -1 when
    // no token was free (the frame is skipped).
    int present(bool drawable) {
        if (tokens == 0) return -1;
        --tokens;
        const int slot = ring.acquire();
        if (slot < 0) {
            ++tokens;
            return -1;
        }
        for (int s : inFlight) overwrote |= s == slot;
        if (!drawable) {
            ++tokens;
            return slot;
        }
        ring.submitted(slot);
        inFlight.push_back(slot);
        return slot;
    }
    void complete(size_t which = 0) {
        const int slot = inFlight[which];
        inFlight.erase(inFlight.begin() + long(which));
        ring.completed(slot);
        ++tokens;
    }
};
}  // namespace

TEST(staging_ring_failed_present_keeps_its_slot) {
    // Two frames on the GPU, a third without a drawable, then a fourth: before, the rotation had
    // moved past the failed frame's buffer and the fourth wrote buffer 0, still read by the first.
    Presenter p;
    CHECK_EQ(p.present(true), 0);
    CHECK_EQ(p.present(true), 1);
    CHECK_EQ(p.present(false), 2);
    CHECK_EQ(p.present(true), 2);
    CHECK(!p.overwrote);
    CHECK_EQ(p.tokens, 0);
    // No token, no frame; the first completes, and its buffer is the next one written.
    CHECK_EQ(p.present(true), -1);
    p.complete();
    CHECK_EQ(p.present(true), 0);
    CHECK(!p.overwrote);
}

TEST(staging_ring_out_of_order_completions) {
    Presenter p;
    p.present(true);
    p.present(true);
    p.present(true);
    // The second completes first: its buffer is the only free one.
    p.complete(1);
    CHECK(p.ring.busy(0) && !p.ring.busy(1) && p.ring.busy(2));
    CHECK_EQ(p.present(true), 1);
    CHECK(!p.overwrote);
}

TEST(staging_ring_random_failures_never_overwrite) {
    std::mt19937 rng(1234);
    Presenter p;
    for (int i = 0; i < 20000; ++i) {
        const int r = int(rng() % 10);
        if (r < 6) {
            p.present(r != 0);  // one frame in six without a drawable
        } else if (!p.inFlight.empty()) {
            p.complete(size_t(rng() % p.inFlight.size()));
        }
        REQUIRE(!p.overwrote);
        REQUIRE(int(p.inFlight.size()) + p.tokens == kN);
    }
}

TEST(staging_ring_completions_on_another_thread) {
    // The completed handlers run on Metal's thread: a slot is free before its token comes back, so
    // the presenting thread always finds one that is not read.
    StagingRing<kN> ring;
    std::mutex m;
    std::condition_variable cv;
    int tokens = kN;
    std::deque<int> queue;
    std::atomic<bool> done{false}, overwrite{false};
    std::atomic<int> reading[kN] = {};
    std::thread gpu([&] {
        std::mt19937 rng(99);
        for (;;) {
            int slot;
            {
                std::unique_lock<std::mutex> lock(m);
                cv.wait(lock, [&] { return !queue.empty() || done; });
                if (queue.empty()) return;
                slot = queue.front();
                queue.pop_front();
            }
            std::this_thread::sleep_for(std::chrono::microseconds(rng() % 200));
            reading[slot] = 0;
            ring.completed(slot);
            std::lock_guard<std::mutex> lock(m);
            ++tokens;
            cv.notify_all();
        }
    });
    std::mt19937 rng(7);
    for (int frame = 0; frame < 5000; ++frame) {
        {
            std::unique_lock<std::mutex> lock(m);
            cv.wait(lock, [&] { return tokens > 0; });
            --tokens;
        }
        const int slot = ring.acquire();
        REQUIRE(slot >= 0);
        if (reading[slot]) overwrite = true;
        if (rng() % 5 == 0) {  // no drawable
            std::lock_guard<std::mutex> lock(m);
            ++tokens;
            continue;
        }
        reading[slot] = 1;
        ring.submitted(slot);
        std::lock_guard<std::mutex> lock(m);
        queue.push_back(slot);
        cv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lock(m);
        done = true;
        cv.notify_all();
    }
    gpu.join();
    CHECK(!overwrite);
}
