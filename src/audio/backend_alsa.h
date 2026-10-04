// ALSA output (Linux; see backend.h). libasound.so.2 is loaded at run time: the game has no
// build-time or hard run-time dependency on it, and uses the host's library, configuration and
// PulseAudio / PipeWire plugins. The functions go through a table so that the tests can drive the
// backend with a fake device.
#pragma once
#ifdef __linux__
#include "backend.h"

namespace audio {

struct snd_pcm;   // ALSA's opaque snd_pcm_t

// The libasound functions the backend uses (prototypes of alsa/asoundlib.h; the enums as int).
struct AlsaApi {
    int (*open)(snd_pcm** pcm, const char* name, int stream, int mode);
    int (*close)(snd_pcm* pcm);
    int (*setParams)(snd_pcm* pcm, int format, int access, unsigned channels, unsigned rate, int softResample,
                     unsigned latencyUs);
    int (*getParams)(snd_pcm* pcm, unsigned long* bufferSize, unsigned long* periodSize);
    long (*writei)(snd_pcm* pcm, const void* buffer, unsigned long frames);
    int (*recover)(snd_pcm* pcm, int err, int silent);
    int (*wait)(snd_pcm* pcm, int timeoutMs);   // 1 ready, 0 timeout, < 0 error
    long (*availUpdate)(snd_pcm* pcm);
    int (*drop)(snd_pcm* pcm);
    const char* (*strerror)(int err);
};

namespace alsa {
constexpr int kStreamPlayback = 0;
constexpr int kNonBlock = 1;
constexpr int kAccessRwInterleaved = 3;
constexpr int kFormatS16 = 2, kFormatS32 = 10, kFormatFloat = 14;   // little-endian
}  // namespace alsa

// The library's table, loaded once; nullptr when libasound.so.2 (or one of its functions) is missing.
const AlsaApi* alsaApi();

// An ALSA backend on 'device' (e.g. "default"), its device already open: nullptr when the device
// does not open or takes no stereo format (the reason is logged).
std::unique_ptr<Backend> createAlsaBackend(const AlsaApi& api, const char* device);

}  // namespace audio
#endif
