// Listener-relative spatialisation parameters: inverse-distance gain (min distance), equal-power
// pan, interaural time difference (Woodworth), far-ear head shadow and a gentle low-pass for
// sources behind the listener / far away (air absorption).
#pragma once
#include "../math/math.h"
#include "dsp.h"

namespace audio {

struct ListenerPose {
    // Default: White's seat, looking at the board centre (see game/layout.h).
    m::vec3 pos{0.0f, 1.23f, 0.62f};
    m::vec3 fwd{0.0f, -0.598f, -0.801f};
    m::vec3 up{0.0f, 1.0f, 0.0f};
};

struct Basis {
    m::vec3 pos, right, up, fwd;
};

inline Basis makeBasis(const ListenerPose& p) {
    Basis b;
    b.pos = p.pos;
    b.fwd = m::normalize(p.fwd);
    if (!(m::length2(b.fwd) > 0.5f)) b.fwd = m::vec3(0, 0, -1);
    m::vec3 u = p.up - b.fwd * m::dot(p.up, b.fwd);
    if (m::length2(u) < 1e-8f) u = m::orthogonal(b.fwd);
    b.up = m::normalize(u);
    b.right = m::cross(b.fwd, b.up);
    return b;
}

struct SpatialParams {
    float gL = 0.70710678f, gR = 0.70710678f;  // equal-power pan gains (include distance gain)
    float itd = 0.0f;       // samples; > 0 delays the left ear, < 0 the right ear
    float shadowL = 0.0f;   // one-pole coefficients of the far-ear head shadow (0 = open)
    float shadowR = 0.0f;
    float lp = 0.0f;        // one-pole coefficient: behind the head / air absorption
    float distGain = 1.0f;
    float dist = 0.0f;
};

constexpr float kRefDistance = 0.5f;   // gain 1 at 0.5 m (typical table distance)
constexpr float kMinDistance = 0.15f;  // no further boost closer than this

inline SpatialParams computeSpatial(const Basis& b, m::vec3 src, float fs, bool itdOn = true) {
    SpatialParams p;
    m::vec3 d = src - b.pos;
    float dist = m::length(d);
    float lateral = 0.0f, front = 1.0f;
    if (dist > 1e-4f) {
        lateral = dsp::clampf(m::dot(d, b.right) / dist, -1.0f, 1.0f);
        front = m::dot(d, b.fwd) / dist;
    }
    p.dist = dist;
    p.distGain = kRefDistance / std::max(dist, kMinDistance);
    // Near-field: interaural level difference grows when the source is very close to the head.
    float panScale = 0.72f + 0.22f * dsp::clampf((0.6f - dist) / 0.45f, 0.0f, 1.0f);
    float a = (lateral * panScale * 0.5f + 0.5f) * 0.5f * dsp::kPi;
    p.gL = std::cos(a) * p.distGain;
    p.gR = std::sin(a) * p.distGain;
    if (itdOn) {
        float th = std::asin(std::fabs(lateral));
        float sec = (0.0875f / 343.0f) * (th + std::sin(th));
        p.itd = (lateral >= 0.0f ? 1.0f : -1.0f) * sec * fs;
    }
    // Head shadow on the far ear: down to ~3.5 kHz at 90 degrees.
    float shMax = dsp::OnePole::coefFor(3500.0f, fs);
    p.shadowL = shMax * dsp::clampf(lateral, 0.0f, 1.0f);
    p.shadowR = shMax * dsp::clampf(-lateral, 0.0f, 1.0f);
    // Behind the listener (gentle, ~5 kHz fully behind) and air absorption beyond a few metres.
    float behind = dsp::clampf(-front, 0.0f, 1.0f);
    float air = dsp::clampf((dist - 2.0f) / 25.0f, 0.0f, 1.0f);
    float amount = std::max(behind * 0.85f, air * 0.6f);
    p.lp = amount * dsp::OnePole::coefFor(5000.0f, fs);
    return p;
}

// Block targets of a SpatialChain: per-ear gains (distance included), ITD, head shadow, low-pass
// and the hall send level.
struct SpatialTarget {
    float gL = 0.0f, gR = 0.0f, itd = 0.0f, shL = 0.0f, shR = 0.0f, lp = 0.0f, send = 0.0f;
};

// Targets for a source of gain 'g' at 'src' (spatial) or centred (non-spatial, still sent to the
// hall). 'extraLp' is a further low-pass coefficient (talker directivity), merged with the
// behind/air one.
inline SpatialTarget spatialTarget(const Basis& b, bool spatial, m::vec3 src, float fs, float g, float send,
                                   float extraLp = 0.0f) {
    SpatialTarget t;
    if (spatial) {
        SpatialParams sp = computeSpatial(b, src, fs);
        t.gL = sp.gL * g;
        t.gR = sp.gR * g;
        t.itd = sp.itd;
        t.shL = sp.shadowL;
        t.shR = sp.shadowR;
        t.lp = std::max(sp.lp, extraLp);
    } else {
        t.gL = t.gR = 0.70710678f * g;
        t.lp = extraLp;
    }
    t.send = g * send;
    return t;
}

// Per-voice state of the spatial chain shared by the effect voices and the speech voices: the
// block targets are ramped linearly per sample; the hall send is taken pre-filter and pre-pan, then
// behind/air low-pass, ITD (64-sample ring, clamped to 60 samples: the full Woodworth range up to
// ~91 kHz, shortened for far-lateral sources above), far-ear head shadow and the per-ear gains.
struct SpatialChain {
    float gL = 0, gR = 0, itd = 0, shL = 0, shR = 0, lp = 0, sendG = 0;
    float dgL = 0, dgR = 0, dItd = 0, dShL = 0, dShR = 0, dLp = 0, dSend = 0;
    float lpZ = 0, zL = 0, zR = 0;
    float ring[64] = {};
    uint32_t w = 0;
    bool fresh = true;  // the next begin() jumps to its targets instead of ramping

    void reset() { *this = SpatialChain(); }
    // Sets up the ramps of a block of n samples towards 't'.
    void begin(const SpatialTarget& t, int n) {
        if (fresh) {
            gL = t.gL; gR = t.gR; itd = t.itd; shL = t.shL; shR = t.shR; lp = t.lp; sendG = t.send;
            fresh = false;
        }
        const float inv = 1.0f / float(n);
        dgL = (t.gL - gL) * inv; dgR = (t.gR - gR) * inv; dItd = (t.itd - itd) * inv;
        dShL = (t.shL - shL) * inv; dShR = (t.shR - shR) * inv; dLp = (t.lp - lp) * inv;
        dSend = (t.send - sendG) * inv;
    }
    // One source sample: adds the ears into l/r and the hall send into room.
    void tick(float s, float& l, float& r, float& room) {
        gL += dgL; gR += dgR; itd += dItd; shL += dShL; shR += dShR; lp += dLp; sendG += dSend;
        room += s * sendG;
        lpZ = s + lp * (lpZ - s);
        ring[w & 63u] = lpZ;
        ++w;
        float sl = lpZ, sr = lpZ;
        if (itd != 0.0f) {
            float dd = std::min(std::fabs(itd), 60.0f);
            int i0 = int(dd);
            float fr = dd - float(i0);
            float a = ring[(w - 1u - uint32_t(i0)) & 63u], b = ring[(w - 2u - uint32_t(i0)) & 63u];
            float delayed = a + (b - a) * fr;
            if (itd > 0.0f) sl = delayed; else sr = delayed;
        }
        zL = sl + shL * (zL - sl);
        zR = sr + shR * (zR - sr);
        l += zL * gL;
        r += zR * gR;
    }
};

}  // namespace audio
