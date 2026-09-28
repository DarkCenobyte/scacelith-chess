// Small DSP toolkit shared by the synthesiser, the reverb, the ambience and the mixer.
// Header-only, allocation-free after init, denormal-safe when run under DenormalGuard.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#define AUDIO_HAS_SSE 1
#endif

namespace audio::dsp {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTau = 6.28318530717958647692f;

inline float dbToGain(float db) { return std::pow(10.0f, db * 0.05f); }
inline float gainToDb(float g) { return 20.0f * std::log10(std::max(g, 1e-12f)); }
inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }

// Flush-to-zero / denormals-are-zero for the current thread (restored on destruction).
struct DenormalGuard {
#ifdef AUDIO_HAS_SSE
    unsigned csr;
    DenormalGuard() : csr(_mm_getcsr()) { _mm_setcsr(csr | 0x8040u); }
    ~DenormalGuard() { _mm_setcsr(csr); }
#else
    DenormalGuard() = default;
#endif
    DenormalGuard(const DenormalGuard&) = delete;
    DenormalGuard& operator=(const DenormalGuard&) = delete;
};

// PCG32: small, fast, good statistical quality. Deterministic per seed.
struct Rng {
    uint64_t state = 0x853c49e6748fea9bULL, inc = 0xda3e39cb94b95bdbULL;
    Rng() = default;
    explicit Rng(uint64_t seed, uint64_t seq = 54u) { reseed(seed, seq); }
    void reseed(uint64_t seed, uint64_t seq = 54u) {
        state = 0u;
        inc = (seq << 1u) | 1u;
        next();
        state += seed;
        next();
    }
    uint32_t next() {
        uint64_t old = state;
        state = old * 6364136223846793005ULL + inc;
        uint32_t xorshifted = uint32_t(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = uint32_t(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }
    float uni() { return float(next() >> 8) * (1.0f / 16777216.0f); }            // [0,1)
    float bi() { return uni() * 2.0f - 1.0f; }                                    // [-1,1)
    float range(float a, float b) { return a + (b - a) * uni(); }
    float jit(float x, float rel) { return x * (1.0f + rel * bi()); }              // x * (1 +- rel)
    float logRange(float a, float b) { return a * std::pow(b / a, uni()); }        // log-uniform
    float gauss() { return (uni() + uni() + uni() + uni() - 2.0f) * 1.7320508f; }  // ~N(0,1)
    bool chance(float p) { return uni() < p; }
    int below(int n) { return int((uint64_t(next()) * uint64_t(n)) >> 32); }
    float expo(float mean) { return -mean * std::log(1.0f - uni() * 0.999999f); }  // exponential
};

// Very cheap white noise for the real-time paths (LCG, top bits only).
struct FastNoise {
    uint32_t s = 22222u;
    explicit FastNoise(uint32_t seed = 22222u) : s(seed * 2654435761u + 1u) {}
    float next() { s = s * 1664525u + 1013904223u; return float(int32_t(s)) * (1.0f / 2147483648.0f); }
};

// Pink noise (Paul Kellet's economy filter, ~ -3 dB/oct from 40 Hz to 10+ kHz), unit-ish RMS.
struct Pink {
    float b0 = 0, b1 = 0, b2 = 0;
    float tick(float w) {
        b0 = 0.99765f * b0 + w * 0.0990460f;
        b1 = 0.96300f * b1 + w * 0.2965164f;
        b2 = 0.57000f * b2 + w * 1.0526913f;
        return (b0 + b1 + b2 + w * 0.1848f) * 0.22f;
    }
};

// Leaky integrator ("brown" noise), DC-free thanks to the leak.
struct Brown {
    float z = 0, leak = 0.997f;
    float tick(float w) { z = z * leak + w * 0.06f; return z; }
};

// One-pole low-pass. coef = exp(-2 pi fc / fs); coef 0 = bypass.
struct OnePole {
    float a = 0, z = 0;
    static float coefFor(float fc, float fs) { return std::exp(-kTau * std::min(fc, 0.49f * fs) / fs); }
    void setCutoff(float fc, float fs) { a = coefFor(fc, fs); }
    float lp(float x) { z = x + a * (z - x); return z; }
    float hp(float x) { return x - lp(x); }
    void reset() { z = 0; }
};

// DC blocker (first-order high-pass, ~fc Hz).
struct DcBlock {
    float r = 0.998f, x1 = 0, y1 = 0;
    void set(float fc, float fs) { r = 1.0f - kTau * fc / fs; }
    float tick(float x) { float y = x - x1 + r * y1; x1 = x; y1 = y; return y; }
};

// Topology-preserving-transform state variable filter (Zavalishin / Simper). Safe to modulate.
struct Svf {
    float g = 0, k = 1, a1 = 0, a2 = 0, a3 = 0, ic1 = 0, ic2 = 0;
    void set(float fc, float q, float fs) {
        fc = clampf(fc, 5.0f, 0.47f * fs);
        g = std::tan(kPi * fc / fs);
        k = 1.0f / std::max(q, 0.05f);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void tick(float v0, float& lo, float& band, float& high) {
        float v3 = v0 - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lo = v2;
        band = v1;
        high = v0 - k * v1 - v2;
    }
    float lp(float x) { float l, b, h; tick(x, l, b, h); return l; }
    float bp(float x) { float l, b, h; tick(x, l, b, h); return b; }
    float bpNorm(float x) { float l, b, h; tick(x, l, b, h); return b * k; }  // unity gain at fc
    float hp(float x) { float l, b, h; tick(x, l, b, h); return h; }
    void reset() { ic1 = ic2 = 0; }
};

// Two-pole modal resonator: impulse response ~ gain * exp(-t/tau) * sin(2 pi f t).
struct Resonator {
    float b = 0, c1 = 0, c2 = 0, y1 = 0, y2 = 0;
    void set(float freq, float tau, float gain, float fs) {
        float w = kTau * std::min(freq, 0.48f * fs) / fs;
        float r = std::exp(-1.0f / (std::max(tau, 1e-5f) * fs));
        c1 = 2.0f * r * std::cos(w);
        c2 = -r * r;
        b = gain * std::sin(w);
    }
    float tick(float x) {
        float y = b * x + c1 * y1 + c2 * y2;
        y2 = y1;
        y1 = y;
        return y;
    }
    void reset() { y1 = y2 = 0; }
};

// Power-of-two circular delay line.
struct DelayLine {
    std::vector<float> buf;
    uint32_t mask = 0, w = 0;
    void init(int maxLen) {
        uint32_t n = 1;
        while (n < uint32_t(maxLen) + 4u) n <<= 1;
        buf.assign(n, 0.0f);
        mask = n - 1;
        w = 0;
    }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); }
    void push(float x) { buf[w & mask] = x; ++w; }
    // d = 1 returns the most recently pushed sample.
    float tap(int d) const { return buf[(w - uint32_t(d)) & mask]; }
    float tapFrac(float d) const {
        int i = int(d);
        float f = d - float(i);
        float a = tap(i), b = tap(i + 1);
        return a + (b - a) * f;
    }
};

// Schroeder all-pass diffuser built on a DelayLine.
struct Allpass {
    DelayLine d;
    int len = 1;
    float g = 0.6f;
    void init(int length, float gain) { len = std::max(1, length); g = gain; d.init(len + 1); }
    float tick(float x) {
        float delayed = d.tap(len);
        float v = x + g * delayed;
        d.push(v);
        return delayed - g * v;
    }
};

// Smooth random control signal: glides (cosine) between random targets at random intervals.
struct SlowRandom {
    float from = 0, to = 0, t = 1, dt = 0, lo = 0, hi = 1, minDur = 1, maxDur = 3;
    void init(Rng& r, float lo_, float hi_, float minSec, float maxSec) {
        lo = lo_; hi = hi_; minDur = minSec; maxDur = maxSec;
        from = to = r.range(lo, hi);
        t = 1.0f;
    }
    // Advance by 'seconds' and return the current value.
    float step(Rng& r, float seconds) {
        t += dt * seconds;
        if (t >= 1.0f) {
            from = to;
            to = r.range(lo, hi);
            t = 0.0f;
            dt = 1.0f / r.range(minDur, maxDur);
        }
        float s = 0.5f - 0.5f * std::cos(kPi * t);
        return from + (to - from) * s;
    }
};

// sin(x) for |x| < ~1e6: reduced to [-pi/2, pi/2], 11th-order Taylor (|error| < 1e-6). Much
// cheaper than libm sinf (notably the MinGW one) in per-sample synthesis loops.
inline float fastSin(float x) {
    float k = float(int(x * (1.0f / kTau) + (x >= 0.0f ? 0.5f : -0.5f)));
    x -= k * kTau;  // [-pi, pi]
    if (x > 0.5f * kPi) x = kPi - x;
    else if (x < -0.5f * kPi) x = -kPi - x;
    float x2 = x * x;
    return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f - x2 * (1.0f / 39916800.0f))))));
}

// 4-point, 3rd-order Hermite interpolation (x in [0,1) between y0 and y1; ym1, y2 neighbours).
inline float hermite(float ym1, float y0, float y1, float y2, float x) {
    float c1 = 0.5f * (y1 - ym1);
    float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
    return ((c3 * x + c2) * x + c1) * x + y0;
}

}  // namespace audio::dsp
