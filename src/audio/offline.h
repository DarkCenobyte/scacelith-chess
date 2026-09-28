// Internal helpers for tests/tools: offline rendering through the full mixer chain (no device,
// no threads, deterministic per seed), WAV writing and live-engine probes.
#pragma once
#include "audio.h"
#include "spatial.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace audio {

// Listener used by the offline renders: White's seat, looking at the board centre.
ListenerPose whiteSeatListener();
// Typical world position of each sound for the offline renders (board square, clock, chair...).
m::vec3 defaultPosition(Sfx s);

struct OfflineStats {
    float limiterMinGain = 1.0f;  // 1 = the limiter never engaged
};

// Interleaved stereo, 48 kHz, through voices + hall + master chain; faded out at the end.
std::vector<float> renderSfxOffline(Sfx s, float seconds, uint32_t seed = 1u, OfflineStats* stats = nullptr);
// Same, at an explicit position/listener (tests of the 3D stage). ui = non-spatial UI bus.
std::vector<float> renderSfxOfflineAt(Sfx s, float seconds, m::vec3 pos, const ListenerPose& lis, uint32_t seed,
                                      bool withRoom = true);
std::vector<float> renderAmbienceOffline(float seconds, uint32_t seed = 1u, OfflineStats* stats = nullptr);

// 16-bit PCM WAV with TPDF dither.
bool writeWav16(const char* path, const float* interleaved, size_t frames, int channels, int sampleRate);

// Live engine probes: peak of the mixer output since the previous call (0 when not running);
// number of bank variants re-synthesised after being played (fresh seeds) since start-up.
float debugTakeOutputPeak();
unsigned debugBankRefreshCount();

}  // namespace audio
