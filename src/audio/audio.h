// Audio: software mixer with procedurally synthesised sounds (no sample files), hall reverb and
// 3D panning. Backends (backend.h): WASAPI shared mode (Windows), ALSA (Linux, libasound loaded at
// run time), null (no device, or SCACELITH_AUDIO=null: real-time pace, output discarded, or
// streamed to a WAV file when SCACELITH_AUDIO_DUMP=<path.wav> is set).
// Implemented by the audio work package. Thread-safe API (the mixer runs on its own thread):
// play/playUI/setListener/volumes may be called from any thread at any time. play/playUI are no-ops
// before init(); setListener and the volume/ambience setters (setVoiceVolume too) are stored and
// apply from the first block. init() and shutdown() must not race with the other calls.
//
// Signal flow (mixer thread, 48 kHz or the device rate, float stereo):
//   voices (32, 3D: inverse distance with 0.15 m min distance, equal-power pan, ITD, head
//   shadow, behind/air low-pass) + speech (2, same 3D chain, talker directivity) + ambience
//   (ducked under speech) -> hall (early reflections + 16-line FDN,
//   RT60 ~2.3 s, 24 ms pre-delay) -> master volume -> DC blocker -> look-ahead limiter (threshold
//   -1.4 dBFS, soft ceiling below -1.0 dBFS).
// Every one-shot is synthesised (modal/noise models) by a low-priority builder thread into a
// small pool of variants per Sfx; each trigger additionally randomises pitch (+-3 %), level and
// tone, and the variant just played is re-synthesised with a new seed, so no two plays match.
#pragma once
#include "../math/math.h"
#include <cstdint>
#include <vector>

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
                     // window of the stroke's length (playPenStroke)
    PenTap,          // ballpoint tip touching the paper (tiny tick through the pad)
    PageTurn,        // page pinched at its corner, lifted and swung over the top edge (~1 s)
    PageFlap,        // the turned page landing face down on the stack
    Count
};

// Short stable identifier ("piece_place", ...), used for file names and logs.
const char* sfxName(Sfx s);

// Starts the device + mixer thread. Returns false (and stays silent) when no output device could
// be opened; the API stays safe to call either way. On Windows the backend keeps retrying in the
// background (device unplugged / default device changed / no device at start-up); on Linux the
// ALSA backend reopens a device it lost, and without any device the null backend runs (silent).
bool init();
void shutdown();

// Listener = the player's head (camera). forward/up unit vectors.
void setListener(m::vec3 position, m::vec3 forward, m::vec3 up);
// Additive: the listener's position as last set (White's seat before the first setListener()).
// Works without init(), like setListener().
m::vec3 listenerPosition();
// Plays a one-shot at a world position. gain in [0,1+], pitch multiplier (small random
// variation is added internally so repeated sounds never sound identical). Suggested use of
// pitch for pieces: heavier pieces slightly lower (king ~0.94, pawn ~1.05).
void play(Sfx s, m::vec3 position, float gain = 1.0f, float pitch = 1.0f);
void playUI(Sfx s, float gain = 1.0f);
// One pen-down stroke at the pen tip: the touch-down tick and 'seconds' of ballpoint friction.
// Call it on anim::Animator's PenDown event with game::sheet::penStrokeSound()'s position,
// length and gain (the writer's own pen is heard from his posture, not at the tip).
void playPenStroke(m::vec3 tip, float seconds, float gain = 1.0f);

// Atmospheric hall ambience (air, distant birds and wind through the windows, faint room tone),
// no music. Fades smoothly when toggled.
void setAmbienceEnabled(bool on);
void setMasterVolume(float v);
void setEffectsVolume(float v);
void setAmbienceVolume(float v);

