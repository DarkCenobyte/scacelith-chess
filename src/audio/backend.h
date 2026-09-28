// Output device backends. The backend owns the audio thread and calls the render function with
// interleaved float stereo at the device rate; it converts to the device format itself.
//   * Windows: WASAPI shared mode, event driven (backend_wasapi.cpp)
//   * elsewhere: null backend consuming at real-time pace (backend_null.cpp)
#pragma once
#include <atomic>
#include <memory>

namespace audio {

using RenderFn = void (*)(void* user, float* stereo, int frames, int sampleRate);

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
