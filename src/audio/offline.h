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
// Same chain at an explicit position/listener (tests of the 3D stage), without the end fade: always
// spatial (UI sounds too, on the UI bus); withRoom = false: dry.
std::vector<float> renderSfxOfflineAt(Sfx s, float seconds, m::vec3 pos, const ListenerPose& lis, uint32_t seed,
                                      bool withRoom = true);
std::vector<float> renderAmbienceOffline(float seconds, uint32_t seed = 1u, OfflineStats* stats = nullptr);

// Mouth of the coach seated at Black's side, head up (the robot has no jaw: one point suffices).
m::vec3 coachMouthDefault();
// Speech 'mono' at 'srcRate' through the full chain (speech voice on the voice bus at volume 1,
// hall, master chain), spoken at 'pos' (talker facing 'facing', zero = omnidirectional) and heard
// from 'lis'; 48 kHz interleaved stereo, deterministic. ambience = ambience on (to hear/measure the
// ducking); withRoom = false: dry.
std::vector<float> renderVoiceOffline(const std::vector<float>& mono, int srcRate, float seconds, m::vec3 pos,
                                      const ListenerPose& lis, bool ambience = false, OfflineStats* stats = nullptr,
                                      m::vec3 facing = m::vec3(0.0f), bool withRoom = true);

// 16-bit PCM WAV with TPDF dither.
bool writeWav16(const char* path, const float* interleaved, size_t frames, int channels, int sampleRate);

// Live engine probes: peak of the mixer output since the previous call (0 when not running);
// number of bank variants re-synthesised after being played (fresh seeds) since start-up.
float debugTakeOutputPeak();
unsigned debugBankRefreshCount();
// Speech chunks allocated by appendVoice() and not yet freed (0 once everything was played or
// discarded and the builder thread ran, and always after shutdown()): leak probe.
int debugSpeechChunksAlive();

}  // namespace audio