// ---- Speech (coach voice) ----------------------------------------------------------------------
// Caller-supplied mono PCM (any source rate, e.g. TTS output at 44.1 kHz) played at a world
// position through the same 3D stage and hall as the effects, on its own "voice" bus (own volume,
// never stolen by effects), ducking the ambience (not the effects) while it sounds. Streamable:
// open a voice, append phrases as they are synthesised, close it. Playback waits silently when it
// runs out of audio before the close ("Starved", the normal state between phrases) and ends
// ("Finished") once everything appended has played after the close. Chunk joins are seamless (the
// resampler reads across them). Poll-based like the rest of the API (no callbacks): read
// voiceStatus() once per frame. Same threading contract as play(): any thread, no-ops before
// init(), must not race init()/shutdown(). Two voices can exist at once (one talking, one fading
// out after stopVoice()).
struct VoiceId {
    uint32_t v = 0;  // 0 = invalid (no engine, no free slot, bad parameters)
    explicit operator bool() const { return v != 0; }
    bool operator==(const VoiceId& o) const { return v == o.v; }
    bool operator!=(const VoiceId& o) const { return v != o.v; }
};
enum class VoiceState : uint8_t {
    None,      // invalid or stale id (the slot was reused): treat as finished
    Pending,   // opened, not yet seen by the audio thread: until its next callback, and for as long
               // as no device runs one (Windows without an output device): the game needs a watchdog
    Playing,
    Starved,   // played everything appended so far, not closed: silent, waiting for more
    Paused,
    Finished,  // closed and fully played (terminal)
    Stopped,   // stopVoice() or shutdown() (terminal)
    Dropped    // never started: a device came back and found the open older than 1.5 s (terminal)
};
struct VoiceParams {
    m::vec3 position{0.0f, 0.0f, 0.0f};  // world position (the coach's mouth)
    m::vec3 facing{0.0f, 0.0f, 0.0f};    // talker's forward (head +Z); zero = omnidirectional
    int sampleRate = 44100;              // source rate of the appended PCM (8000..192000)
    float gain = 1.0f;                   // on top of the voice bus volume (0..4)
    float roomSend = 0.13f;              // hall send relative to the direct gain (0..1)
    bool spatial = true;                 // false: centred (narrator), still reverberated
    float duckDb = -6.0f;                // ambience ducking while this voice sounds (0 = none, >= -40)
};
struct VoiceStatus {
    VoiceState state = VoiceState::None;
    // Seconds of source audio consumed: the speech clock (monotonic, updated once per audio callback;
    // what is heard lags it by the output latency, Stats::bufferFrames / sampleRate). While Starved
    // it equals 'queued'.
    double played = 0.0;
    double queued = 0.0;  // seconds appended so far
    bool closed = false;
};
// Opens a voice with no audio yet (Pending, then Starved). Invalid id when the engine is not
// running, the parameters are not finite / out of range, or both slots hold a voice that was not
// asked to stop (stop the current voice first: a voice being stopped gives its slot up at once).
VoiceId openVoice(const VoiceParams& p);
// Moves 'mono' (at the voice's sampleRate) in and returns the chunk's start on the speech clock
// (source seconds), or -1 when refused: invalid/stale/terminal id, voice closed or stopped, engine
// not running, or FIFO full (64 chunks in flight, or the command queue: retry next frame). On
// refusal 'mono' keeps its content (except that its non-finite samples may already have been
// replaced with silence). An empty chunk is not queued (returns the current end).
// Non-finite samples (NaN, Inf) are replaced with silence.
// Give phrase edges a few ms of fade or silence (the TTS does); the voice adds 4 ms edge fades when
// it starts and when it resumes after starving. Use the returned start to schedule gestures and
// subtitles on phrase boundaries.
double appendVoice(VoiceId id, std::vector<float>&& mono);
void closeVoice(VoiceId id);
// open + append + close in one call (whole utterance already synthesised). Invalid id on failure.
VoiceId playVoice(std::vector<float>&& mono, const VoiceParams& p);
// Click-free fade of 'fadeSeconds' (0 = immediate), then Stopped. Queued audio is discarded.
void stopVoice(VoiceId id, float fadeSeconds = 0.06f);
// 15 ms fades; keeps the position (played stops advancing once faded out).
void setVoicePaused(VoiceId id, bool paused);
// Moves the emitter (and its facing, for the talker directivity). Cheap: call it every frame, next to
// setListener(). Does not go through the command queue.
void setVoicePose(VoiceId id, m::vec3 position, m::vec3 facing = m::vec3(0.0f));
VoiceStatus voiceStatus(VoiceId id);
// "Coach voice" volume, [0, 2] (1 = designed level). Survives init()/shutdown() like the others.
void setVoiceVolume(float v);

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
    int activeSpeech = 0;       // speech voices holding a voice (open, not yet Finished/Stopped)
    float cpuLoad = 0.0f;       // mixer time / real time, one core (0.01 = 1 %)
    unsigned underruns = 0;     // device glitches detected (WASAPI, ALSA)
    unsigned deviceRestarts = 0;
};
Stats stats();

}  // namespace audio
