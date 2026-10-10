// Procedural one-shot synthesis. Physical sketch of every model is next to its code; the common
// building blocks are:
//   * Excitation: contact force as half-sine pulses of given duration and impulse. The contact
//     time is the physics that separates "felt thud" (1-2 ms, spectrum rolls off above ~500 Hz)
//     from "marble click" (30-100 us, flat to 10+ kHz).
//   * Modal banks: two-pole resonators driven by the force (board plate, table top, pieces,
//     porcelain shells, plastic housing...). Frequencies from thin-plate formulas where it makes
//     sense, amplitudes from mode shapes at the impact point and radiation efficiency, decay
//     from material Q (tau = Q / (pi f)).
//   * Filtered noise bursts (felt friction, contact cracks, pad compression) with optional
//     stick-slip granulation.
#include "synth.h"
#include "dsp.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>
#include <initializer_list>

namespace audio {
namespace {
using namespace dsp;
constexpr float FS = float(kBankRate);

struct Buf {
    std::vector<float> x;
    explicit Buf(float seconds) : x(size_t(seconds * FS) + 1, 0.0f) {}
    int n() const { return int(x.size()); }
};
inline int idx(float t) { return int(std::lround(t * FS)); }

struct Mode { float f, tau, g; };
using Modes = std::vector<Mode>;

struct Excitation {
    std::vector<float> f;
    int first = INT_MAX, last = -1;
    explicit Excitation(int n) : f(size_t(n), 0.0f) {}
    void add(int i, float v) {
        if (i < 0 || i >= int(f.size())) return;
        f[size_t(i)] += v;
        first = std::min(first, i);
        last = std::max(last, i);
    }
    // Half-sine force pulse starting at t0 (s), duration dur (s), total impulse 'area'. The pulse
    // is integrated over each sample interval so sub-sample contact times stay exact in energy.
    void pulse(float t0, float dur, float area) {
        float len = std::max(dur * FS, 0.25f);
        float start = t0 * FS;
        int i0 = int(std::floor(start)), i1 = int(std::ceil(start + len));
        for (int i = i0; i <= i1; ++i) {
            float a0 = clampf((float(i) - start) / len, 0.0f, 1.0f);
            float a1 = clampf((float(i + 1) - start) / len, 0.0f, 1.0f);
            float w = 0.5f * (std::cos(kPi * a0) - std::cos(kPi * a1));
            if (w != 0.0f) add(i, area * w);
        }
    }
    void mixFrom(const Excitation& o, float g) {
        for (int i = o.first; i <= o.last; ++i) add(i, o.f[size_t(i)] * g);
    }
};

void renderModes(Buf& out, const Excitation& e, const Modes& modes, float gain) {
    if (e.last < 0 || gain == 0.0f) return;
    for (const Mode& m : modes) {
        if (m.f <= 20.0f || m.f >= 0.45f * FS || m.g == 0.0f) continue;
        Resonator r;
        r.set(m.f, m.tau, m.g * gain, FS);
        int end = std::min(out.n(), e.last + int(9.0f * m.tau * FS) + 2);  // e^-9 ~ -78 dB
        int i = e.first;
        for (; i <= e.last && i < end; ++i) out.x[size_t(i)] += r.tick(e.f[size_t(i)]);
        for (; i < end; ++i) out.x[size_t(i)] += r.tick(0.0f);
    }
}

// Mode set with jittered frequencies, Q in [qLo, qHi] and the given relative gains.
Modes jittered(Rng& r, std::initializer_list<float> freqs, std::initializer_list<float> gains, float qLo,
               float qHi, float fJit = 0.04f) {
    Modes ms;
    auto g = gains.begin();
    for (float f0 : freqs) {
        float f = r.jit(f0, fJit);
        float q = r.range(qLo, qHi);
        float gg = (g != gains.end() ? *g++ : 0.3f) * r.range(0.75f, 1.25f);
        ms.push_back({f, q / (kPi * f), gg});
    }
    return ms;
}

// Marble board: 0.50 x 0.50 x 0.022 m slab lying on the table. Thin-plate modes
// f_mn ~ f11 (m^2 + n^2) / 2 with f11 ~ 390 Hz (E 60 GPa, rho 2700), slightly split by the
// felt lining; amplitudes from the mode shapes at the impact point (px, py in 0..1);
// radiation efficiency rising to ~1 above the coincidence frequency (~600 Hz); marble resting
// on felt and wood: the lowest modes are strongly coupled to the table (Q ~ 30), the stone
// rings a little above (Q ~ 55-90).
Modes boardModes(Rng& r, float px, float py) {
    Modes ms;
    float f11 = r.jit(392.0f, 0.03f);
    float asp = r.range(0.96f, 1.04f);
    for (int m = 1; m <= 6; ++m)
        for (int n = 1; n <= 6; ++n) {
            float f = f11 * 0.5f * (float(m * m) * asp + float(n * n) / asp) * r.jit(1.0f, 0.015f);
            if (f > 12000.0f) continue;
            float shape = std::fabs(std::sin(float(m) * kPi * px) * std::sin(float(n) * kPi * py));
            float rad = std::min(1.0f, std::pow(f / 600.0f, 1.5f));
            float q = r.range(0.8f, 1.25f) * (f < 700.0f ? 30.0f : (f < 2000.0f ? 55.0f : 90.0f));
            ms.push_back({f, q / (kPi * f), (0.15f + 0.85f * shape) * rad * r.range(0.7f, 1.3f)});
        }
    return ms;
}

// Waxed oak table top 1.20 x 0.86 x 0.045 m on its frame: f_mn = 88.9((m/1.2)^2 + (n/0.86)^2)
// (E 12 GPa, rho 700), coincidence ~360 Hz, thick top on a frame + wax: Q ~ 10-25.
Modes tableModes(Rng& r, float px, float py) {
    Modes ms;
    for (int m = 1; m <= 7; ++m)
        for (int n = 1; n <= 5; ++n) {
            float a = float(m) / 1.2f, b = float(n) / 0.86f;
            float f = 88.9f * (a * a + b * b) * r.jit(1.0f, 0.04f);
            if (f > 5000.0f) continue;
            float shape = std::fabs(std::sin(float(m) * kPi * px) * std::sin(float(n) * kPi * py));
            float rad = std::min(1.0f, std::pow(f / 360.0f, 1.5f));
            float q = r.range(10.0f, 25.0f) * (f < 400.0f ? 1.2f : 1.0f);
            ms.push_back({f, q / (kPi * f), (0.2f + 0.8f * shape) * rad * r.range(0.7f, 1.3f)});
        }
    return ms;
}

// Small marble Staunton piece (lead-weighted): first bending mode of collar/crown ~2.7-3.7 kHz
// for tournament sizes, marble (held / standing on felt) Q ~ 80-200. size = relative height.
Modes pieceModes(Rng& r, float size) {
    static const float ratio[] = {1.0f, 1.47f, 2.03f, 2.58f, 3.21f, 3.96f, 4.83f, 5.70f};
    Modes ms;
    float f1 = r.range(2700.0f, 3700.0f) / size;
    float g = 1.0f;
    for (float k : ratio) {
        float f = f1 * k * r.jit(1.0f, 0.04f);
        float q = r.range(80.0f, 200.0f);
        ms.push_back({f, q / (kPi * f), g * r.range(0.5f, 1.0f)});
        g *= 0.82f;
    }
    return ms;
}

// Glazed porcelain shell of a robot finger/hand: sparse, high-Q modes (lower Q when the shell is
// held against something, e.g. clasped hands).
Modes porcelainModes(Rng& r, float qLo = 250.0f, float qHi = 480.0f) {
    return jittered(r, {2650.0f, 4190.0f, 5670.0f, 7500.0f, 9570.0f, 11660.0f}, {1.0f, 0.75f, 0.6f, 0.42f, 0.3f, 0.2f},
                    qLo, qHi, 0.06f);
}

struct NoiseSpec {
    float t0, attack, tau, length, amp, hp, lp;
    float grain = 0.0f;      // 0..1 depth of stick-slip granulation
    float grainRate = 0.0f;  // grains per second
};

// Band-limited noise burst: smooth attack, exponential decay, faded end. amp ~ peak level
// independent of the bandwidth.
void noiseBurst(Buf& out, Rng& r, const NoiseSpec& s) {
    Svf hp, lp;
    hp.set(s.hp, 0.707f, FS);
    lp.set(s.lp, 0.707f, FS);
    float bw = std::max(s.lp - s.hp, 100.0f) / (0.5f * FS);
    float norm = s.amp / std::sqrt(bw) * 0.55f;
    FastNoise n(r.next());
    int i0 = idx(s.t0), len = idx(s.length);
    float ge = 0.0f, gdec = std::exp(-1.0f / (0.0005f * FS)), gp = s.grainRate / FS;
    const int na = std::max(1, idx(s.attack));
    const float dec = std::exp(-1.0f / (std::max(s.tau, 1e-5f) * FS));
    float ed = 1.0f;
    for (int k = 0; k < len; ++k) {
        int i = i0 + k;
        if (i >= out.n()) break;
        float env;
        if (k < na) {
            float a = float(k) / float(na);
            env = a * a * (3.0f - 2.0f * a);  // smooth attack
        } else {
            env = ed;
            ed *= dec;
        }
        float tail = float(len - k) / (0.1f * float(len));
        if (tail < 1.0f) env *= tail;
        float v = lp.lp(hp.hp(n.next()));
        if (s.grain > 0.0f) {
            ge *= gdec;
            if (r.uni() < gp) ge += r.range(0.5f, 1.6f);
            v *= (1.0f - s.grain) + s.grain * ge;
        }
        if (i >= 0) out.x[size_t(i)] += v * env * norm;
    }
}

// Felt pad friction when a piece unsticks / slides a few millimetres.
void feltSlide(Buf& out, Rng& r, float t0, float amp) {
    noiseBurst(out, r, {t0, r.range(0.004f, 0.009f), r.range(0.015f, 0.035f), 0.13f, amp, r.range(900.0f, 1400.0f),
                        r.range(3800.0f, 6000.0f), 0.6f, r.range(250.0f, 700.0f)});
}

// Geared servo whirr: gear-mesh tone + harmonics following the joint speed profile, band-passed
// mechanical noise, faint bearing hiss, all heard through the porcelain shell (low-passed).
void servo(Buf& out, Rng& r, float t0, float dur, float f0, float amp, const std::function<float(float)>& speed) {
    FastNoise n(r.next());
    Svf bp, hiss;
    hiss.set(4500.0f, 0.7f, FS);
    OnePole shell1, shell2;
    shell1.setCutoff(r.range(3000.0f, 4200.0f), FS);
    shell2.setCutoff(r.range(5000.0f, 7000.0f), FS);
    float ph = r.range(0.0f, kTau), wob = r.range(0.0f, kTau), wobF = r.range(5.0f, 11.0f);
    float h2 = r.range(0.35f, 0.55f), h3 = r.range(0.12f, 0.25f), h5 = r.range(0.04f, 0.09f);
    int i0 = idx(t0), len = idx(dur);
    for (int k = 0; k < len; ++k) {
        int i = i0 + k;
        if (i < 0 || i >= out.n()) continue;
        float t = float(k) / FS;
        float s = clampf(speed(t), 0.0f, 1.0f);
        float f = f0 * (0.3f + 0.7f * s) * (1.0f + 0.004f * fastSin(wob + kTau * wobF * t));
        ph += kTau * f / FS;
        if (ph > kTau) ph -= kTau;
        if ((k & 31) == 0) bp.set(2.3f * f, 2.5f, FS);
        float tone = fastSin(ph) + h2 * fastSin(2.0f * ph) + h3 * fastSin(3.0f * ph) + h5 * fastSin(5.03f * ph);
        float w = n.next();
        float v = 0.55f * tone + 0.9f * bp.bpNorm(w) * 0.6f + 0.05f * hiss.hp(w);
        v = shell2.lp(shell1.lp(v));
        out.x[size_t(i)] += v * amp * s * (1.25f - 0.25f * s);  // ~ s^0.8
    }
}

// Removes DC (and optionally rumble below hpHz), trims the tail below -66 dB, fades the end
// (fadeSec, smoothstep) and normalises the peak to 1.
std::vector<float> finish(Buf& b, float hpHz = 0.0f, float fadeSec = 0.004f) {
    DcBlock dc;
    dc.set(12.0f, FS);
    Svf hp;
    if (hpHz > 0.0f) hp.set(hpHz, 0.6f, FS);
    float peak = 0.0f;
    for (float& v : b.x) {
        v = dc.tick(v);
        if (hpHz > 0.0f) v = hp.hp(v);
        peak = std::max(peak, std::fabs(v));
    }
    if (!(peak > 1e-9f)) return std::vector<float>(64, 0.0f);
    int last = b.n() - 1;
    while (last > 0 && std::fabs(b.x[size_t(last)]) < peak * 5e-4f) --last;
    int end = std::min(b.n(), last + idx(0.004f));
    b.x.resize(size_t(end));
    int fade = std::min(end / 2, std::max(idx(0.004f), idx(fadeSec)));
    for (int k = 0; k < fade; ++k) {
        float a = float(k) / float(fade);
        b.x[size_t(end - 1 - k)] *= a * a * (3.0f - 2.0f * a);
    }
    float inv = 1.0f / peak;
    for (float& v : b.x) v *= inv;
    return std::vector<float>(b.x.begin(), b.x.end());  // exact size: the bank keeps no scratch capacity
}

// ---------------------------------------------------------------------------------------------
// Piece set down: felt-bottomed, lead-weighted marble piece on the marble board. The felt makes
// a ~1 ms contact (weighty low-mid "tock": slab/table compliance + lowest board modes), a small
// share of rim contact excites the stone modes and the piece itself (the marble identity), and
// the piece often rocks once or twice onto its full base.
std::vector<float> piecePlace(Rng& r) {
    Buf out(0.5f);
    const float t0 = 0.003f;
    float mass = r.range(0.85f, 1.2f);
    Modes board = boardModes(r, r.range(0.2f, 0.8f), r.range(0.2f, 0.8f));
    Modes table = tableModes(r, r.range(0.35f, 0.65f), r.range(0.35f, 0.65f));
    Modes piece = pieceModes(r, mass);
    Excitation soft(out.n()), hard(out.n());
    float tc = r.range(0.7e-3f, 1.3e-3f) * std::sqrt(mass);
    float hc = r.range(0.04f, 0.10f);
    soft.pulse(t0, tc, 1.0f);
    hard.pulse(t0 + r.range(0.0f, 0.4e-3f), r.range(0.05e-3f, 0.10e-3f), hc);
    int bounces = r.chance(0.6f) ? (r.chance(0.35f) ? 2 : 1) : 0;
    float t = t0, a = 1.0f;
    for (int b = 0; b < bounces; ++b) {
        t += r.range(0.005f, 0.018f);
        a *= r.range(0.12f, 0.3f);
        soft.pulse(t, tc * r.range(0.7f, 1.0f), a);
        hard.pulse(t + r.range(0.0f, 0.3e-3f), 0.07e-3f, a * hc * r.range(0.5f, 1.8f));
    }
    Excitation all(out.n());
    all.mixFrom(soft, 1.0f);
    all.mixFrom(hard, 1.0f);
    renderModes(out, all, board, 2.2f);
    renderModes(out, soft, table, 0.6f);
    renderModes(out, hard, piece, 0.25f);
    renderModes(out, soft, Modes{{r.range(140.0f, 190.0f) / std::sqrt(mass), r.range(0.006f, 0.009f), 0.35f}}, 1.0f);
    noiseBurst(out, r, {t0, 0.0008f, 0.004f, 0.03f, 0.05f, 150.0f, 1600.0f});
    return finish(out, 70.0f);
}

// Piece lifted: porcelain fingertips close on the piece (faint tick), the felt pad unsticks and
// slides a few millimetres, the marble rim ticks the board as the piece tilts off.
std::vector<float> piecePickup(Rng& r) {
    Buf out(0.5f);
    const float t0 = 0.003f;
    Modes piece = pieceModes(r, r.range(0.85f, 1.2f));
    Modes board = boardModes(r, r.range(0.2f, 0.8f), r.range(0.2f, 0.8f));
    Modes shell = porcelainModes(r);
    Excitation grip(out.n());
    grip.pulse(t0, r.range(0.12e-3f, 0.25e-3f), r.range(0.04f, 0.07f));
    if (r.chance(0.5f)) grip.pulse(t0 + r.range(0.002f, 0.009f), 0.15e-3f, r.range(0.015f, 0.035f));
    renderModes(out, grip, piece, 1.0f);
    renderModes(out, grip, shell, 0.25f);
    float tl = t0 + r.range(0.035f, 0.07f);
    feltSlide(out, r, tl, r.range(0.05f, 0.08f));
    Excitation rim(out.n());
    float tr = tl + r.range(0.008f, 0.03f);
    rim.pulse(tr, r.range(0.06e-3f, 0.12e-3f), r.range(0.05f, 0.09f));
    renderModes(out, rim, board, 1.6f);
    renderModes(out, rim, piece, 0.5f);
    Excitation unload(out.n());
    unload.pulse(tr, 1.5e-3f, 0.15f);
    renderModes(out, unload, tableModes(r, 0.5f, 0.5f), 0.5f);
    return finish(out, 80.0f);
}

// Marble-on-marble: the capturing hand knocks the captured piece (Hertzian contact of a few
// tens of microseconds: bright piece modes of both pieces, a crack transient, a rattle or two),
// then the captured piece is lifted off its felt.
std::vector<float> captureClick(Rng& r) {
    Buf out(0.45f);
    const float t0 = 0.003f;
    Modes p1 = pieceModes(r, r.range(0.85f, 1.2f)), p2 = pieceModes(r, r.range(0.85f, 1.2f));
    Modes board = boardModes(r, r.range(0.2f, 0.8f), r.range(0.2f, 0.8f));
    Excitation hit(out.n());
    hit.pulse(t0, r.range(0.025e-3f, 0.05e-3f), 1.0f);
    int rattles = 1 + r.below(2);
    float t = t0, a = 1.0f;
    for (int k = 0; k < rattles; ++k) {
        t += r.range(0.006f, 0.02f);
        a *= r.range(0.15f, 0.4f);
        hit.pulse(t, 0.04e-3f, a);
    }
    renderModes(out, hit, p1, 1.0f);
    renderModes(out, hit, p2, 0.8f);
    renderModes(out, hit, board, 0.35f);
    noiseBurst(out, r, {t0, 0.00005f, 0.0003f, 0.003f, 0.35f, 2500.0f, 16000.0f});
    feltSlide(out, r, t0 + r.range(0.07f, 0.13f), r.range(0.025f, 0.04f));
    return finish(out);
}

// Piece set down on the waxed oak table: felt contact on wood, table-top plate modes (hollow,
// short woody knock), the local wood response under the contact, faint rim/piece click.
std::vector<float> tablePlace(Rng& r) {
    Buf out(0.5f);
    const float t0 = 0.003f;
    float mass = r.range(0.85f, 1.2f);
    float px = r.chance(0.5f) ? r.range(0.08f, 0.3f) : r.range(0.7f, 0.92f);
    Modes table = tableModes(r, px, r.range(0.2f, 0.8f));
    Modes piece = pieceModes(r, mass);
    Modes local = jittered(r, {640.0f, 1180.0f, 2100.0f}, {0.9f, 0.6f, 0.3f}, 8.0f, 14.0f, 0.08f);
    Excitation soft(out.n()), hard(out.n());
    float tc = r.range(0.9e-3f, 1.6e-3f) * std::sqrt(mass);
    soft.pulse(t0, tc, 1.0f);
    hard.pulse(t0 + r.range(0.0f, 0.4e-3f), 0.08e-3f, r.range(0.03f, 0.08f));
    if (r.chance(0.5f)) {
        float tb = t0 + r.range(0.006f, 0.02f), ab = r.range(0.1f, 0.25f);
        soft.pulse(tb, tc * 0.8f, ab);
        hard.pulse(tb, 0.07e-3f, ab * 0.05f);
    }
    Excitation all(out.n());
    all.mixFrom(soft, 1.0f);
    all.mixFrom(hard, 1.0f);
    renderModes(out, all, table, 1.4f);
    renderModes(out, all, local, 1.4f);
    renderModes(out, hard, piece, 0.3f);
    renderModes(out, soft, Modes{{r.range(110.0f, 150.0f), 0.008f, 0.3f}}, 1.0f);
    noiseBurst(out, r, {t0, 0.0008f, 0.004f, 0.03f, 0.04f, 150.0f, 1600.0f});
    return finish(out, 70.0f);
}

std::vector<float> capture(Rng& r) {
    std::vector<float> click = captureClick(r);
    std::vector<float> knock = tablePlace(r);
    float tk = r.range(0.55f, 0.75f);
    Buf out(tk + float(knock.size()) / FS + 0.01f);
    for (size_t i = 0; i < click.size() && i < out.x.size(); ++i) out.x[i] += click[i];
    float g = r.range(0.62f, 0.78f);
    size_t o = size_t(idx(tk));
    for (size_t i = 0; i < knock.size() && o + i < out.x.size(); ++i) out.x[o + i] += knock[i] * g;
    return finish(out);
}

// Lever chess clock (DGT-like): fingertip on the plastic lever, the lever bottoms out (hard
// plastic clack through lever + hollow housing + air cavity, the clock rocks on the table), the
// micro-switch snaps (contact bounce + tiny metal ring), the opposite lever springs up against
// its stop, and the return spring rings/rattles faintly.
std::vector<float> clockPress(Rng& r) {
    Buf out(0.4f);
    const float t0 = 0.003f;
    Modes housing = jittered(r, {640.0f, 1020.0f, 1480.0f, 1930.0f, 2560.0f, 3350.0f, 4400.0f},
                             {0.6f, 0.9f, 1.0f, 0.8f, 0.7f, 0.5f, 0.35f}, 18.0f, 32.0f);
    Modes lever = jittered(r, {1850.0f, 2900.0f, 4100.0f, 5600.0f, 7300.0f}, {1.0f, 0.8f, 0.6f, 0.4f, 0.25f}, 25.0f, 45.0f);
    Modes cavity = jittered(r, {1150.0f}, {0.5f}, 5.0f, 7.0f, 0.05f);
    Modes metal = jittered(r, {6200.0f, 8900.0f, 11500.0f}, {1.0f, 0.6f, 0.4f}, 60.0f, 100.0f);
    Modes spring = jittered(r, {2250.0f, 3500.0f, 4800.0f}, {1.0f, 0.7f, 0.4f}, 200.0f, 320.0f);
    Modes table = tableModes(r, r.range(0.8f, 0.9f), r.range(0.45f, 0.55f));

    Excitation finger(out.n());
    finger.pulse(t0, r.range(0.4e-3f, 0.7e-3f), r.range(0.08f, 0.14f));
    renderModes(out, finger, housing, 0.6f);
    renderModes(out, finger, lever, 0.5f);

    float td = t0 + r.range(0.008f, 0.015f);
    Excitation down(out.n());
    down.pulse(td, r.range(0.10e-3f, 0.16e-3f), 1.0f);
    renderModes(out, down, lever, 1.0f);
    renderModes(out, down, housing, 0.8f);
    renderModes(out, down, cavity, 0.6f);
    Excitation body(out.n());
    body.pulse(td, r.range(1.5e-3f, 2.5e-3f), 0.5f);
    renderModes(out, body, table, 0.8f);
    renderModes(out, body, Modes{{r.range(110.0f, 150.0f), 0.010f, 0.4f}}, 1.0f);

    Excitation sw(out.n());
    float ts = td + r.range(-0.0012f, 0.0008f);
    sw.pulse(ts, 0.02e-3f, r.range(0.12f, 0.2f));
    int nb = 1 + r.below(3);
    float tb = ts, ab = 1.0f;
    for (int k = 0; k < nb; ++k) {
        tb += r.range(0.0003f, 0.0012f);
        ab *= r.range(0.3f, 0.6f);
        sw.pulse(tb, 0.02e-3f, 0.15f * ab);
    }
    renderModes(out, sw, metal, 1.0f);
    renderModes(out, sw, housing, 0.4f);
    noiseBurst(out, r, {ts, 0.00003f, 0.00025f, 0.002f, 0.08f, 3500.0f, 15000.0f});

    float tu = td + r.range(0.018f, 0.04f);
    Excitation up(out.n());
    up.pulse(tu, r.range(0.15e-3f, 0.25e-3f), r.range(0.3f, 0.45f));
    Modes lever2 = lever;
    float shift = r.range(0.94f, 1.06f);
    for (Mode& m : lever2) m.f *= shift;
    renderModes(out, up, lever2, 0.9f);
    renderModes(out, up, housing, 0.6f);

    Excitation sp(out.n());
    sp.pulse(td, 0.05e-3f, 0.05f);
    sp.pulse(tu, 0.05e-3f, 0.03f);
    int nr = 2 + r.below(4);
    for (int k = 0; k < nr; ++k) sp.pulse(td + r.range(0.004f, 0.07f), 0.05e-3f, r.range(0.008f, 0.025f));
    renderModes(out, sp, spring, 1.0f);
    return finish(out, 80.0f);
}

// Two robot hands clasp: padded composite palms meet (soft thump through hand/arm structure),
// fingertip porcelain shells tick faintly, then 2-3 shake strokes driven by quiet servos with
// faint rubs at the stroke reversals, and a soft release.
std::vector<float> handshake(Rng& r) {
    Buf out(1.6f);
    const float t0 = 0.005f;
    Modes arm = jittered(r, {190.0f, 310.0f, 480.0f, 700.0f, 1050.0f, 1500.0f}, {0.8f, 1.0f, 0.8f, 0.55f, 0.35f, 0.2f}, 3.0f, 8.0f);
    Modes shell = porcelainModes(r, 90.0f, 180.0f);
    Excitation palm(out.n());
    palm.pulse(t0, r.range(3e-3f, 5e-3f), 1.0f);
    float tw = t0 + r.range(0.025f, 0.06f);
    palm.pulse(tw, r.range(2.5e-3f, 4e-3f), r.range(0.3f, 0.5f));
    renderModes(out, palm, arm, 1.0f);
    noiseBurst(out, r, {t0, 0.002f, 0.012f, 0.08f, 0.12f, 120.0f, 900.0f});

    Excitation tick(out.n());
    tick.pulse(t0 + r.range(0.004f, 0.015f), 0.05e-3f, r.range(0.02f, 0.035f));
    tick.pulse(tw + r.range(0.0f, 0.01f), 0.05e-3f, r.range(0.01f, 0.02f));
    renderModes(out, tick, shell, 1.0f);

    int strokes = 2 + r.below(2);
    float P = r.range(0.26f, 0.34f);
    float ts = tw + r.range(0.06f, 0.12f);
    float dur = float(strokes) * P;
    servo(out, r, ts, dur, r.range(190.0f, 260.0f), 0.03f, [=](float t) {
        float fade = std::min(1.0f, std::min(t, dur - t) / 0.08f);
        return std::fabs(std::sin(kTau * t / P)) * std::max(fade, 0.0f);
    });
    for (int k = 0; k <= 2 * strokes; ++k) {
        float tr = ts + float(k) * 0.5f * P;  // speed reversals
        noiseBurst(out, r, {tr, 0.01f, 0.02f, 0.06f, r.range(0.012f, 0.025f), 400.0f, 2500.0f, 0.7f, 200.0f});
    }
    float trel = ts + dur + r.range(0.04f, 0.1f);
    noiseBurst(out, r, {trel, 0.015f, 0.03f, 0.09f, 0.03f, 500.0f, 3500.0f, 0.5f, 300.0f});
    Excitation rel(out.n());
    rel.pulse(trel + r.range(0.02f, 0.05f), 0.05e-3f, 0.012f);
    renderModes(out, rel, shell, 1.0f);
    return finish(out, 90.0f);
}

// Short robotic joint move: bell-shaped speed profile.
std::vector<float> servoShort(Rng& r) {
    float T = r.range(0.28f, 0.55f);
    Buf out(T + 0.35f);
    servo(out, r, 0.004f, T, r.range(300.0f, 440.0f), 1.0f, [=](float t) {
        float s = std::sin(kPi * t / T);
        return s * s;
    });
    Excitation tk(out.n());
    tk.pulse(0.006f, 0.1e-3f, 0.05f);
    tk.pulse(0.004f + T * r.range(0.9f, 1.0f), 0.1e-3f, 0.04f);
    renderModes(out, tk, porcelainModes(r), 0.4f);
    return finish(out);
}

// Wood stick-slip creak: a train of slip events whose rate wanders with the load (the pitch of
// the creak) drives the chair's wood resonances; each slip also has a tiny direct "bite".
std::vector<float> chairCreak(Rng& r) {
    float dur = r.range(0.35f, 0.8f);
    Buf out(dur + 0.4f);
    Modes wood = jittered(r, {210.0f, 380.0f, 620.0f, 930.0f, 1350.0f, 1900.0f, 2700.0f},
                          {0.5f, 1.0f, 0.9f, 0.7f, 0.5f, 0.33f, 0.2f}, 10.0f, 25.0f, 0.1f);
    Excitation ex(out.n());
    float rate0 = r.range(45.0f, 150.0f), ph = r.range(0.0f, kTau), modF = r.range(0.6f, 1.8f) / dur;
    float depth = r.range(0.2f, 0.45f);
    SlowRandom wob;
    wob.init(r, -1.0f, 1.0f, 0.05f, 0.15f);
    float t = 0.004f;
    while (t < dur) {
        float x = t / dur;
        float env = std::pow(std::sin(kPi * x), 0.6f);
        float rate = rate0 * (1.0f + depth * std::sin(kTau * modF * t + ph) + 0.15f * wob.step(r, 1.0f / rate0));
        rate = std::max(rate, 20.0f);
        ex.pulse(t, r.range(0.15e-3f, 0.4e-3f), env * r.range(0.6f, 1.3f));
        t += r.range(0.9f, 1.1f) / rate;
    }
    renderModes(out, ex, wood, 1.0f);
    OnePole hp;
    hp.setCutoff(1500.0f, FS);
    float bite = 0.25f;
    for (int i = ex.first; i <= ex.last; ++i) out.x[size_t(i)] += hp.hp(ex.f[size_t(i)]) * bite;
    return finish(out);
}

// The hall's floor under a foot or a chair leg: marble tiles bonded to a concrete slab. No plate
// rings (the slab is massive and the bond damps the tiles): a few low-mid modes of the tile on
// its mortar, Q ~ 3-8, the energy below ~1 kHz.
Modes floorModes(Rng& r) {
    return jittered(r, {95.0f, 150.0f, 235.0f, 380.0f, 640.0f, 1050.0f, 1700.0f}, {0.7f, 1.0f, 0.9f, 0.6f, 0.4f, 0.25f, 0.12f},
                    3.0f, 8.0f, 0.08f);
}

// A robot's step on the marble floor (a player getting up and walking round the table). The sole
// is a soft pad: the heel lands in a few milliseconds (no click: the floor's damped thud, a faint
// tick of the leg's porcelain shell through its joints), the ball of the foot follows a tenth of a
// second later, lighter, and the pad scuffs the polished stone as the weight rolls over it.
std::vector<float> footstep(Rng& r) {
    Buf out(0.45f);
    const float t0 = 0.003f;
    Modes floor = floorModes(r);
    float weight = r.range(0.8f, 1.2f);
    Excitation heel(out.n()), both(out.n());
    heel.pulse(t0, r.range(3.0e-3f, 5.5e-3f), weight);
    both.mixFrom(heel, 1.0f);
    float tb = t0 + r.range(0.07f, 0.13f);
    both.pulse(tb, r.range(2.5e-3f, 4.5e-3f), weight * r.range(0.3f, 0.55f));
    renderModes(out, both, floor, 1.0f);
    Excitation tick(out.n());
    tick.pulse(t0 + r.range(0.0f, 1.0e-3f), 0.15e-3f, 0.03f * weight);
    renderModes(out, tick, porcelainModes(r, 60.0f, 140.0f), 0.35f);
    noiseBurst(out, r, {t0 + r.range(0.012f, 0.03f), r.range(0.01f, 0.02f), r.range(0.03f, 0.06f), r.range(0.10f, 0.16f),
                        r.range(0.12f, 0.2f), r.range(500.0f, 800.0f), r.range(3000.0f, 4500.0f), 0.4f, r.range(150.0f, 400.0f)});
    return finish(out, 50.0f);
}

// A chair pushed back from the table or drawn in again, its oak legs sliding over the marble: each
// pair of legs sticks and slips (a train of slip events whose rate follows the sliding speed, a
// smooth bell over the push), the rear pair more loaded and at its own rate (a rough, beating
// scrape). The slips drive the chair's wood resonances (as in chairCreak) and the floor; a band of
// granular friction noise rides on top.
std::vector<float> chairSlide(Rng& r) {
    float dur = r.range(0.45f, 0.7f);
    Buf out(dur + 0.4f);
    Modes wood = jittered(r, {210.0f, 380.0f, 620.0f, 930.0f, 1350.0f, 1900.0f, 2700.0f},
                          {0.5f, 1.0f, 0.9f, 0.7f, 0.5f, 0.33f, 0.2f}, 10.0f, 25.0f, 0.1f);
    Modes floor = floorModes(r);
    Excitation ex(out.n());
    const float t0 = 0.004f;
    for (int pair = 0; pair < 2; ++pair) {
        float rate0 = r.range(90.0f, 200.0f) * (pair == 0 ? 1.0f : r.range(1.15f, 1.4f));
        float load = pair == 0 ? 1.0f : r.range(0.4f, 0.7f);
        SlowRandom wob;
        wob.init(r, -1.0f, 1.0f, 0.04f, 0.12f);
        float t = t0 + r.range(0.0f, 0.02f);
        while (t < t0 + dur) {
            float speed = std::sin(kPi * clampf((t - t0) / dur, 0.0f, 1.0f));
            float rate = std::max(25.0f, rate0 * (0.45f + 0.55f * speed) * (1.0f + 0.12f * wob.step(r, 1.0f / rate0)));
            if (r.chance(0.85f)) ex.pulse(t, r.range(0.2e-3f, 0.5e-3f), load * std::sqrt(speed) * r.range(0.6f, 1.3f));
            t += r.range(0.85f, 1.15f) / rate;
        }
    }
    renderModes(out, ex, wood, 1.0f);
    renderModes(out, ex, floor, 0.5f);
    noiseBurst(out, r, {t0, dur * 0.2f, dur * 0.6f, dur, r.range(0.3f, 0.45f), r.range(400.0f, 700.0f), r.range(2500.0f, 3800.0f),
                        0.7f, r.range(120.0f, 300.0f)});
    return finish(out, 60.0f, 0.03f);
}

std::vector<float> uiHover(Rng& r) {
    Buf out(0.08f);
    Excitation e(out.n());
    e.pulse(0.002f, 0.25e-3f, 1.0f);
    renderModes(out, e, jittered(r, {1150.0f, 2150.0f, 3700.0f}, {0.3f, 1.0f, 0.3f}, 9.0f, 14.0f, 0.03f), 1.0f);
    return finish(out);
}

std::vector<float> uiClick(Rng& r) {
    Buf out(0.13f);
    Modes m = jittered(r, {520.0f, 1350.0f, 2650.0f, 4100.0f}, {0.5f, 1.0f, 0.45f, 0.2f}, 12.0f, 20.0f, 0.02f);
    Excitation e(out.n());
    e.pulse(0.002f, 0.35e-3f, 1.0f);
    renderModes(out, e, m, 1.0f);
    Modes m2 = m;
    for (Mode& x : m2) x.f *= 1.08f;
    Excitation e2(out.n());
    e2.pulse(0.002f + r.range(0.012f, 0.018f), 0.3e-3f, 0.25f);
    renderModes(out, e2, m2, 1.0f);
    return finish(out);
}

// Single soft low tone (singing-bowl-like, soft mallet): inharmonic partials as slowly beating
// pairs, no melody.
std::vector<float> bowl(Rng& r, float f0, float seconds, float bright, float decayMul) {
    Buf out(seconds);
    static const float ratio[] = {1.0f, 2.76f, 5.40f, 8.93f};
    const float amp[] = {1.0f, 0.30f * bright, 0.09f * bright * bright, 0.03f * bright * bright * bright};
    const float tau[] = {1.5f * decayMul, 0.85f * decayMul, 0.45f * decayMul, 0.25f * decayMul};
    const float attack = 0.012f;
    for (int p = 0; p < 4; ++p) {
        float f = f0 * ratio[p] * r.jit(1.0f, 0.004f);
        float beat = r.range(0.6f, 1.6f);
        float bal = r.range(0.35f, 0.5f);
        for (int k = 0; k < 2; ++k) {
            float fk = f + (k ? 0.5f : -0.5f) * beat;
            float ak = amp[p] * (k ? bal : 1.0f - bal);
            float ph = r.range(0.0f, kTau), w = kTau * fk / FS;
            float decay = std::exp(-1.0f / (tau[p] * FS)), env = 1.0f;
            const int na = idx(attack);
            for (int i = 0; i < out.n(); ++i) {
                float a = i < na ? float(i) / float(na) : 1.0f;
                float att = a * a * (3.0f - 2.0f * a);
                out.x[size_t(i)] += ak * att * env * fastSin(ph);
                ph += w;
                if (ph > kTau) ph -= kTau;
                env *= decay;
            }
        }
    }
    noiseBurst(out, r, {0.0f, 0.002f, 0.01f, 0.05f, 0.02f, 60.0f, 500.0f});
    return finish(out, 0.0f, 0.4f * seconds);  // long natural-sounding release, no truncation
}

// ---------------------------------------------------------------------------------------------
// Scoresheet and pen. The pad is a 5 mm stack of paper on a grey card board lying on the table:
// a soft, heavily damped support (card + paper modes at a few hundred Hz to ~3 kHz, Q ~ 6-12)
// that colours everything the pen and the pages do.
Modes padModes(Rng& r) {
    return jittered(r, {380.0f, 690.0f, 1120.0f, 1650.0f, 2400.0f, 3300.0f}, {0.7f, 1.0f, 0.9f, 0.7f, 0.5f, 0.3f}, 6.0f,
                    12.0f, 0.08f);
}

// Ballpoint writing: the ball rolls in its socket over the paper fibres. The friction noise is
// broadband with a stick-slip grain whose density and brightness follow the tip speed; handwriting
// moves the tip in quick strokes (60-170 ms each, ~9 per second) with speed minima at the
// direction reversals, where the ball ticks faintly as it changes direction. A sustained ~3 s
// texture: the mixer plays a window of it per pen-down stroke.
std::vector<float> penWrite(Rng& r) {
    const float T = r.range(2.9f, 3.3f);
    Buf out(T + 0.1f);
    // Stroke speed profile: a chain of strokes of random length and peak speed.
    std::vector<float> speed(size_t(out.n()), 0.0f);
    std::vector<int> reversals;
    float t = 0.0f;
    while (t < T) {
        float d = r.range(0.06f, 0.17f), peak = r.range(0.45f, 1.0f);
        int i0 = idx(t), i1 = std::min(out.n(), idx(t + d));
        for (int i = i0; i < i1; ++i) {
            float x = float(i - i0) / float(std::max(1, i1 - i0));
            float s = std::sin(kPi * x);
            speed[size_t(i)] = 0.12f + 0.88f * peak * std::pow(s, 0.8f);  // the ball never quite stops
        }
        reversals.push_back(i1);
        t += d;
    }
    // Fade in/out over the first/last 60 ms so the whole buffer is a clean one-shot too.
    const int edge = idx(0.06f), stop = out.n() - idx(0.02f);
    for (int i = 0; i < out.n(); ++i) {
        float a = std::max(0.0f, std::min(1.0f, float(std::min(i, stop - i)) / float(edge)));
        speed[size_t(i)] *= a * a * (3.0f - 2.0f * a);
    }
    FastNoise n(r.next());
    Svf hp, lp, body;
    hp.set(r.range(900.0f, 1300.0f), 0.6f, FS);
    body.set(r.range(2600.0f, 3400.0f), 1.4f, FS);
    Excitation drive(out.n());
    const float grainDepth = r.range(0.45f, 0.65f);
    float ge = 0.0f;
    const float gdec = std::exp(-1.0f / (0.0004f * FS));
    for (int i = 0; i < out.n(); ++i) {
        float s = speed[size_t(i)];
        if ((i & 31) == 0) lp.set(2500.0f + 5500.0f * s, 0.6f, FS);  // brighter when faster
        float w = n.next();
        // Stick-slip grain: 250-1100 micro-slips per second, denser with speed.
        ge *= gdec;
        if (r.uni() < (250.0f + 850.0f * s) / FS) ge += r.range(0.4f, 1.6f);
        float v = lp.lp(hp.hp(w)) * ((1.0f - grainDepth) + grainDepth * ge);
        v += 0.35f * body.bpNorm(w) * s;  // the ball's own ring in its socket
        float amp = s * s * (1.4f - 0.4f * s);
        out.x[size_t(i)] += 0.2f * v * amp;
        drive.add(i, 0.02f * v * amp);
    }
    // The pad picks up the friction (its low modes give the writing its "on a pad" body).
    renderModes(out, drive, padModes(r), 1.0f);
    // Faint ticks at the direction reversals.
    Excitation ticks(out.n());
    for (int i : reversals)
        if (i > edge && i < stop - edge && r.chance(0.6f)) ticks.pulse(float(i) / FS, 0.08e-3f, r.range(0.01f, 0.03f));
    renderModes(out, ticks, padModes(r), 1.0f);
    renderModes(out, ticks, jittered(r, {5200.0f, 7900.0f}, {1.0f, 0.6f}, 20.0f, 40.0f), 0.5f);
    return finish(out, 400.0f, 0.03f);
}

// The ballpoint touching down: the tip's steel ball and plastic cone tick against the paper, the
// pad under it thumps very softly.
std::vector<float> penTap(Rng& r) {
    Buf out(0.12f);
    const float t0 = 0.002f;
    Excitation e(out.n());
    e.pulse(t0, r.range(0.10e-3f, 0.2e-3f), 1.0f);
    renderModes(out, e, padModes(r), 1.0f);
    renderModes(out, e, jittered(r, {3900.0f, 6100.0f, 8700.0f}, {1.0f, 0.6f, 0.35f}, 25.0f, 45.0f, 0.08f), 0.35f);
    Excitation soft(out.n());
    soft.pulse(t0, r.range(0.8e-3f, 1.4e-3f), 0.6f);
    renderModes(out, soft, Modes{{r.range(160.0f, 220.0f), 0.004f, 0.3f}}, 1.0f);
    noiseBurst(out, r, {t0, 0.0002f, 0.0015f, 0.012f, 0.05f, 2000.0f, 9000.0f});
    return finish(out, 120.0f);
}

// Crackle of a bending sheet: sparse micro-buckling events (Poisson, rate(t) per second), each a
// short band-passed noise grain at a random pitch, amplitudes spread over ~20 dB.
void paperCrackle(Buf& out, Rng& r, float t0, float dur, float amp, const std::function<float(float)>& rate) {
    FastNoise n(r.next());
    float t = t0;
    while (t < t0 + dur) {
        float lambda = std::max(1.0f, rate(t - t0));
        t += r.expo(1.0f / lambda);
        if (t >= t0 + dur) break;
        Svf bp;
        bp.set(r.logRange(1800.0f, 9000.0f), r.range(1.0f, 3.0f), FS);
        float a = amp * std::pow(10.0f, -r.uni()), len = r.range(0.0004f, 0.003f);
        int i0 = idx(t), nl = std::max(4, idx(len));
        for (int k = 0; k < nl && i0 + k < out.n(); ++k) {
            float e = std::exp(-4.0f * float(k) / float(nl));
            out.x[size_t(i0 + k)] += bp.bpNorm(n.next()) * a * e;
        }
    }
}

// Page turn: the fingertip slides under the corner and pinches it (fingernail scratch on paper,
// a light tap on the stack), the sheet bends and lifts (crackles, the page unsticking from the one
// below), then swings over the binding (air whoosh: low-passed noise following the swing speed,
// with a little flutter). The landing is PageFlap.
std::vector<float> pageTurn(Rng& r) {
    const float T = r.range(1.0f, 1.15f);
    Buf out(T + 0.15f);
    const float t0 = 0.004f;
    noiseBurst(out, r, {t0, 0.01f, 0.03f, 0.1f, r.range(0.10f, 0.16f), 1800.0f, 8000.0f, 0.6f, r.range(500.0f, 900.0f)});
    Excitation tap(out.n());
    tap.pulse(t0 + r.range(0.04f, 0.08f), 0.4e-3f, 0.3f);
    renderModes(out, tap, padModes(r), 1.0f);
    // Lift and bend: crackles densest while the curl forms.
    const float tb = t0 + r.range(0.1f, 0.14f);
    paperCrackle(out, r, tb, 0.55f, r.range(0.25f, 0.35f), [](float t) {
        float x = t / 0.55f;
        return 40.0f + 260.0f * std::sin(kPi * std::min(1.0f, x)) * std::exp(-1.5f * x);
    });
    noiseBurst(out, r, {tb, 0.04f, 0.12f, 0.35f, r.range(0.08f, 0.12f), 600.0f, 5000.0f, 0.3f, 300.0f});
    // Swing: whoosh through the air, speed peaking past the middle of the turn.
    FastNoise n(r.next());
    Svf lp, hp;
    hp.set(180.0f, 0.6f, FS);
    const float ts = t0 + 0.2f, te = T - 0.02f;
    const float fl = r.range(9.0f, 14.0f), flPh = r.range(0.0f, kTau), wAmp = r.range(0.35f, 0.5f);
    for (int i = idx(ts); i < idx(te) && i < out.n(); ++i) {
        float x = (float(i) / FS - ts) / (te - ts);
        float s = std::sin(kPi * x);
        s = s * s * (0.6f + 0.4f * x);
        if ((i & 31) == 0) lp.set(500.0f + 1800.0f * s, 0.7f, FS);
        float flutter = 1.0f + 0.25f * fastSin(kTau * fl * float(i) / FS + flPh);
        out.x[size_t(i)] += lp.lp(hp.hp(n.next())) * s * flutter * wAmp;
    }
    // A few crackles as the sheet straightens over the top.
    paperCrackle(out, r, t0 + 0.55f, 0.35f, 0.12f, [](float) { return 45.0f; });
    return finish(out, 120.0f, 0.06f);
}

// Page landing face down: the air cushion under the falling sheet escapes (a soft low puff), the
// sheet slaps the stack (pad thump) and settles with a couple of paper ticks.
std::vector<float> pageFlap(Rng& r) {
    Buf out(0.4f);
    const float t0 = 0.004f;
    noiseBurst(out, r, {t0, 0.012f, 0.035f, 0.15f, r.range(0.5f, 0.7f), 90.0f, r.range(700.0f, 1000.0f)});
    Excitation slap(out.n());
    float ts = t0 + r.range(0.018f, 0.03f);
    slap.pulse(ts, r.range(2.0e-3f, 3.5e-3f), 1.0f);
    renderModes(out, slap, padModes(r), 0.8f);
    renderModes(out, slap, tableModes(r, 0.2f, r.range(0.2f, 0.8f)), 0.35f);
    noiseBurst(out, r, {ts, 0.001f, 0.01f, 0.05f, r.range(0.15f, 0.25f), 1200.0f, 7000.0f});
    paperCrackle(out, r, ts + 0.01f, 0.12f, 0.08f, [](float t) { return 60.0f * std::exp(-t / 0.05f); });
    return finish(out, 60.0f);
}

}  // namespace

const SfxInfo& sfxInfo(Sfx s) {
    //                      name            level(dB)         send   pitchJ  lvlJ  ui
    static const SfxInfo table[int(Sfx::Count)] = {
        {"piece_pickup", dbToGain(-19.0f), 0.25f, 0.03f, 1.5f, false},
        {"piece_place", dbToGain(-9.0f), 0.22f, 0.03f, 1.5f, false},
        {"capture", dbToGain(-9.5f), 0.22f, 0.03f, 1.5f, false},
        {"clock_press", dbToGain(-11.0f), 0.22f, 0.03f, 1.5f, false},
        {"handshake", dbToGain(-13.0f), 0.22f, 0.03f, 1.5f, false},
        {"servo_short", dbToGain(-30.0f), 0.2f, 0.03f, 2.0f, false},
        {"chair_creak", dbToGain(-19.0f), 0.3f, 0.03f, 2.0f, false},
        {"ui_hover", dbToGain(-33.0f), 0.0f, 0.02f, 1.0f, true},
        {"ui_click", dbToGain(-25.0f), 0.0f, 0.02f, 1.0f, true},
        {"game_start", dbToGain(-22.0f), 0.35f, 0.004f, 0.5f, true},
        {"game_end", dbToGain(-21.0f), 0.4f, 0.004f, 0.5f, true},
        {"capture_click", dbToGain(-10.0f), 0.22f, 0.03f, 1.5f, false},
        {"table_place", dbToGain(-10.0f), 0.22f, 0.03f, 1.5f, false},
        {"pen_write", dbToGain(-24.0f), 0.18f, 0.04f, 1.5f, false},
        {"pen_tap", dbToGain(-30.0f), 0.18f, 0.05f, 2.0f, false},
        {"page_turn", dbToGain(-20.0f), 0.25f, 0.04f, 1.5f, false},
        {"page_flap", dbToGain(-19.0f), 0.25f, 0.04f, 1.5f, false},
        {"footstep", dbToGain(-21.0f), 0.3f, 0.05f, 2.0f, false},
        {"chair_slide", dbToGain(-21.0f), 0.3f, 0.04f, 1.5f, false},
    };
    int i = int(s);
    if (i < 0 || i >= int(Sfx::Count)) i = 0;
    return table[i];
}

std::vector<float> synthesize(Sfx s, uint32_t seed) {
    DenormalGuard guard;
    Rng r(seed, uint64_t(s) + 1u);
    switch (s) {
        case Sfx::PiecePickup: return piecePickup(r);
        case Sfx::PiecePlace: return piecePlace(r);
        case Sfx::Capture: return capture(r);
        case Sfx::ClockPress: return clockPress(r);
        case Sfx::Handshake: return handshake(r);
        case Sfx::ServoShort: return servoShort(r);
        case Sfx::ChairCreak: return chairCreak(r);
        case Sfx::UIHover: return uiHover(r);
        case Sfx::UIClick: return uiClick(r);
        case Sfx::GameStart: return bowl(r, 196.0f, 4.5f, 1.0f, 1.0f);
        case Sfx::GameEnd: return bowl(r, 146.8f, 5.5f, 0.7f, 1.2f);
        case Sfx::CaptureClick: return captureClick(r);
        case Sfx::TablePlace: return tablePlace(r);
        case Sfx::PenWrite: return penWrite(r);
        case Sfx::PenTap: return penTap(r);
        case Sfx::PageTurn: return pageTurn(r);
        case Sfx::PageFlap: return pageFlap(r);
        case Sfx::Footstep: return footstep(r);
        case Sfx::ChairSlide: return chairSlide(r);
        default: return std::vector<float>(64, 0.0f);
    }
}

const char* sfxName(Sfx s) { return sfxInfo(s).name; }

int bankVariants(Sfx s) {
    // The long, rarely played tones keep fewer variants (memory); the pen friction texture is
    // played in random windows, so a few long variants are plenty; everything else kBankVariants.
    if (s == Sfx::GameStart || s == Sfx::GameEnd) return 3;
    if (s == Sfx::PenWrite) return 4;
    return kBankVariants;
}

}  // namespace audio
