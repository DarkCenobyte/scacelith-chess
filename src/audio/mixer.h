// Real-time mixer: 32 voices playing bank variants with per-trigger randomisation, 2 speech voices
// streaming caller-supplied PCM (their own pool: never stolen), 3D spatialisation, four buses
// (effects, UI, ambience, voice), ambience ducking under speech, hall reverb, master volume, DC
// blocker and a look-ahead peak limiter. Single-threaded object: every method is called from the
// thread that renders (the device thread live, the caller's thread offline). No allocation in
// process(), nor in the speech methods (chunks arrive allocated and leave through the retired list).
#pragma once
#include "ambience.h"
#include "audio.h"
#include "reverb.h"
#include "spatial.h"
#include "synth.h"
#include <cstdint>
#include <vector>

namespace audio {

constexpr int kVariants = kBankVariants;
constexpr int kMaxVoices = 32;
constexpr int kMaxBlock = 256;   // internal processing block (frames)

enum class Bus : uint8_t { Effects = 0, UI = 1, Ambience = 2, Voice = 3 };

struct SoundBuffer {
    std::vector<float> samples;  // mono, kBankRate (speech chunk: the voice's source rate)
    int sfx = 0, variant = 0;    // speech chunk: sfx = -1, variant = speech slot
};

// ---- Speech voices ----
constexpr int kMaxSpeech = 2;       // one talking + one fading out (stop, then open the next one)
constexpr int kSpeechChunks = 64;   // FIFO capacity per voice (phrases in flight)
// Designed level of the voice bus at volume 1: a TTS phrase normalised to -20 dBFS RMS, spoken at
// the coach's mouth across the table, is heard at about -26 dBFS RMS from the player's seat, its
// peaks level with a piece placement and ~20 dB above the ambience (tests/audio_tests.cpp).
constexpr float kSpeechLevel = 1.4f;
constexpr float kSpeechEdgeFade = 0.004f;   // s: start, and resume after starving
constexpr float kSpeechPauseFade = 0.015f;  // s: pause / resume
constexpr float kDuckAttack = 0.15f, kDuckHold = 0.5f, kDuckRelease = 0.7f;  // s

struct SpeechParams {
    m::vec3 pos{0.0f, 0.0f, 0.0f};
    m::vec3 facing{0.0f, 0.0f, 0.0f};  // unit talker forward, zero = omnidirectional
    float srcRate = 44100.0f;
    float gain = 1.0f;
    float send = 0.13f;                // hall send relative to the direct gain
    float duckGain = 0.5f;             // ambience gain while this voice sounds
    bool spatial = true;
};
// Sanitised mixer parameters of a public VoiceParams (clamped ranges, dB -> gain, unit facing).
SpeechParams speechParams(const VoiceParams& p);

struct SpeechInfo {
    VoiceState state = VoiceState::None;  // Playing / Starved / Paused while open, then Finished or
                                          // Stopped (never Pending / Dropped: those are the engine's)
    int64_t played = 0;                   // source samples consumed (monotonic speech clock)
    uint32_t chunksDone = 0;              // chunks consumed, refused or discarded (FIFO accounting)
};

struct PlayRequest {
    Sfx sfx = Sfx::UIClick;
    m::vec3 pos{0.0f, 0.0f, 0.0f};
    float gain = 1.0f, pitch = 1.0f;
    Bus bus = Bus::Effects;
    bool spatial = true;
    // > 0: plays only a window of that many seconds of the sound (sustained textures such as the
    // pen friction), starting 'offset' seconds into it (< 0: at a random place), with short fades.
    float duration = 0.0f;
    float offset = -1.0f;
};

// The voices of one pen-down stroke (audio::playPenStroke): the touch-down tick at the tip and, when
// the stroke is long enough to sound, a 'seconds' window of the ballpoint friction. Returns how many.
int penStrokeRequests(m::vec3 tip, float seconds, float gain, PlayRequest out[2]);

class Mixer {
public:
    using RefreshFn = void (*)(void* user, int sfx, int variant);

    explicit Mixer(uint32_t seed = 1234u);
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    void prepare(float sampleRate);  // (re)allocates rate-dependent state; keeps voices and bank
    float sampleRate() const { return fs_; }

