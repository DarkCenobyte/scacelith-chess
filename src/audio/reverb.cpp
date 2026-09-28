#include "reverb.h"
#include <cmath>

namespace audio {
using namespace dsp;

namespace {
bool isPrime(int n) {
    if (n < 2) return false;
    for (int d = 2; d * d <= n; ++d)
        if (n % d == 0) return false;
    return true;
}
int nearestPrime(int n) {
    for (int k = 0;; ++k) {
        if (isPrime(n + k)) return n + k;
        if (n - k > 1 && isPrime(n - k)) return n - k;
    }
}
// In-place fast Walsh-Hadamard transform of 16 values, scaled to be orthonormal.
inline void fwht16(float* v) {
    for (int h = 1; h < 16; h <<= 1)
        for (int i = 0; i < 16; i += h << 1)
            for (int j = i; j < i + h; ++j) {
                float a = v[j], b = v[j + h];
                v[j] = a + b;
                v[j + h] = a - b;
            }
    for (int i = 0; i < 16; ++i) v[i] *= 0.25f;
}

// Line lengths (ms): spread 22-76 ms (mean free path of the hall ~26 ms), made prime per rate.
const float kLineMs[HallReverb::kLines] = {22.1f, 24.9f, 27.3f, 29.9f, 32.6f, 35.2f, 38.1f, 41.3f,
                                           44.6f, 48.0f, 51.9f, 56.1f, 60.4f, 65.3f, 70.7f, 76.4f};
const float kOutSignL[HallReverb::kLines] = {1, -1, 1, 1, -1, 1, -1, -1, 1, 1, -1, 1, -1, -1, 1, -1};
const float kInSign[HallReverb::kLines] = {1, 1, -1, 1, -1, -1, 1, -1, 1, -1, 1, 1, -1, 1, -1, -1};
constexpr float kOnset = 0.35f;  // level of the diffuse onset relative to the line sum

// Early reflections relative to the direct sound, for a listener seated at the table:
// {delay ms, gain, pan (-1 left .. +1 right)}. Gains ~ (direct distance / path length) relative
// to the nominal send, stone/glass reflect strongly, tapestries and the vault diffuse.
struct ErTap { float ms, gain, pan; };
const ErTap kErTaps[HallReverb::kTaps] = {
    {3.4f, 0.20f, 0.05f},   {5.3f, 0.15f, 0.35f},   {7.4f, 0.12f, -0.4f},   // robots, chairs, table
    {12.8f, 0.07f, -0.6f},  {16.2f, 0.08f, 0.55f},  {21.5f, 0.07f, -0.2f},  // pillars / furniture
    {27.1f, 0.08f, 0.7f},   {31.4f, 0.07f, -0.75f},
    {37.9f, 0.26f, -0.9f},  {39.6f, 0.24f, 0.9f},                           // side walls (windows L)
    {44.2f, 0.20f, 0.0f},                                                   // vault
    {48.7f, 0.08f, 0.4f},   {53.3f, 0.07f, -0.3f},
    {60.2f, 0.15f, -0.15f}, {62.3f, 0.14f, 0.15f},                          // end walls (tapestries)
    {70.4f, 0.07f, 0.6f},   {77.2f, 0.06f, -0.55f}, {84.9f, 0.05f, 0.2f},
};
}  // namespace

void HallReverb::prepare(float sampleRate) {
    fs_ = sampleRate;
    inHp_.setCutoff(90.0f, fs_);
    inLp_.setCutoff(9000.0f, fs_);
    outLpL_.setCutoff(7500.0f, fs_);
    outLpR_.setCutoff(7500.0f, fs_);

    int maxTap = 0;
    for (int t = 0; t < kTaps; ++t) {
        tapDelay_[t] = std::max(1, int(std::lround(kErTaps[t].ms * 0.001f * fs_)));
        maxTap = std::max(maxTap, tapDelay_[t]);
        float a = (kErTaps[t].pan * 0.5f + 0.5f) * 0.5f * kPi;
        tapL_[t] = kErTaps[t].gain * std::cos(a) * 1.41421356f;
        tapR_[t] = kErTaps[t].gain * std::sin(a) * 1.41421356f;
    }
    er_.init(maxTap + kBlock + 2);

    preLen_ = std::max(1, int(std::lround(preDelay * fs_)));
    pre_.init(preLen_ + 2);
    const float apMs[4] = {2.96f, 2.23f, 7.9f, 5.8f};
    const float apG[4] = {0.62f, 0.62f, 0.55f, 0.55f};
    for (int i = 0; i < 4; ++i) ap_[i].init(nearestPrime(int(apMs[i] * 0.001f * fs_)), apG[i]);
    onsetL_.init(nearestPrime(int(0.0113f * fs_)), 0.55f);
    onsetR_.init(nearestPrime(int(0.0137f * fs_)), 0.55f);

    int maxLen = 0;
    for (int i = 0; i < kLines; ++i) {
        len_[i] = nearestPrime(int(kLineMs[i] * 0.001f * fs_));
        maxLen = std::max(maxLen, len_[i]);
        // Absorption: one-pole with DC gain gdc, Nyquist gain gny (Jot).
        float gdc = std::pow(10.0f, -3.0f * float(len_[i]) / (rt60Low * fs_));
        float gny = std::pow(10.0f, -3.0f * float(len_[i]) / (rt60High * fs_));
        fa_[i] = (gdc - gny) / (gdc + gny);
        fb_[i] = gdc * (1.0f - fa_[i]);
    }
    // Slow modulation on four lines: +-0.2..0.4 ms at 0.09..0.6 Hz.
    const int modLines[kModLines] = {3, 7, 10, 14};
    const float modMs[kModLines] = {0.2f, 0.33f, 0.27f, 0.4f}, modHz[kModLines] = {0.6f, 0.23f, 0.41f, 0.09f};
    float maxDepth = 0.0f;
    for (int k = 0; k < kModLines; ++k) {
        modLine_[k] = modLines[k];
        modDepth_[k] = modMs[k] * 0.001f * fs_;
        modInc_[k] = kTau * modHz[k] / fs_;
        modPhase_[k] = float(k) * 1.7f;
        modCur_[k] = modDepth_[k] * (1.0f + std::sin(modPhase_[k]));
        maxDepth = std::max(maxDepth, 2.0f * modDepth_[k]);
    }
    lineSize_ = 1;
    while (lineSize_ < uint32_t(maxLen + int(maxDepth) + 4)) lineSize_ <<= 1;
    lineMask_ = lineSize_ - 1;
    lines_.assign(size_t(lineSize_) * kLines, 0.0f);
    lineW_ = 0;
    // Output normalisation: calibrated so a unit impulse gives ~unit IR energy (tests).
    norm_ = 0.9f;
    clear();
}

void HallReverb::clear() {
    er_.clear();
    pre_.clear();
    for (auto& a : ap_) a.d.clear();
    onsetL_.d.clear();
    onsetR_.d.clear();
    std::fill(lines_.begin(), lines_.end(), 0.0f);
    for (int i = 0; i < kLines; ++i) fz_[i] = 0.0f;
    inHp_.reset();
    inLp_.reset();
    outLpL_.reset();
    outLpR_.reset();
}

void HallReverb::process(const float* in, float* outL, float* outR, int n) {
    while (n > 0) {
        int m = std::min(n, kBlock);
        processBlock(in, outL, outR, m);
        in += m;
        outL += m;
        outR += m;
        n -= m;
    }
}

void HallReverb::processBlock(const float* in, float* outL, float* outR, int n) {
    float x[kBlock];
    for (int k = 0; k < n; ++k) {
        float v = in[k];
        v -= inHp_.lp(v);
        x[k] = inLp_.lp(v);
    }

    // Early reflections: block-wise taps over contiguous ring segments (vectorisable).
    const uint32_t w0 = er_.w;
    for (int k = 0; k < n; ++k) er_.push(x[k]);
    if (earlyGain != 0.0f) {
        const uint32_t size = er_.mask + 1;
        const float* buf = er_.buf.data();
        for (int t = 0; t < kTaps; ++t) {
            const float gl = tapL_[t] * earlyGain, gr = tapR_[t] * earlyGain;
            uint32_t idx = (w0 - uint32_t(tapDelay_[t])) & er_.mask;
            int k = 0;
            while (k < n) {
                int seg = std::min(n - k, int(size - idx));
                const float* src = buf + idx;
                for (int j = 0; j < seg; ++j) {
                    outL[k + j] += src[j] * gl;
                    outR[k + j] += src[j] * gr;
                }
                k += seg;
                idx = 0;
            }
        }
    }

    // Modulation targets for the end of this block (linear ramp per sample).
    float modStep[kModLines];
    for (int j = 0; j < kModLines; ++j) {
        modPhase_[j] += modInc_[j] * float(n);
        if (modPhase_[j] > kTau) modPhase_[j] -= kTau;
        float target = modDepth_[j] * (1.0f + std::sin(modPhase_[j]));
        modStep[j] = (target - modCur_[j]) / float(n);
    }

    const float lg = lateGain * norm_;
    float* L = lines_.data();
    const uint32_t mask = lineMask_, size = lineSize_;
    float y[kLines];
    for (int k = 0; k < n; ++k) {
        pre_.push(x[k]);
        float xd = pre_.tap(preLen_);
        for (auto& a : ap_) xd = a.tick(xd);
        const float oL = onsetL_.tick(xd) * kOnset, oR = onsetR_.tick(xd) * kOnset;
        xd *= 0.25f;

        const uint32_t w = lineW_;
        for (int i = 0; i < kLines; ++i) y[i] = L[size_t(i) * size + ((w - uint32_t(len_[i])) & mask)];
        for (int j = 0; j < kModLines; ++j) {
            modCur_[j] += modStep[j];
            const int i = modLine_[j];
            float d = float(len_[i]) + modCur_[j];
            int di = int(d);
            float fr = d - float(di);
            const float* base = L + size_t(i) * size;
            float a = base[(w - uint32_t(di)) & mask], b = base[(w - uint32_t(di) - 1u) & mask];
            y[i] = a + (b - a) * fr;
        }
        for (int i = 0; i < kLines; ++i) {
            fz_[i] = fb_[i] * y[i] + fa_[i] * fz_[i];
            y[i] = fz_[i];
        }
        fwht16(y);
        float sl = 0.0f, sr = 0.0f;
        for (int i = 0; i < kLines; ++i) {
            float v = y[i] + kInSign[i] * xd;
            L[size_t(i) * size + (w & mask)] = v;
            float s = kOutSignL[i] * v;
            sl += s;
            sr += (i & 1) ? -s : s;
        }
        lineW_ = w + 1u;
        outL[k] += outLpL_.lp(sl + oL) * lg;
        outR[k] += outLpR_.lp(sr + oR) * lg;
    }
}

}  // namespace audio
