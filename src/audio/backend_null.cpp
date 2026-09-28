// Null backend (Linux/tests): renders 10 ms blocks at real-time pace and discards them.
#ifndef _WIN32
#include "backend.h"
#include "dsp.h"
#include <chrono>
#include <thread>
#include <vector>

namespace audio {
namespace {

class NullBackend final : public Backend {
public:
    ~NullBackend() override { stop(); }

    bool start(RenderFn fn, void* user) override {
        if (thread_.joinable()) return true;
        quit_.store(false);
        status.sampleRate = kRate;
        status.bufferFrames = kBlock;
        status.deviceOpen = true;
        thread_ = std::thread([this, fn, user] { run(fn, user); });
        return true;
    }

    void stop() override {
        quit_.store(true);
        if (thread_.joinable()) thread_.join();
        status.deviceOpen = false;
    }

private:
    static constexpr int kRate = 48000;
    static constexpr int kBlock = 480;  // 10 ms

    void run(RenderFn fn, void* user) {
        dsp::DenormalGuard guard;
        std::vector<float> buf(size_t(kBlock) * 2);
        using clock = std::chrono::steady_clock;
        auto next = clock::now();
        const auto period = std::chrono::microseconds(1000000LL * kBlock / kRate);
        while (!quit_.load(std::memory_order_relaxed)) {
            fn(user, buf.data(), kBlock, kRate);
            next += period;
            auto now = clock::now();
            if (now - next > std::chrono::milliseconds(200)) next = now;  // stalled: resync, no burst
            std::this_thread::sleep_until(next);
        }
    }

    std::thread thread_;
    std::atomic<bool> quit_{false};
};

}  // namespace

std::unique_ptr<Backend> createBackend() { return std::unique_ptr<Backend>(new NullBackend()); }

}  // namespace audio
#endif
