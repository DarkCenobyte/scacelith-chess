#include "mixer.h"
#include <cmath>
#include <cstring>

namespace audio {
using namespace dsp;

struct Mixer::Voice {
    bool active = false;
    bool fresh = true;
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
    // Current (per-sample ramped) parameters.
    float gL = 0, gR = 0, itd = 0, shL = 0, shR = 0, lp = 0, sendG = 0;
    float lpZ = 0, zL = 0, zR = 0;
    float ring[64];
    uint32_t w = 0;
    uint32_t start = 0;
};

Mixer::Mixer(uint32_t seed) : rng_(seed, 99u), seed_(seed) {
    voices_ = new Voice[kMaxVoices];
    for (int& v : lastVariant_) v = -1;
    basis_ = makeBasis(pose_);
}

Mixer::~Mixer() {
    for (auto& row : slots_)
        for (Slot& s : row) {
            delete s.buf;
            delete s.pending;
        }
    for (int i = 0; i < retiredCount_; ++i) delete retired_[i];
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
    for (int i = 0; i < kMaxVoices; ++i) voices_[i].fresh = true;
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

bool Mixer::hasSound(Sfx s) const {
    int i = int(s);
    if (i < 0 || i >= int(Sfx::Count)) return false;
    for (const Slot& sl : slots_[i])
        if (sl.buf) return true;
    return false;
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
        case Bus::Ambience: return amb_ * ambFade_ * ambFade_;
        default: return fx_;
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
    v->fresh = true;
    v->spatial = r.spatial;
    v->bus = r.bus;
    v->sfx = si;
    v->variant = var;
    v->data = slot.buf->samples.data();
    v->length = int(slot.buf->samples.size());
    v->pos = 0.0;
    float j = 0.5f * (rng_.bi() + rng_.bi());  // triangular in [-1, 1]
    v->rate = std::max(0.25f, r.pitch) * (1.0f + info.pitchJitter * j);
    v->gain = info.level * std::max(0.0f, r.gain) * dbToGain(info.levelJitterDb * rng_.bi());
    v->send = info.roomSend;
    v->position = r.pos;
    v->tilt = 0.3f * rng_.bi();
    v->tiltZ = v->lpZ = v->zL = v->zR = 0.0f;
    std::memset(v->ring, 0, sizeof(v->ring));
    v->w = 0;
    v->start = clock_;
    ++slot.users;
    lastVariant_[si] = var;
    if (refreshFn_) refreshFn_(refreshUser_, si, var);  // re-synthesise this slot with a new seed
    return true;
}

void Mixer::renderVoice(Voice& v, int n, float bg) {
    float g = v.gain * bg;
    float tgL, tgR, tItd = 0.0f, tShL = 0.0f, tShR = 0.0f, tLp = 0.0f;
    if (v.spatial) {
        SpatialParams sp = computeSpatial(basis_, v.position, fs_);
        tgL = sp.gL * g;
        tgR = sp.gR * g;
        tItd = sp.itd;
        tShL = sp.shadowL;
        tShR = sp.shadowR;
        tLp = sp.lp;
    } else {
        tgL = tgR = 0.70710678f * g;
    }
    float tSend = g * v.send;
    if (v.fresh) {
        v.gL = tgL; v.gR = tgR; v.itd = tItd; v.shL = tShL; v.shR = tShR; v.lp = tLp; v.sendG = tSend;
        v.fresh = false;
    }
    const float inv = 1.0f / float(n);
    const float dgL = (tgL - v.gL) * inv, dgR = (tgR - v.gR) * inv, dItd = (tItd - v.itd) * inv;
    const float dShL = (tShL - v.shL) * inv, dShR = (tShR - v.shR) * inv, dLp = (tLp - v.lp) * inv;
    const float dSend = (tSend - v.sendG) * inv;
    const double step = double(v.rate) * double(kBankRate) / double(fs_);
    const float tiltA = OnePole::coefFor(2500.0f, fs_);
    const float* d = v.data;
    const int len = v.length;
    for (int i = 0; i < n; ++i) {
        int ip = int(v.pos);
        if (ip >= len) {
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
        v.pos += step;
        v.tiltZ = s + tiltA * (v.tiltZ - s);
        s += v.tilt * (s - v.tiltZ);

        v.gL += dgL; v.gR += dgR; v.itd += dItd; v.shL += dShL; v.shR += dShR; v.lp += dLp; v.sendG += dSend;
        room_[i] += s * v.sendG;

        v.lpZ = s + v.lp * (v.lpZ - s);
        v.ring[v.w & 63u] = v.lpZ;
        ++v.w;
        float sl = v.lpZ, sr = v.lpZ;
        if (v.itd != 0.0f) {
            float dd = std::min(std::fabs(v.itd), 60.0f);
            int i0 = int(dd);
            float fr = dd - float(i0);
            float a = v.ring[(v.w - 1u - uint32_t(i0)) & 63u], b = v.ring[(v.w - 2u - uint32_t(i0)) & 63u];
            float delayed = a + (b - a) * fr;
            if (v.itd > 0.0f) sl = delayed; else sr = delayed;
        }
        v.zL = sl + v.shL * (v.zL - sl);
        v.zR = sr + v.shR * (v.zR - sr);
        L_[i] += v.zL * v.gL;
        R_[i] += v.zR * v.gR;
    }
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
    first_ = false;
    const float fadeStep = blockSec / 1.5f;
    ambFade_ = ambOn_ ? std::min(1.0f, ambFade_ + fadeStep) : std::max(0.0f, ambFade_ - fadeStep);

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
