#include "mixer.h"
#include <cmath>
#include <cstring>

namespace audio {
using namespace dsp;

struct Mixer::Voice {
    bool active = false;
    bool spatial = true;
    Bus bus = Bus::Effects;
    int sfx = 0, variant = 0;
    const float* data = nullptr;
    int length = 0;
    double pos = 0.0;
    float rate = 1.0f;   // playback ratio relative to kBankRate
    float gain = 1.0f;   // level * user gain * jitter
    float send = 0.0f;   // hall send relative to gain
    m::vec3 position;
    float tilt = 0.0f, tiltZ = 0.0f;
    SpatialChain chain;  // current (per-sample ramped) 3D parameters and filter/ITD state
    uint32_t start = 0;
    // Windowed playback (PlayRequest::duration): source positions of the window, fade lengths.
    double winStart = 0.0, winEnd = 0.0;  // winEnd 0 = whole sound
    float fadeIn = 1.0f, fadeOut = 1.0f;
};

// A streamed speech voice: FIFO of chunks read as one continuous stream.
struct Mixer::Speech {
    bool open = false;             // holds a voice (speechOpen .. Finished / Stopped)
    bool closed = false, paused = false, stopping = false;
    bool starved = true;           // the read position waits for more audio (or the close)
    VoiceState state = VoiceState::None;  // last terminal state once closed
    SpeechParams p;
    SoundBuffer* fifo[kSpeechChunks] = {};
    int head = 0, count = 0;
    int64_t avail = 0;             // source samples in the FIFO, from the head chunk's start
    double pos = 0.0;              // read position in the head chunk (source samples)
    int64_t playedBase = 0;        // source samples of the chunks already consumed
    int64_t played = 0;            // reported speech clock (monotonic)
    uint32_t chunksDone = 0;
    float prev[2] = {0.0f, 0.0f};  // the two stream samples before the head chunk (Hermite continuity)
    float env = 0.0f;              // start / pause / stop / resume fade (linear, smoothstepped)
    float envFade = kSpeechEdgeFade;  // length of the current fade (s)
    float lastY = 0.0f;            // last source output
    float tail = 0.0f;             // declick: an abrupt cut hands its last value over, decaying fast
    int flush = 0;                 // samples still rendered after the voice ended (tail, ITD ring)
    SpatialChain chain;

