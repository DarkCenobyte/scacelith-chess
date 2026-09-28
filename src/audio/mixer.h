// Real-time mixer: 32 voices playing bank variants with per-trigger randomisation, 3D
// spatialisation, three buses (effects, UI, ambience), hall reverb, master volume, DC blocker and
// a look-ahead peak limiter. Single-threaded object: every method is called from the thread that
// renders (the device thread live, the caller's thread offline). No allocation in process().
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

enum class Bus : uint8_t { Effects = 0, UI = 1, Ambience = 2 };

struct SoundBuffer {
    std::vector<float> samples;  // mono, kBankRate
    int sfx = 0, variant = 0;
};

struct PlayRequest {
    Sfx sfx = Sfx::UIClick;
    m::vec3 pos{0.0f, 0.0f, 0.0f};
    float gain = 1.0f, pitch = 1.0f;
    Bus bus = Bus::Effects;
    bool spatial = true;
};

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

    int activeVoices() const;
    // Lowest limiter gain since the last call (1 = never engaged).
    float takeLimiterMinGain();

private:
    struct Voice;
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
    void retire(SoundBuffer* b);
    float busGain(Bus b) const;

    float fs_ = 0.0f;
    dsp::Rng rng_;
    uint32_t seed_;
    Voice* voices_;
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
