// Null backend (no device, the tests, SCACELITH_AUDIO=null): renders 10 ms blocks at real-time
// pace and discards them, or streams them to a 16-bit stereo WAV file when
// SCACELITH_AUDIO_DUMP=<path.wav> is set (to hear what the game played, e.g. the coach's speech
// against its gestures, without a device).
// Each start() rewrites the file; the header sizes are patched at stop().
#ifndef _WIN32
#include "backend.h"
#include "dsp.h"
#include "../core/files.h"
#include "../core/log.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace audio {
namespace {

class WavDump {
public:
    ~WavDump() { close(); }

    bool open(const char* path, int rate) {
        close();
        f_ = files::create(path);
        if (!f_) return false;
        bytes_ = 0;
        header(rate);
        return true;
    }
    bool isOpen() const { return f_ != nullptr; }

    void write(const float* stereo, int frames) {
        if (!f_ || bytes_ > kMaxBytes) return;
        buf_.resize(size_t(frames) * 4u);
        for (int i = 0; i < 2 * frames; ++i) {
            float v = stereo[i];
            if (!std::isfinite(v)) v = 0.0f;
            long q = std::lrint(double(v) * 32767.0 + double(rng_.uni() - rng_.uni()));  // TPDF dither
            q = q > 32767 ? 32767 : (q < -32768 ? -32768 : q);
            const uint16_t u = uint16_t(int16_t(q));
            buf_[2 * size_t(i)] = uint8_t(u);
            buf_[2 * size_t(i) + 1] = uint8_t(u >> 8);
        }
        bytes_ += uint32_t(std::fwrite(buf_.data(), 1, buf_.size(), f_));
    }

    void close() {
        if (!f_) return;
        std::fseek(f_, 0, SEEK_SET);
        header(rate_);
        std::fclose(f_);
        f_ = nullptr;
    }

private:
    static constexpr uint32_t kMaxBytes = 0xF0000000u;  // stay inside the 32-bit RIFF sizes (~5.5 h)

    void header(int rate) {
        rate_ = rate;
        uint8_t h[44];
        auto u32 = [&h](int at, uint32_t v) { for (int k = 0; k < 4; ++k) h[at + k] = uint8_t(v >> (8 * k)); };
        auto u16 = [&h](int at, uint16_t v) { h[at] = uint8_t(v); h[at + 1] = uint8_t(v >> 8); };
        std::memcpy(h, "RIFF", 4);
        u32(4, 36u + bytes_);
        std::memcpy(h + 8, "WAVEfmt ", 8);
        u32(16, 16u);
        u16(20, 1);                        // PCM
        u16(22, 2);                        // stereo
        u32(24, uint32_t(rate));
        u32(28, uint32_t(rate) * 4u);      // byte rate
        u16(32, 4);                        // block align
        u16(34, 16);                       // bits
        std::memcpy(h + 36, "data", 4);
        u32(40, bytes_);
        std::fwrite(h, 1, sizeof(h), f_);
    }

    FILE* f_ = nullptr;
    uint32_t bytes_ = 0;
    int rate_ = 48000;
    std::vector<uint8_t> buf_;
    dsp::Rng rng_{12345u};
};

class NullBackend final : public Backend {
public:
    ~NullBackend() override { stop(); }

    bool start(RenderFn fn, void* user) override {
        if (thread_.joinable()) return true;
        quit_.store(false);
        const char* env = std::getenv("SCACELITH_AUDIO_DUMP");
        if (env && *env) {
            // Written under the file name given (a pipe such as /dev/stdout stays one), in its folder
            // resolved once (weakly_canonical: absolute, no "." or ".." parts, no symbolic link where
            // it exists): the path the log names.
            const std::filesystem::path given(env);
            std::error_code ec;
            const std::filesystem::path folder =
                std::filesystem::weakly_canonical(given.has_parent_path() ? given.parent_path() : ".", ec);
            const std::string path = (folder / given.filename()).string();
            if (!ec && dump_.open(path.c_str(), kRate)) LOGI("audio: dumping the output to %s", path.c_str());
            else LOGW("audio: cannot write the output dump %s", env);
        }
        status.sampleRate = kRate;
        status.bufferFrames = kBlock;
        status.deviceOpen = true;
        thread_ = std::thread([this, fn, user] { run(fn, user); });
        return true;
    }

    void stop() override {
        quit_.store(true);
        if (thread_.joinable()) thread_.join();
        dump_.close();
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
            dump_.write(buf.data(), kBlock);  // no-op unless dumping
            next += period;
            auto now = clock::now();
            if (now - next > std::chrono::milliseconds(200)) next = now;  // stalled: resync, no burst
            std::this_thread::sleep_until(next);
        }
    }

    std::thread thread_;
    std::atomic<bool> quit_{false};
    WavDump dump_;
};

}  // namespace

std::unique_ptr<Backend> createNullBackend() { return std::unique_ptr<Backend>(new NullBackend()); }

#ifndef __linux__   // Linux: backend_alsa.cpp
std::unique_ptr<Backend> createBackend() { return createNullBackend(); }
#endif

}  // namespace audio
#endif
