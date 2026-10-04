// ALSA backend (see backend_alsa.h). The device is opened non-blocking with snd_pcm_set_params
// (48 kHz stereo, float32, else int32, else int16; about 20 ms of buffer, the plug layer converting
// what the hardware does not take). A render thread waits for room (snd_pcm_wait), renders what
// fits and writes it; xruns and suspends are recovered in place (snd_pcm_recover); a device that
// fails for good (unplugged, sound server gone) is closed and reopened with the WASAPI back-off.
#ifdef __linux__
#include "backend_alsa.h"
#include "dsp.h"
#include "../core/log.h"
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace audio {
namespace {

// alsa-lib prints its own errors on stderr ("ALSA lib confmisc.c:..." for a machine without a
// sound card): the backend logs the reason itself.
void quietAlsa(const char*, int, const char*, int, const char*, ...) {}

class AlsaBackend final : public Backend {
public:
    AlsaBackend(const AlsaApi& api, std::string device) : api_(api), device_(std::move(device)) {}
    ~AlsaBackend() override {
        stop();
        closeDevice();
    }

    // Opens and configures the device (the first attempt runs before start()). false: lastError_.
    bool openDevice() {
        snd_pcm* pcm = nullptr;
        int err = api_.open(&pcm, device_.c_str(), alsa::kStreamPlayback, alsa::kNonBlock);
        if (err < 0 || !pcm) {
            lastError_ = err < 0 ? err : -ENODEV;
            return false;
        }
        for (int format : {alsa::kFormatFloat, alsa::kFormatS32, alsa::kFormatS16}) {
            err = api_.setParams(pcm, format, alsa::kAccessRwInterleaved, 2, kRate, 1, kLatencyUs);
            if (err >= 0) {
                format_ = format;
                break;
            }
        }
        if (err < 0) {
            api_.close(pcm);
            lastError_ = err;
            return false;
        }
        unsigned long buffer = 0, period = 0;
        if (api_.getParams(pcm, &buffer, &period) < 0 || buffer == 0) buffer = kRate / 50;
        if (period == 0 || period > buffer) period = buffer / 4 + 1;
        pcm_ = pcm;
        bufferFrames_ = int(buffer);
        periodFrames_ = int(period);
        scratch_.assign(size_t(bufferFrames_) * 2, 0.0f);
        bytes_.assign(size_t(bufferFrames_) * 2 * sampleBytes(), 0);
        status.sampleRate = kRate;
        status.bufferFrames = bufferFrames_;
        status.deviceOpen = true;
        LOGI("audio: ALSA '%s' %d Hz, 2 ch, %s, buffer %d frames (%.1f ms), period %d", device_.c_str(), kRate,
             format_ == alsa::kFormatFloat ? "float32" : format_ == alsa::kFormatS32 ? "int32" : "int16", bufferFrames_,
             1000.0 * bufferFrames_ / kRate, periodFrames_);
        return true;
    }

    const char* lastError() const { return api_.strerror ? api_.strerror(lastError_) : "error"; }

    bool start(RenderFn fn, void* user) override {
        if (thread_.joinable()) return status.deviceOpen.load();
        fn_ = fn;
        user_ = user;
        {
            std::lock_guard<std::mutex> lk(m_);
            quit_ = false;
        }
        const bool open = pcm_ != nullptr;
        thread_ = std::thread([this] { run(); });
        return open;
    }

    void stop() override {
        {
            std::lock_guard<std::mutex> lk(m_);
            quit_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    static constexpr int kRate = 48000;
    static constexpr unsigned kLatencyUs = 20000;
    static constexpr int kWaitMs = 200;       // snd_pcm_wait timeout
    static constexpr int kStallWaits = 10;    // 2 s without room: the device is stuck

    bool quitting() {
        std::lock_guard<std::mutex> lk(m_);
        return quit_;
    }
    // Sleeps up to 'ms', woken at once by stop().
    void pause(int ms) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::milliseconds(ms), [this] { return quit_; });
    }

    void closeDevice() {
        if (!pcm_) return;
        api_.drop(pcm_);   // not drain: that would play out the whole buffer first
        api_.close(pcm_);
        pcm_ = nullptr;
        status.deviceOpen = false;
    }

    void run() {
        dsp::DenormalGuard guard;
        ReopenBackoff backoff;
        bool logged = false;   // the current failure to open is already in the log
        while (!quitting()) {
            const bool opened = pcm_ || openDevice();
            StreamEnd end = StreamEnd::Lost;
            bool healthy = false;
            if (opened) {
                logged = false;
                end = stream(healthy);
                closeDevice();
                if (end == StreamEnd::Quit) break;
                status.restarts.fetch_add(1);
                LOGW("audio: ALSA '%s' %s, reopening", device_.c_str(),
                     end == StreamEnd::Stalled ? "stalled" : "lost");
            } else if (!logged) {
                LOGW("audio: ALSA '%s' cannot be opened (%s)", device_.c_str(), lastError());
                logged = true;
            }
            const int ms = backoff.waitMs(opened, end, healthy);
            if (ms > 0) pause(ms);
        }
        closeDevice();
    }

    // Recovers from a failed call: an xrun (counted once the stream has settled) or a suspend.
    // false: the device is gone.
    bool recover(int err, unsigned long long written) {
        if (err == -EPIPE && written > unsigned(kRate / 2)) status.underruns.fetch_add(1);
        const int r = api_.recover(pcm_, err, 1);
        if (r < 0) lastError_ = r;
        return r >= 0;
    }

    StreamEnd stream(bool& healthy) {
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        const size_t frameBytes = 2 * sampleBytes();
        unsigned long long written = 0;
        int timeouts = 0;
        size_t pending = 0, offset = 0;   // rendered frames not written yet (never dropped)
        while (!quitting()) {
            if (pending == 0) {
                const int w = api_.wait(pcm_, kWaitMs);
                if (w < 0) {
                    if (!recover(w, written)) return StreamEnd::Lost;
                    continue;
                }
                if (w == 0) {
                    if (++timeouts >= kStallWaits) return StreamEnd::Stalled;
                    continue;
                }
                timeouts = 0;
                const long avail = api_.availUpdate(pcm_);
                if (avail < 0) {
                    if (!recover(int(avail), written)) return StreamEnd::Lost;
                    continue;
                }
                if (avail == 0) continue;
                const double elapsed = std::chrono::duration<double>(clock::now() - t0).count();
                if (aheadOfClock(written, elapsed, kRate, bufferFrames_)) {
                    pause(std::max(1, 1000 * periodFrames_ / kRate));
                    continue;
                }
                const int n = int(std::min<long>(avail, bufferFrames_));
                fn_(user_, scratch_.data(), n, kRate);
                convert(scratch_.data(), bytes_.data(), n);
                pending = size_t(n);
                offset = 0;
            }
            const long r = api_.writei(pcm_, bytes_.data() + offset * frameBytes, pending);
            if (r == -EAGAIN) {
                api_.wait(pcm_, kWaitMs);
                continue;
            }
            if (r < 0) {
                if (!recover(int(r), written)) return StreamEnd::Lost;
                continue;   // the pending frames are written after the recovery
            }
            pending -= size_t(r);
            offset += size_t(r);
            written += (unsigned long long)r;
            healthy = written >= (unsigned long long)kRate;
        }
        return StreamEnd::Quit;
    }

    size_t sampleBytes() const { return format_ == alsa::kFormatS16 ? 2 : 4; }

    // Float stereo to the device format (little-endian, as both Linux targets): NaN is silence,
    // the rest is clamped to [-1, 1].
    void convert(const float* in, uint8_t* out, int frames) const {
        for (int i = 0; i < 2 * frames; ++i) {
            float v = in[i];
            if (v != v) v = 0.0f;
            v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
            if (format_ == alsa::kFormatFloat) {
                std::memcpy(out + size_t(i) * 4, &v, 4);
            } else if (format_ == alsa::kFormatS32) {
                const int32_t s = int32_t(std::llrint(double(v) * 2147483647.0));
                std::memcpy(out + size_t(i) * 4, &s, 4);
            } else {
                const int16_t s = int16_t(std::lrint(v * 32767.0f));
                std::memcpy(out + size_t(i) * 2, &s, 2);
            }
        }
    }

    const AlsaApi& api_;
    const std::string device_;
    snd_pcm* pcm_ = nullptr;
    int format_ = alsa::kFormatFloat;
    int bufferFrames_ = 960, periodFrames_ = 240;
    int lastError_ = 0;
    std::vector<float> scratch_;
    std::vector<uint8_t> bytes_;
    RenderFn fn_ = nullptr;
    void* user_ = nullptr;
    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    bool quit_ = false;
};

template <typename F>
bool symbol(void* lib, F& fn, const char* name) {
    fn = reinterpret_cast<F>(dlsym(lib, name));
    return fn != nullptr;
}

}  // namespace

const AlsaApi* alsaApi() {
    static const AlsaApi* const api = []() -> const AlsaApi* {
        // Never closed: the PulseAudio / PipeWire plugins it loads keep threads and exit handlers.
        void* lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return nullptr;
        static AlsaApi a;
        const bool ok = symbol(lib, a.open, "snd_pcm_open") && symbol(lib, a.close, "snd_pcm_close") &&
                        symbol(lib, a.setParams, "snd_pcm_set_params") && symbol(lib, a.getParams, "snd_pcm_get_params") &&
                        symbol(lib, a.writei, "snd_pcm_writei") && symbol(lib, a.recover, "snd_pcm_recover") &&
                        symbol(lib, a.wait, "snd_pcm_wait") && symbol(lib, a.availUpdate, "snd_pcm_avail_update") &&
                        symbol(lib, a.drop, "snd_pcm_drop") && symbol(lib, a.strerror, "snd_strerror");
        if (!ok) {
            LOGW("audio: libasound.so.2 lacks a PCM function");
            return nullptr;
        }
        using ErrorHandler = void (*)(const char*, int, const char*, int, const char*, ...);
        int (*setHandler)(ErrorHandler) = nullptr;
        if (symbol(lib, setHandler, "snd_lib_error_set_handler")) setHandler(&quietAlsa);
        return &a;
    }();
    return api;
}

std::unique_ptr<Backend> createAlsaBackend(const AlsaApi& api, const char* device) {
    std::unique_ptr<AlsaBackend> b(new AlsaBackend(api, device ? device : "default"));
    if (!b->openDevice()) {
        LOGI("audio: ALSA '%s' not available (%s)", device ? device : "default", b->lastError());
        return nullptr;
    }
    return b;
}

std::unique_ptr<Backend> createBackend() {
    if (chooseBackend(std::getenv("SCACELITH_AUDIO"), std::getenv("SCACELITH_AUDIO_DUMP")) == BackendChoice::Device) {
        if (const AlsaApi* api = alsaApi()) {
            const char* device = std::getenv("SCACELITH_ALSA_DEVICE");
            if (std::unique_ptr<Backend> b = createAlsaBackend(*api, device && *device ? device : "default")) return b;
        } else {
            LOGI("audio: libasound.so.2 not found");
        }
        LOGI("audio: no sound output, running silent");
    }
    return createNullBackend();
}

}  // namespace audio
#endif