    void setListener(const ListenerPose& p);
    void setVolumes(float master, float effects, float ambience);
    void setVoiceVolume(float voice);
    void setAmbienceEnabled(bool on, bool instant = false);
    void setRoomEnabled(bool on) { roomOn_ = on; }  // tests: dry output
    bool play(const PlayRequest& r);
    void process(float* interleavedStereo, int frames);

    // Bank: install() takes ownership. A replaced buffer still in use is swapped when its voices
    // end; replaced buffers are handed back through peekRetired()/dropRetired() so that they can
    // be freed off the audio thread.
    void install(SoundBuffer* b);
    SoundBuffer* peekRetired() const { return retiredCount_ ? retired_[0] : nullptr; }
    void dropRetired();
    bool hasSound(Sfx s) const;
    void setRefreshHook(RefreshFn fn, void* user) { refreshFn_ = fn; refreshUser_ = user; }

    int activeVoices() const;  // effect voices only
    // Lowest limiter gain since the last call (1 = never engaged).
    float takeLimiterMinGain();
    // Hands a buffer back through the retired list (freed off the audio thread by the owner).
    void discard(SoundBuffer* b) { if (b) retire(b); }

    // Speech slots [0, kMaxSpeech). A slot plays a FIFO of mono chunks at SpeechParams::srcRate,
    // resampled (4-point Hermite reading across chunk joins) to the mixer rate, through the same
    // spatial chain as the effect voices (no pitch/level jitter, no tilt) on Bus::Voice. Consumed
    // or discarded chunks go through the retired list, like replaced bank buffers.
    // speechOpen resets the slot (discarding a voice still in it, declicked) and starts Starved.
    bool speechOpen(int slot, const SpeechParams& p);
    // Always takes ownership; false when refused (slot not open, closed, stopping or FIFO full):
    // the chunk is then retired.
    bool speechAppend(int slot, SoundBuffer* chunk);
    void speechClose(int slot);                    // Finished once everything has played
    void speechStop(int slot, float fadeSeconds);  // fade (0 = at once, declicked), then Stopped
    void speechPause(int slot, bool paused);       // kSpeechPauseFade fades, keeps the position
    void speechPose(int slot, m::vec3 pos, m::vec3 facing);
    SpeechInfo speechInfo(int slot) const;
    int activeSpeech() const;                      // open speech voices
    float duckGain() const { return duck_; }       // current ambience ducking factor (tests)

private:
    struct Voice;
    struct Speech;
    struct Slot { SoundBuffer* buf = nullptr; SoundBuffer* pending = nullptr; int users = 0; };
    struct Limiter {
        float thr = 0.85f, hold = 1.0f, g = 1.0f, att = 0.1f, rel = 0.001f, minG = 1.0f;
        int look = 64, holdCnt = 0;
        dsp::DelayLine dl, dr;
        void prepare(float fs);
        void tick(float& l, float& r);
    };

    void block(float* out, int n);
    void renderVoice(Voice& v, int n, float busGain);
    void releaseVoice(Voice& v);
    void renderSpeech(Speech& s, int n);
    float speechSample(Speech& s, double step);
    void advanceChunk(Speech& s);
    void endSpeech(Speech& s, VoiceState st);
    void updateDuck(float blockSec);
    void retire(SoundBuffer* b);
    float busGain(Bus b) const;

    float fs_ = 0.0f;
    dsp::Rng rng_;
    uint32_t seed_;
    Voice* voices_;
    Speech* speech_;
    Slot slots_[int(Sfx::Count)][kVariants];
    int lastVariant_[int(Sfx::Count)];
    SoundBuffer* retired_[512];
    int retiredCount_ = 0;
    RefreshFn refreshFn_ = nullptr;
    void* refreshUser_ = nullptr;

    ListenerPose pose_;
    Basis basis_;
    float masterT_ = 1.0f, fxT_ = 1.0f, ambT_ = 1.0f;
    float master_ = 1.0f, fx_ = 1.0f, amb_ = 1.0f;
    float voiceT_ = 1.0f, voice_ = 1.0f;
    float duck_ = 1.0f, duckHold_ = 0.0f;  // ambience ducking under speech
    bool ambOn_ = true;
    float ambFade_ = 0.0f;
    bool first_ = true;
    bool roomOn_ = true;
    uint32_t clock_ = 0;

    HallReverb reverb_;
    Ambience ambience_;
    Limiter limiter_;
    dsp::DcBlock dcL_, dcR_;
    float L_[kMaxBlock], R_[kMaxBlock], room_[kMaxBlock];
};

}  // namespace audio
