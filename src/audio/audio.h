// Audio: software mixer with procedurally synthesised sounds (no sample files), hall reverb and
// 3D panning. Backends: WASAPI shared mode (Windows), null/WAV-dump (Linux tests).
// Implemented by the audio work package. Thread-safe API (the mixer runs on its own thread).
#pragma once
#include "../math/math.h"

namespace audio {

enum class Sfx {
    PiecePickup,     // marble piece lifted off marble board (felt slide + light click)
    PiecePlace,      // piece set down on the board (felt-dampened marble "tock")
    Capture,         // captured piece knocked/lifted + placed on the wooden table
    ClockPress,      // lever clock press (mechanical click + switch)
    Handshake,       // two porcelain hands clasping (soft clap + joints)
    ServoShort,      // subtle robotic joint whirr for quick arm moves (very quiet)
    ChairCreak,      // occasional seated shift
    UIHover,
    UIClick,
    GameStart,       // soft low chime when the game begins (not music)
    GameEnd,
    Count
};

bool init();          // starts the device + mixer thread; returns false (and stays silent) on failure
void shutdown();

// Listener = the player's head (camera). forward/up unit vectors.
void setListener(m::vec3 position, m::vec3 forward, m::vec3 up);
// Plays a one-shot at a world position. gain in [0,1+], pitch multiplier (small random
// variation is added internally so repeated sounds never sound identical).
void play(Sfx s, m::vec3 position, float gain = 1.0f, float pitch = 1.0f);
void playUI(Sfx s, float gain = 1.0f);

// Atmospheric hall ambience (air, distant birds and wind through the windows, faint room tone),
// no music. Fades smoothly when toggled.
void setAmbienceEnabled(bool on);
void setMasterVolume(float v);
void setEffectsVolume(float v);
void setAmbienceVolume(float v);

// Tests: renders 'seconds' of a sound into a 48 kHz stereo WAV file (works without a device).
bool renderToWav(Sfx s, const char* path, float seconds = 1.5f);
bool renderAmbienceToWav(const char* path, float seconds);

}  // namespace audio