    // Stream sample k counted from the head chunk's start (k >= -2; silence past the queued audio).
    float at(int64_t k) const {
        if (k < 0) return k >= -2 ? prev[2 + k] : 0.0f;
        for (int i = 0, c = head; i < count; ++i, c = (c + 1) % kSpeechChunks) {
            const int64_t len = int64_t(fifo[c]->samples.size());
            if (k < len) return fifo[c]->samples[size_t(k)];
            k -= len;
        }
        return 0.0f;
    }
    // Source samples played. While starved, everything queued counts as played (the interpolator
    // still holds its last two samples back, waiting for the next chunk).
    int64_t clock() const {
        return starved ? played : playedBase + std::min<int64_t>(int64_t(pos), avail);
    }
};

namespace {
float smooth01(float e) { return e * e * (3.0f - 2.0f * e); }
}  // namespace

SpeechParams speechParams(const VoiceParams& v) {
    SpeechParams p;
    p.pos = v.position;
    p.facing = m::length2(v.facing) > 1e-8f ? m::normalize(v.facing) : m::vec3(0.0f);
    p.srcRate = float(std::min(192000, std::max(8000, v.sampleRate)));
    p.gain = std::isfinite(v.gain) ? clampf(v.gain, 0.0f, 4.0f) : 1.0f;
    p.send = std::isfinite(v.roomSend) ? clampf(v.roomSend, 0.0f, 1.0f) : 0.13f;
    p.duckGain = dbToGain(std::isfinite(v.duckDb) ? clampf(v.duckDb, -40.0f, 0.0f) : 0.0f);
    p.spatial = v.spatial;
    return p;
}

Mixer::Mixer(uint32_t seed) : rng_(seed, 99u), seed_(seed) {
    voices_ = new Voice[kMaxVoices];
    speech_ = new Speech[kMaxSpeech];
    for (int& v : lastVariant_) v = -1;
    basis_ = makeBasis(pose_);
}

Mixer::~Mixer() {
    for (auto& row : slots_)
        for (Slot& s : row) {
            delete s.buf;
            delete s.pending;
        }
    for (int k = 0; k < kMaxSpeech; ++k)
        for (int i = 0; i < speech_[k].count; ++i) delete speech_[k].fifo[(speech_[k].head + i) % kSpeechChunks];
    for (int i = 0; i < retiredCount_; ++i) delete retired_[i];
    delete[] speech_;
    delete[] voices_;
}

void Mixer::Limiter::prepare(float fs) {
    look = std::max(8, int(0.0015f * fs));
    dl.init(look + 4);
    dr.init(look + 4);
    att = 1.0f - std::exp(-5.0f / float(look));
    rel = 1.0f - std::exp(-1.0f / (0.12f * fs));
    hold = g = 1.0f;
    holdCnt = 0;
}

void Mixer::Limiter::tick(float& l, float& r) {
    float p = std::max(std::fabs(l), std::fabs(r));
    float t = p > thr ? thr / p : 1.0f;
    if (t <= hold) {
        hold = t;
        holdCnt = look;
    } else if (holdCnt > 0) {
        --holdCnt;
    } else {
        hold = t;
    }
    g += (hold - g) * (hold < g ? att : rel);
    minG = std::min(minG, g);
    dl.push(l);
    dr.push(r);
    l = dl.tap(look + 1) * g;
    r = dr.tap(look + 1) * g;
    // Safety soft ceiling (never above -1.0 dBFS).
    auto soft = [this](float x) {
        float a = std::fabs(x);
        if (a <= thr) return x;
        float y = thr + 0.04f * std::tanh((a - thr) / 0.04f);
        return x < 0.0f ? -y : y;
    };
    l = soft(l);
    r = soft(r);
}

void Mixer::prepare(float sampleRate) {
    fs_ = sampleRate;
    reverb_.prepare(fs_);
    ambience_.prepare(fs_, seed_ ^ 0x5eed5u);
    limiter_.prepare(fs_);
    dcL_ = DcBlock();
    dcR_ = DcBlock();
    dcL_.set(10.0f, fs_);
    dcR_.set(10.0f, fs_);
    for (int i = 0; i < kMaxVoices; ++i) voices_[i].chain.fresh = true;
    for (int k = 0; k < kMaxSpeech; ++k) speech_[k].chain.fresh = true;
}

void Mixer::setListener(const ListenerPose& p) {
    pose_ = p;
    basis_ = makeBasis(p);
}

void Mixer::setVolumes(float master, float effects, float ambience) {
    masterT_ = clampf(master, 0.0f, 2.0f);
    fxT_ = clampf(effects, 0.0f, 2.0f);
    ambT_ = clampf(ambience, 0.0f, 2.0f);
    if (first_) {
        master_ = masterT_;
        fx_ = fxT_;
        amb_ = ambT_;
    }
}

void Mixer::setVoiceVolume(float voice) {
    voiceT_ = clampf(voice, 0.0f, 2.0f);
    if (first_) voice_ = voiceT_;
}

void Mixer::setAmbienceEnabled(bool on, bool instant) {
    ambOn_ = on;
    if (instant) ambFade_ = on ? 1.0f : 0.0f;
}

void Mixer::install(SoundBuffer* b) {
    if (!b) return;
    if (b->sfx < 0 || b->sfx >= int(Sfx::Count) || b->variant < 0 || b->variant >= kVariants) {
        retire(b);
        return;
    }
    Slot& s = slots_[b->sfx][b->variant];
    if (!s.buf) {
        s.buf = b;
    } else if (s.users == 0 && !s.pending) {
        retire(s.buf);
        s.buf = b;
    } else if (!s.pending) {
        s.pending = b;
    } else {
        retire(s.pending);
        s.pending = b;
    }
}

void Mixer::retire(SoundBuffer* b) {
    if (retiredCount_ < int(sizeof(retired_) / sizeof(retired_[0]))) retired_[retiredCount_++] = b;
    else delete b;  // cannot happen in practice (the owner drains every block)
}

void Mixer::dropRetired() {
    if (!retiredCount_) return;
    --retiredCount_;
    std::memmove(retired_, retired_ + 1, sizeof(retired_[0]) * size_t(retiredCount_));
}

int Mixer::activeVoices() const {
    int n = 0;
    for (int i = 0; i < kMaxVoices; ++i) n += voices_[i].active ? 1 : 0;
    return n;
}

float Mixer::takeLimiterMinGain() {
    float g = limiter_.minG;
    limiter_.minG = 1.0f;
    return g;
}

float Mixer::busGain(Bus b) const {
    switch (b) {
        case Bus::Ambience: return amb_ * ambFade_ * ambFade_ * duck_;  // creaks are ducked too
        case Bus::Voice: return voice_;
        default: return fx_;  // effects and UI (never ducked: the board stays audible under speech)
    }
}

void Mixer::releaseVoice(Voice& v) {
    if (!v.active) return;
    v.active = false;
    Slot& s = slots_[v.sfx][v.variant];
    if (s.users > 0) --s.users;
    if (s.users == 0 && s.pending) {
        retire(s.buf);
        s.buf = s.pending;
        s.pending = nullptr;
    }
}

bool Mixer::play(const PlayRequest& r) {
    int si = int(r.sfx);
    if (si < 0 || si >= int(Sfx::Count) || fs_ <= 0.0f) return false;
    // Choose a ready variant, avoiding the last one played and preferring idle slots.
    int cand[kVariants], nc = 0, any[kVariants], na = 0;
    for (int v = 0; v < kVariants; ++v) {
        const Slot& s = slots_[si][v];
        if (!s.buf) continue;
        any[na++] = v;
        if (v != lastVariant_[si] && s.users == 0 && !s.pending) cand[nc++] = v;
    }
    if (!na) return false;
    int var = nc ? cand[rng_.below(nc)] : any[rng_.below(na)];
    Slot& slot = slots_[si][var];

    Voice* v = nullptr;
    for (int i = 0; i < kMaxVoices; ++i)
        if (!voices_[i].active) { v = &voices_[i]; break; }
    if (!v) {  // steal the oldest voice
        Voice* oldest = &voices_[0];
        for (int i = 1; i < kMaxVoices; ++i)
            if (clock_ - voices_[i].start > clock_ - oldest->start) oldest = &voices_[i];
        releaseVoice(*oldest);
        v = oldest;
    }
    const SfxInfo& info = sfxInfo(r.sfx);
    v->active = true;
    v->spatial = r.spatial;
    v->bus = r.bus;
    v->sfx = si;
    v->variant = var;
    v->data = slot.buf->samples.data();
    v->length = int(slot.buf->samples.size());
    v->pos = 0.0;
    float j = 0.5f * (rng_.bi() + rng_.bi());  // triangular in [-1, 1]
    v->rate = std::max(0.25f, r.pitch) * (1.0f + info.pitchJitter * j);
    v->winStart = v->winEnd = 0.0;
    if (r.duration > 0.0f) {
        // The window is measured in source samples: the voice plays it at 'rate'.
        const double len = double(v->length), win = double(r.duration) * double(v->rate) * double(kBankRate);
        v->fadeIn = 0.004f * v->rate * float(kBankRate);
        v->fadeOut = std::min(0.012f * v->rate * float(kBankRate), float(0.4 * win));
        double room = len - win - 0.002 * kBankRate;
        double start = 0.0;
        if (room > 0.0) start = r.offset >= 0.0f ? std::min(room, double(r.offset) * kBankRate) : double(rng_.uni()) * room;
        v->pos = v->winStart = start;
        v->winEnd = std::min(len, start + win);
    }
    v->gain = info.level * std::max(0.0f, r.gain) * dbToGain(info.levelJitterDb * rng_.bi());
    v->send = info.roomSend;
    v->position = r.pos;
    v->tilt = 0.3f * rng_.bi();
    v->tiltZ = 0.0f;
    v->chain.reset();
    v->start = clock_;
    ++slot.users;
    lastVariant_[si] = var;
    // Re-synthesise this slot with a new seed. Windowed plays (many short strokes per second) take
    // their variety from the random window position instead.
    if (refreshFn_ && r.duration <= 0.0f) refreshFn_(refreshUser_, si, var);
    return true;
}

void Mixer::renderVoice(Voice& v, int n, float bg) {
    // Non-spatial voices (UI bus) are centred and still feed the hall through the send.
    v.chain.begin(spatialTarget(basis_, v.spatial, v.position, fs_, v.gain * bg, v.send), n);
    const double step = double(v.rate) * double(kBankRate) / double(fs_);
    const float tiltA = OnePole::coefFor(2500.0f, fs_);
    const float* d = v.data;
    const int len = v.length;
    const bool windowed = v.winEnd > 0.0;
    for (int i = 0; i < n; ++i) {
        int ip = int(v.pos);
        if (ip >= len || (windowed && v.pos >= v.winEnd)) {
            releaseVoice(v);
            return;
        }
        float f = float(v.pos - double(ip));
        float s;
        if (ip >= 1 && ip + 2 < len) {
            s = hermite(d[ip - 1], d[ip], d[ip + 1], d[ip + 2], f);
        } else {
            auto at = [&](int k) { return (k >= 0 && k < len) ? d[k] : 0.0f; };
            s = hermite(at(ip - 1), at(ip), at(ip + 1), at(ip + 2), f);
        }
        if (windowed) {
            float a = std::min(1.0f, float(v.pos - v.winStart) / v.fadeIn);
            float b = std::min(1.0f, float(v.winEnd - v.pos) / v.fadeOut);
            s *= a * a * (3.0f - 2.0f * a) * b * b * (3.0f - 2.0f * b);
        }
        v.pos += step;
        v.tiltZ = s + tiltA * (v.tiltZ - s);
        s += v.tilt * (s - v.tiltZ);
        v.chain.tick(s, L_[i], R_[i], room_[i]);
    }
}

// ---- Speech -----------------------------------------------------------------------------------

bool Mixer::speechOpen(int slot, const SpeechParams& p) {
    if (slot < 0 || slot >= kMaxSpeech) return false;
    Speech& s = speech_[slot];
    // A voice still sounding in the slot is cut: its last value is handed to the declick tail and
    // the chain keeps its state (no jump in the filters or the ITD line).
    const bool sounding = s.open || s.flush > 0;
    for (int i = 0; i < s.count; ++i) retire(s.fifo[(s.head + i) % kSpeechChunks]);
    const float tail = sounding ? s.tail + s.lastY : 0.0f;
    const SpatialChain chain = s.chain;
    s = Speech();
    if (sounding) s.chain = chain;
    s.tail = tail;
    s.p = p;
    s.open = true;
    s.starved = true;
    s.envFade = kSpeechEdgeFade;
    return true;
}

bool Mixer::speechAppend(int slot, SoundBuffer* chunk) {
    if (!chunk) return false;
    if (slot < 0 || slot >= kMaxSpeech) {
        retire(chunk);
        return false;
    }
    Speech& s = speech_[slot];
    const bool accept = s.open && !s.closed && !s.stopping;
    const bool empty = chunk->samples.empty();
    if (!accept || empty || s.count >= kSpeechChunks) {
        retire(chunk);
        ++s.chunksDone;
        return accept && empty;  // an empty chunk is accepted: nothing to play
    }
    s.fifo[(s.head + s.count) % kSpeechChunks] = chunk;
    ++s.count;
    s.avail += int64_t(chunk->samples.size());
    return true;
}

void Mixer::speechClose(int slot) {
    if (slot >= 0 && slot < kMaxSpeech && speech_[slot].open) speech_[slot].closed = true;
}

void Mixer::speechStop(int slot, float fadeSeconds) {
    if (slot < 0 || slot >= kMaxSpeech) return;
    Speech& s = speech_[slot];
    if (!s.open) return;
    // Nothing audible to fade (starved, paused, not started): at once.
    if (!(fadeSeconds > 0.0f) || s.starved || s.env <= 0.0f) {
        endSpeech(s, VoiceState::Stopped);
        return;
    }
    const float fade = std::min(fadeSeconds, 10.0f);
    s.envFade = s.stopping ? std::min(s.envFade, fade) : fade;
    s.stopping = true;
}

void Mixer::speechPause(int slot, bool paused) {
    if (slot < 0 || slot >= kMaxSpeech) return;
    Speech& s = speech_[slot];
    if (!s.open || s.paused == paused) return;
    s.paused = paused;
    if (!s.stopping) s.envFade = kSpeechPauseFade;
}

void Mixer::speechPose(int slot, m::vec3 pos, m::vec3 facing) {
    if (slot < 0 || slot >= kMaxSpeech) return;
    Speech& s = speech_[slot];
    s.p.pos = pos;
    s.p.facing = m::length2(facing) > 1e-8f ? m::normalize(facing) : m::vec3(0.0f);
}

SpeechInfo Mixer::speechInfo(int slot) const {
    SpeechInfo info;
    if (slot < 0 || slot >= kMaxSpeech) return info;
    const Speech& s = speech_[slot];
    if (!s.open) info.state = s.state;
    else if (s.paused) info.state = VoiceState::Paused;
    else if (s.starved) info.state = VoiceState::Starved;
    else info.state = VoiceState::Playing;
    info.played = s.open ? std::max(s.played, s.clock()) : s.played;
    info.chunksDone = s.chunksDone;
    return info;
}

int Mixer::activeSpeech() const {
    int n = 0;
    for (int k = 0; k < kMaxSpeech; ++k) n += speech_[k].open ? 1 : 0;
    return n;
}

void Mixer::advanceChunk(Speech& s) {
    SoundBuffer* c = s.fifo[s.head];
    const int64_t len = int64_t(c->samples.size());  // >= 1 (empty chunks are never queued)
    if (len >= 2) {
        s.prev[0] = c->samples[size_t(len - 2)];
        s.prev[1] = c->samples[size_t(len - 1)];
    } else {
        s.prev[0] = s.prev[1];
        s.prev[1] = c->samples[0];
    }
    s.pos -= double(len);
    s.playedBase += len;
    s.avail -= len;
    retire(c);
    s.fifo[s.head] = nullptr;
    s.head = (s.head + 1) % kSpeechChunks;
    --s.count;
    ++s.chunksDone;
}

void Mixer::endSpeech(Speech& s, VoiceState st) {
    s.played = std::max(s.played, s.clock());
    for (int i = 0; i < s.count; ++i) {
        retire(s.fifo[(s.head + i) % kSpeechChunks]);
        s.fifo[(s.head + i) % kSpeechChunks] = nullptr;
        ++s.chunksDone;
    }
    s.head = s.count = 0;
    s.avail = 0;
    s.open = false;
    s.state = st;
    s.tail += s.lastY;
    s.lastY = 0.0f;
    s.env = 0.0f;
    s.flush = std::max(96, int(0.012f * fs_));
}

// One source sample of a speech voice (before the spatial chain); advances the voice.
float Mixer::speechSample(Speech& s, double step) {
    while (s.count > 0 && s.pos >= double(s.fifo[s.head]->samples.size())) advanceChunk(s);
    const int64_t ip = int64_t(s.pos);
    // Interpolating at ip needs the stream up to ip + 2: wait for it (starve) unless the voice is
    // closed, in which case the end reads silence. Holding back keeps a late chunk's join seamless.
    const bool ready = s.closed ? s.count > 0 : ip + 2 < s.avail;
    if (!ready) {
        if (s.closed || s.stopping) {
            endSpeech(s, s.stopping ? VoiceState::Stopped : VoiceState::Finished);
            return 0.0f;
        }
        if (!s.starved) {
            s.starved = true;
            s.played = std::max(s.played, s.playedBase + s.avail);
            s.tail += s.lastY;  // non-silent phrase end: decays instead of a step
            s.lastY = 0.0f;
            s.env = 0.0f;       // the next chunk fades in
            if (!s.paused) s.envFade = kSpeechEdgeFade;
        }
        return 0.0f;
    }
    s.starved = false;
    const float target = (s.paused || s.stopping) ? 0.0f : 1.0f;
    const float step1 = 1.0f / (s.envFade * fs_);
    if (s.env < target) s.env = std::min(target, s.env + step1);
    else if (s.env > target) s.env = std::max(target, s.env - step1);
    if (s.env <= 0.0f) {
        if (s.stopping) {
            endSpeech(s, VoiceState::Stopped);
            return 0.0f;
        }
        s.lastY = 0.0f;
        return 0.0f;  // paused: the position is held
    }
    const std::vector<float>& c = s.fifo[s.head]->samples;
    const float f = float(s.pos - double(ip));
    float x;
    if (ip >= 1 && ip + 2 < int64_t(c.size())) {
        const size_t k = size_t(ip);
        x = hermite(c[k - 1], c[k], c[k + 1], c[k + 2], f);
    } else {
        x = hermite(s.at(ip - 1), s.at(ip), s.at(ip + 1), s.at(ip + 2), f);
    }
    s.pos += step;
    const float y = x * smooth01(s.env);
    s.lastY = y;
    return y;
}

void Mixer::renderSpeech(Speech& s, int n) {
    // Talker directivity: speech radiates its highs forward. A coach turned towards the board or
    // aside sounds a little softer and duller from the player's seat; the hall send is unchanged.
    float dirGain = 1.0f, dirLp = 0.0f;
    if (s.p.spatial && m::length2(s.p.facing) > 0.5f) {
        const m::vec3 d = basis_.pos - s.p.pos;
        const float len = m::length(d);
        if (len > 1e-4f) {
            const float w = 0.5f + 0.5f * clampf(m::dot(s.p.facing, d / len), -1.0f, 1.0f);
            dirGain = 0.55f + 0.45f * w;
            dirLp = (1.0f - w) * OnePole::coefFor(3000.0f, fs_);
        }
    }
    const float g = kSpeechLevel * s.p.gain * busGain(Bus::Voice);
    s.chain.begin(spatialTarget(basis_, s.p.spatial, s.p.pos, fs_, g * dirGain, s.p.send / dirGain, dirLp), n);
    const double step = double(s.p.srcRate) / double(fs_);
    const float tailK = std::exp(-1.0f / (0.0012f * fs_));
    for (int i = 0; i < n; ++i) {
        float y = 0.0f;
        if (s.open) y = speechSample(s, step);
        else if (s.flush > 0) --s.flush;
        y += s.tail;
        s.tail *= tailK;
        s.chain.tick(y, L_[i], R_[i], room_[i]);
    }
    if (s.open) s.played = std::max(s.played, s.clock());
    else if (s.flush <= 0) s.tail = 0.0f;
}

// Ambience ducking under speech: attack while a voice sounds, hold through short gaps (between
// phrases, demonstration moves), then release. Only the ambience bus: effects are never ducked.
void Mixer::updateDuck(float blockSec) {
    bool talking = false;
    float target = 1.0f;
    for (int k = 0; k < kMaxSpeech; ++k) {
        const Speech& s = speech_[k];
        if (s.open && !s.starved && !s.paused) {
            talking = true;
            target = std::min(target, s.p.duckGain);
        }
    }
    if (talking) {
        duckHold_ = kDuckHold;
    } else {
        duckHold_ = std::max(0.0f, duckHold_ - blockSec);
        if (duckHold_ > 0.0f) return;
    }
    const float tau = target < duck_ ? kDuckAttack : kDuckRelease;
    duck_ += (target - duck_) * (1.0f - std::exp(-blockSec / tau));
}

void Mixer::block(float* out, int n) {
    // Pending bank swaps whose voices ended.
    for (auto& row : slots_)
        for (Slot& s : row)
            if (s.pending && s.users == 0) {
                retire(s.buf);
                s.buf = s.pending;
                s.pending = nullptr;
            }

    const float blockSec = float(n) / fs_;
    const float kv = first_ ? 1.0f : 1.0f - std::exp(-blockSec / 0.05f);
    master_ += (masterT_ - master_) * kv;
    fx_ += (fxT_ - fx_) * kv;
    amb_ += (ambT_ - amb_) * kv;
    voice_ += (voiceT_ - voice_) * kv;
    first_ = false;
    const float fadeStep = blockSec / 1.5f;
    ambFade_ = ambOn_ ? std::min(1.0f, ambFade_ + fadeStep) : std::max(0.0f, ambFade_ - fadeStep);
    updateDuck(blockSec);

    std::memset(L_, 0, sizeof(float) * size_t(n));
    std::memset(R_, 0, sizeof(float) * size_t(n));
    std::memset(room_, 0, sizeof(float) * size_t(n));

    float ambGain = busGain(Bus::Ambience);
    if (ambGain > 1e-6f) {
        ambience_.process(L_, R_, room_, n, basis_, ambGain);
        Ambience::Event e;
        if (ambience_.popCreak(e)) {
            PlayRequest r;
            r.sfx = Sfx::ChairCreak;
            r.pos = e.pos;
            r.gain = e.gain;
            r.pitch = e.pitch;
            r.bus = Bus::Ambience;
            play(r);
        }
    }
    for (int i = 0; i < kMaxVoices; ++i) {
        Voice& v = voices_[i];
        if (v.active) renderVoice(v, n, busGain(v.bus));
    }
    for (int k = 0; k < kMaxSpeech; ++k) {
        Speech& s = speech_[k];
        if (s.open || s.flush > 0) renderSpeech(s, n);
    }
    if (roomOn_) reverb_.process(room_, L_, R_, n);

    const float mg = master_;
    for (int i = 0; i < n; ++i) {
        float l = dcL_.tick(L_[i] * mg);
        float r = dcR_.tick(R_[i] * mg);
        limiter_.tick(l, r);
        out[2 * i] = l;
        out[2 * i + 1] = r;
    }
    clock_ += uint32_t(n);
}

void Mixer::process(float* out, int frames) {
    DenormalGuard guard;
    if (fs_ <= 0.0f) prepare(48000.0f);
    while (frames > 0) {
        int n = std::min(frames, kMaxBlock);
        block(out, n);
        out += 2 * n;
        frames -= n;
    }
}

}  // namespace audio
