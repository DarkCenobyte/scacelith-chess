// Output device backends. The backend owns the audio thread and calls the render function with
// interleaved float stereo at the device rate; it converts to the device format itself.
//   * Windows: WASAPI shared mode, event driven (backend_wasapi.cpp)
//   * elsewhere: null backend consuming at real-time pace, optionally dumping to a WAV file
//     (SCACELITH_AUDIO_DUMP, backend_null.cpp)
#pragma once
#include <algorithm>
#include <atomic>
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

// How a device stream ended (WASAPI).
enum class StreamEnd { Quit, Changed, Lost, Stalled };
// WASAPI reopen policy. waitMs() gives the wait in ms before the next open attempt (0 = at once)
// after a failed open (opened = false) or a stream that ended with 'end'; 'healthy': the stream
// delivered at least a second of audio. The back-off (250 ms, doubling up to 5 s) restarts after a
// healthy stream, a default-device change, or an open that follows failed opens (the device came
// back). A default-device change, or the loss of a healthy device, reopens at once; a device that
// keeps failing right after opening backs off like a failed open (no tight reopen loop).
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

std::unique_ptr<Backend> createBackend();

}  // namespace audio
