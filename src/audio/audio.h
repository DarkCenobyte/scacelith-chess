// Audio: software mixer with procedurally synthesised sounds (no sample files), hall reverb and
// 3D panning. Backends: WASAPI shared mode (Windows), null/WAV-dump (Linux tests).
// Implemented by the audio work package. Thread-safe API (the mixer runs on its own thread):
// play/playUI/setListener/volumes may be called from any thread at any time (they are no-ops
// before init()); init() and shutdown() must not race with the other calls.
//
// Signal flow (mixer thread, 48 kHz or the device rate, float stereo):
//   voices (32, 3D: inverse distance with 0.15 m min distance, equal-power pan, ITD, head
//   shadow, behind/air low-pass) + ambience -> hall (early reflections + 16-line FDN,
//   RT60 ~2.3 s, 24 ms pre-delay) -> master volume -> DC blocker -> look-ahead limiter (-1.2 dBFS).
// Every one-shot is synthesised (modal/noise models) by a low-priority builder thread into a
// small pool of variants per Sfx; each trigger additionally randomises pitch (+-3 %), level and
// tone, and the variant just played is re-synthesised with a new seed, so no two plays match.
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
    // Additive: the two halves of Capture, for callers that sync them with the animation.
    CaptureClick,    // marble-on-marble click as the captured piece is taken (bright stone modes)
    TablePlace,      // a (captured) piece set down on the waxed wooden table (woody knock)
    // Additive: scoresheet and pen.
    PenWrite,        // ballpoint rolling on paper over the pad: a sustained texture, played as a
                     // window of the stroke's length (playFor / playPenStroke)
    PenTap,          // ballpoint tip touching the paper (tiny tick through the pad)
    PageTurn,        // page pinched at its corner, lifted and swung over the top edge (~1 s)
    PageFlap,        // the turned page landing face down on the stack
    Count
};

// Short stable identifier ("piece_place", ...), used for file names and logs.
const char* sfxName(Sfx s);

// Starts the device + mixer thread. Returns false (and stays silent) when no output device could
// be opened; the API stays safe to call either way. On Windows the backend keeps retrying in the
// background (device unplugged / default device changed / no device at start-up).
bool init();
void shutdown();

// Listener = the player's head (camera). forward/up unit vectors.
void setListener(m::vec3 position, m::vec3 forward, m::vec3 up);
// Plays a one-shot at a world position. gain in [0,1+], pitch multiplier (small random
// variation is added internally so repeated sounds never sound identical). Suggested use of
// pitch for pieces: heavier pieces slightly lower (king ~0.94, pawn ~1.05).
void play(Sfx s, m::vec3 position, float gain = 1.0f, float pitch = 1.0f);
void playUI(Sfx s, float gain = 1.0f);
// Additive: plays only 'seconds' of a sustained sound (a window at a random place inside it,
// with short fades), e.g. PenWrite for one pen-down stroke.
void playFor(Sfx s, m::vec3 position, float seconds, float gain = 1.0f, float pitch = 1.0f);
// One pen-down stroke at the pen tip: the touch-down tick and 'seconds' of ballpoint friction.
// Call it on anim::Animator's PenDown event with the stroke length from the scoresheet.
void playPenStroke(m::vec3 tip, float seconds, float gain = 1.0f);

// Atmospheric hall ambience (air, distant birds and wind through the windows, faint room tone),
// no music. Fades smoothly when toggled.
void setAmbienceEnabled(bool on);
void setMasterVolume(float v);
void setEffectsVolume(float v);
void setAmbienceVolume(float v);

// Tests: renders 'seconds' of a sound into a 48 kHz stereo WAV file (works without a device).
// The sound is heard from White's seat through the full chain (3D, hall reverb, limiter).
bool renderToWav(Sfx s, const char* path, float seconds = 1.5f);
bool renderAmbienceToWav(const char* path, float seconds);

// Additive: live diagnostics (e.g. for a debug overlay).
struct Stats {
    bool running = false;       // between init() and shutdown() (also when no device could be opened)
    bool deviceOpen = false;    // an output device is currently open
    int sampleRate = 0;         // device (= mixer) rate
    int bufferFrames = 0;       // frames queued ahead of the device (latency ~ bufferFrames / sampleRate)
    int activeVoices = 0;
    float cpuLoad = 0.0f;       // mixer time / real time, one core (0.01 = 1 %)
    unsigned underruns = 0;     // device glitches detected (WASAPI)
    unsigned deviceRestarts = 0;
};
Stats stats();

}  // namespace audio
