// Output device backends. The backend owns the audio thread and calls the render function with
// interleaved float stereo at the device rate; it converts to the device format itself.
//   * Windows: WASAPI shared mode, event driven (backend_wasapi.cpp)
//   * Linux: ALSA, libasound.so.2 loaded at run time (also PulseAudio and PipeWire through their
//     ALSA plugins), the null backend when it is missing or no device opens (backend_alsa.cpp)
//   * elsewhere, and on request: the null backend, consuming at real-time pace, optionally
//     dumping to a WAV file (backend_null.cpp)
// Environment: SCACELITH_AUDIO=null forces the null backend (the unit tests and the screenshot
// tool set it), SCACELITH_AUDIO_DUMP=<file.wav> too (it records what the null backend plays),
// SCACELITH_ALSA_DEVICE=<pcm> names the ALSA device ("default" otherwise).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>

namespace audio {

using RenderFn = void (*)(void* user, float* stereo, int frames, int sampleRate);

// Device rates the mixer runs at (WASAPI: a mix format outside them is converted by the engine).
constexpr int kMinDeviceRate = 8000, kMaxDeviceRate = 384000;
// Rate of the float32 stereo format handed to the engine when the mix format is not usable: the
// device rate when the mixer can run at it, else 48 kHz (the engine resamples).
inline int fallbackDeviceRate(unsigned long rate) {
    return rate >= unsigned(kMinDeviceRate) && rate <= unsigned(kMaxDeviceRate) ? int(rate) : 48000;
}

// How a device stream ended.
enum class StreamEnd { Quit, Changed, Lost, Stalled };
// Device reopen policy (WASAPI, ALSA). waitMs() gives the wait in ms before the next open
// attempt (0 = at once) after a failed open (opened = false) or a stream that ended with 'end';
// 'healthy': the stream delivered at least a second of audio. The back-off (250 ms, doubling up to
// 5 s) restarts after a healthy stream, a default-device change, or an open that follows failed
// opens (the device came back). A default-device change, or the loss of a healthy device, reopens
// at once; a device that keeps failing right after opening backs off like a failed open (no tight
// reopen loop).
struct ReopenBackoff {
    int failures = 0;         // consecutive attempts that backed off
    bool lastOpened = false;  // the previous attempt opened the device
    int waitMs(bool opened, StreamEnd end, bool healthy) {
        if (opened && (!lastOpened || healthy || end == StreamEnd::Changed)) failures = 0;
        lastOpened = opened;
        if (opened && (end == StreamEnd::Changed || (end == StreamEnd::Lost && healthy))) return 0;
        const int ms = std::min(5000, 250 << std::min(failures, 5));
        ++failures;
        return ms;
    }
};

// WASAPI underrun detection, one per stream: an audio event that finds the device buffer empty is
// an underrun (counted in BackendStatus::underruns; the latency target grows), except in the first
// kGraceEvents events after Start, while the stream settles.
struct UnderrunDetector {
    static constexpr unsigned kGraceEvents = 4;
    unsigned events = 0;  // audio events seen, up to kGraceEvents
    bool onEvent(unsigned padding) {
        if (events < kGraceEvents) {
            ++events;
            return false;
        }
        return padding == 0;
    }
};

struct BackendStatus {
    std::atomic<bool> deviceOpen{false};
    std::atomic<int> sampleRate{0};
    std::atomic<int> bufferFrames{0};
    std::atomic<unsigned> underruns{0};
    std::atomic<unsigned> restarts{0};
};

class Backend {
public:
    virtual ~Backend() = default;
    // Starts the audio thread. Returns true when a device was opened by the first attempt
    // (waits for it, bounded); on false the thread may keep retrying in the background.
    virtual bool start(RenderFn fn, void* user) = 0;
    virtual void stop() = 0;  // joins the thread; the render function is never called afterwards
    BackendStatus status;
};

// Which backend createBackend() makes, from SCACELITH_AUDIO and SCACELITH_AUDIO_DUMP (either may
// be null): a dump, or SCACELITH_AUDIO=null, asks for the null backend; otherwise the platform's
// device backend, falling back to the null one.
enum class BackendChoice { Device, Null };
inline BackendChoice chooseBackend(const char* audioEnv, const char* dumpEnv) {
    if (dumpEnv && *dumpEnv) return BackendChoice::Null;
    if (audioEnv && std::strcmp(audioEnv, "null") == 0) return BackendChoice::Null;
    return BackendChoice::Device;
}

// A device that takes data faster than it plays it (ALSA's "null" PCM, a broken plugin) would make
// the mixer run ahead of real time: true when 'written' frames exceed what 'elapsed' seconds of
// playing at 'rate' could have consumed, with 1 % and 100 ms of margin plus the whole buffer (far
// above any real clock drift).
inline bool aheadOfClock(unsigned long long written, double elapsed, int rate, int bufferFrames) {
    return double(written) > (elapsed * 1.01 + 0.1) * double(rate) + double(bufferFrames);
}

std::unique_ptr<Backend> createBackend();
#ifndef _WIN32
std::unique_ptr<Backend> createNullBackend();
#endif

}  // namespace audio
