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

}  // namespace audio
