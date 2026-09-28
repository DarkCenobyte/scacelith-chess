// Audio package tests: every sound is rendered through the full chain and checked numerically
// (no clipping, no DC, no NaN/denormals, durations, spectral centroids), the hall reverb RT60
// and pre-delay are measured, the 3D stage is probed, the lock-free queue is stressed, the
// mixer CPU cost is measured and the live engine is started/stopped.
// WAV files for listening are written to /tmp/audio_out/ (Windows: %TEMP%\scacelith_audio_out).
#include "test.h"
#include "audio/audio.h"
#include "audio/mixer.h"
#include "audio/offline.h"
#include "audio/queue.h"
#include "audio/reverb.h"
#include "audio/synth.h"
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <time.h>
#endif

namespace {

constexpr float kFs = 48000.0f;
constexpr float kMinus1dB = 0.8912509f;

void makeDir(const char* p) {
#ifdef _WIN32
    _mkdir(p);
#else
    mkdir(p, 0755);
#endif
}

std::string outDir() {
#ifdef _WIN32
    // %TEMP%\scacelith_audio_out (never the working directory, which is usually the repo root).
    char tmp[MAX_PATH + 1] = {};
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    std::string d = (n > 0 && n <= MAX_PATH) ? std::string(tmp) : std::string(".\\");
    d += "scacelith_audio_out";
#else
    std::string d = "/tmp/audio_out";
#endif
    makeDir(d.c_str());
    return d;
}

double threadCpuSeconds() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0.0;
    auto f = [](FILETIME t) { return double((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 1e-7; };
    return f(k) + f(u);
#else
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
#endif
}

// Magnitude-weighted spectral centroid of a mono signal (Hz).
float spectralCentroid(const std::vector<float>& x) {
    size_t n = 1;
    while (n < x.size() && n < (1u << 18)) n <<= 1;
    std::vector<std::complex<float>> a(n);
    for (size_t i = 0; i < n && i < x.size(); ++i) {
        float w = 0.5f - 0.5f * std::cos(6.2831853f * float(i) / float(std::min(n, x.size())));  // Hann over the data
        a[i] = x[i] * (x.size() > 4096 ? 1.0f : w);
    }
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        float ang = -6.2831853f / float(len);
        std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t j = 0; j < len / 2; ++j) {
                std::complex<float> u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    double num = 0.0, den = 0.0;
    for (size_t k = 1; k < n / 2; ++k) {
        double f = double(k) * kFs / double(n);
        double mag = std::abs(a[k]);
        num += f * mag;
        den += mag;
    }
    return den > 0.0 ? float(num / den) : 0.0f;
}

struct Analysis {
    float peak = 0, rms = 0, dcL = 0, dcR = 0, centroid = 0, duration = 0;
    bool finite = true;
    int denormals = 0;
};

// Interleaved stereo analysis. duration = time until the (10 ms RMS) level stays below -60 dB
// relative to the loudest 10 ms window.
Analysis analyzeStereo(const std::vector<float>& s) {
    Analysis a;
    size_t frames = s.size() / 2;
    double sum2 = 0.0, sL = 0.0, sR = 0.0;
    std::vector<float> mono(frames);
    for (size_t i = 0; i < frames; ++i) {
        float l = s[2 * i], r = s[2 * i + 1];
        if (!std::isfinite(l) || !std::isfinite(r)) a.finite = false;
        if ((l != 0.0f && std::fabs(l) < FLT_MIN) || (r != 0.0f && std::fabs(r) < FLT_MIN)) ++a.denormals;
        a.peak = std::max(a.peak, std::max(std::fabs(l), std::fabs(r)));
        sum2 += double(l) * l + double(r) * r;
        sL += l;
        sR += r;
        mono[i] = 0.5f * (l + r);
    }
    if (frames) {
        a.rms = float(std::sqrt(sum2 / double(2 * frames)));
        a.dcL = float(sL / double(frames));
        a.dcR = float(sR / double(frames));
    }
    a.centroid = spectralCentroid(mono);
    const size_t win = 480;
    std::vector<float> lv;
    float maxLv = 0.0f;
    for (size_t i = 0; i + win <= frames; i += win) {
        double e = 0.0;
        for (size_t k = 0; k < win; ++k) e += double(mono[i + k]) * mono[i + k];
        lv.push_back(float(std::sqrt(e / win)));
        maxLv = std::max(maxLv, lv.back());
    }
    size_t last = 0;
    for (size_t i = 0; i < lv.size(); ++i)
        if (lv[i] > maxLv * 1e-3f) last = i + 1;
    a.duration = float(last * win) / kFs;
    return a;
}

float db(float g) { return 20.0f * std::log10(std::max(g, 1e-9f)); }

// Clicks/discontinuities: largest |HP 12 kHz (4th order)| relative to the RMS of the signal.
float clickRatio(const std::vector<float>& s) {
    audio::dsp::Svf a, b;
    a.set(12000.0f, 0.707f, kFs);
    b.set(12000.0f, 0.707f, kFs);
    float mx = 0.0f;
    double e = 0.0;
    for (size_t i = 0; i < s.size() / 2; ++i) {
        float x = 0.5f * (s[2 * i] + s[2 * i + 1]);
        e += double(x) * x;
        mx = std::max(mx, std::fabs(b.hp(a.hp(x))));
    }
    float rms = float(std::sqrt(e / double(std::max<size_t>(1, s.size() / 2))));
    return mx / std::max(rms, 1e-12f);
}

void installAllSounds(audio::Mixer& m, uint32_t seed) {
    for (int s = 0; s < int(audio::Sfx::Count); ++s)
        for (int v = 0; v < audio::bankVariants(audio::Sfx(s)); ++v) {
            audio::SoundBuffer* b = new audio::SoundBuffer();
            b->samples = audio::synthesize(audio::Sfx(s), seed * 131u + uint32_t(s * 17 + v));
            b->sfx = s;
            b->variant = v;
            m.install(b);
        }
}

struct Expect {
    audio::Sfx sfx;
    float seconds;         // render length (tail included)
    float minCentroid, maxCentroid;
    float minDur, maxDur;  // dry synthesis duration (s) at -60 dB
};
const Expect kExpect[] = {
    {audio::Sfx::PiecePickup, 1.2f, 1500.0f, 6000.0f, 0.03f, 0.5f},
    {audio::Sfx::PiecePlace, 1.5f, 400.0f, 2200.0f, 0.04f, 0.5f},
    {audio::Sfx::Capture, 2.5f, 900.0f, 6000.0f, 0.6f, 1.4f},
    {audio::Sfx::ClockPress, 1.5f, 900.0f, 5000.0f, 0.04f, 0.4f},
    {audio::Sfx::Handshake, 2.5f, 200.0f, 2500.0f, 0.5f, 1.6f},
    {audio::Sfx::ServoShort, 1.5f, 250.0f, 2500.0f, 0.2f, 0.9f},
    {audio::Sfx::ChairCreak, 2.0f, 250.0f, 2500.0f, 0.3f, 1.2f},
    {audio::Sfx::UIHover, 0.6f, 900.0f, 4000.0f, 0.005f, 0.08f},
    {audio::Sfx::UIClick, 0.8f, 600.0f, 3500.0f, 0.01f, 0.13f},
    {audio::Sfx::GameStart, 6.0f, 150.0f, 900.0f, 1.5f, 4.5f},
    {audio::Sfx::GameEnd, 7.0f, 120.0f, 800.0f, 2.0f, 5.5f},
    {audio::Sfx::CaptureClick, 1.5f, 1500.0f, 7000.0f, 0.05f, 0.45f},
    {audio::Sfx::TablePlace, 1.5f, 300.0f, 1800.0f, 0.04f, 0.5f},
    {audio::Sfx::PenWrite, 4.0f, 1500.0f, 7000.0f, 2.8f, 3.45f},
    {audio::Sfx::PenTap, 0.8f, 500.0f, 6000.0f, 0.02f, 0.13f},
    {audio::Sfx::PageTurn, 2.0f, 900.0f, 6000.0f, 0.8f, 1.35f},
    {audio::Sfx::PageFlap, 1.2f, 200.0f, 3500.0f, 0.08f, 0.45f},
};
static_assert(sizeof(kExpect) / sizeof(kExpect[0]) == size_t(audio::Sfx::Count), "every sound has expectations");

}  // namespace

TEST(audio_synth_variants_sane) {
    for (const Expect& ex : kExpect) {
        std::vector<float> prev;
        for (uint32_t seed = 1; seed <= 5; ++seed) {
            std::vector<float> v = audio::synthesize(ex.sfx, seed * 7777u);
            CHECK(v.size() > 64);
            float peak = 0.0f;
            bool finite = true;
            int den = 0;
            for (float x : v) {
                finite = finite && std::isfinite(x);
                if (x != 0.0f && std::fabs(x) < FLT_MIN) ++den;
                peak = std::max(peak, std::fabs(x));
            }
            CHECK(finite);
            CHECK_EQ(den, 0);
            CHECK(std::fabs(peak - 1.0f) < 1e-3f);
            CHECK(std::fabs(v.front()) < 0.05f);           // no click at the start
            CHECK(std::fabs(v.back()) < 1e-3f);            // faded tail
            {                                              // not truncated: last 20 ms < -45 dB
                size_t n20 = std::min(v.size(), size_t(0.02f * kFs));
                double e = 0.0;
                for (size_t i = v.size() - n20; i < v.size(); ++i) e += double(v[i]) * v[i];
                float lastDb = 10.0f * std::log10(float(e / double(n20)) + 1e-20f);
                if (lastDb > -45.0f) {
                    std::fprintf(stderr, "  %s seed %u: tail truncated (last 20 ms at %.1f dB)\n", audio::sfxName(ex.sfx), seed, lastDb);
                    CHECK(false);
                }
            }
            float dur = float(v.size()) / kFs;
            if (dur < ex.minDur || dur > ex.maxDur + 0.01f) {
                std::fprintf(stderr, "  %s seed %u: duration %.3f s outside [%.3f, %.3f]\n", audio::sfxName(ex.sfx), seed, dur,
                             ex.minDur, ex.maxDur);
                CHECK(false);
            }
            if (!prev.empty()) {  // every variant differs
                size_t n = std::min(prev.size(), v.size());
                double dotp = 0, e1 = 0, e2 = 0;
                for (size_t i = 0; i < n; ++i) {
                    dotp += double(prev[i]) * v[i];
                    e1 += double(prev[i]) * prev[i];
                    e2 += double(v[i]) * v[i];
                }
                double corr = dotp / std::sqrt(e1 * e2 + 1e-30);
                CHECK(corr < 0.995);
            }
            prev = v;
        }
    }
}

TEST(audio_render_all_wavs_and_check) {
    std::string dir = outDir();
    std::fprintf(stderr, "  %-14s %8s %8s %9s %9s %9s %8s\n", "sound", "peak dB", "rms dB", "DC", "centroid", "dur(s)", "limGR dB");
    for (const Expect& ex : kExpect) {
        audio::OfflineStats st;
        std::vector<float> buf = audio::renderSfxOffline(ex.sfx, ex.seconds, 1u, &st);
        Analysis a = analyzeStereo(buf);
        std::fprintf(stderr, "  %-14s %8.2f %8.2f %9.2e %9.0f %9.3f %8.2f\n", audio::sfxName(ex.sfx), db(a.peak), db(a.rms),
                     std::max(std::fabs(a.dcL), std::fabs(a.dcR)), a.centroid, a.duration, db(st.limiterMinGain));
        CHECK(a.finite);
        CHECK_EQ(a.denormals, 0);
        CHECK(a.peak <= kMinus1dB);
        CHECK(a.peak > 0.003f);                                  // audible (> -50 dBFS)
        CHECK(std::fabs(a.dcL) < 2e-4f && std::fabs(a.dcR) < 2e-4f);
        CHECK(st.limiterMinGain > 0.7f);                         // levels are designed, not limited
        if (a.centroid < ex.minCentroid || a.centroid > ex.maxCentroid) {
            std::fprintf(stderr, "  %s: centroid %.0f Hz outside [%.0f, %.0f]\n", audio::sfxName(ex.sfx), a.centroid,
                         ex.minCentroid, ex.maxCentroid);
            CHECK(false);
        }
        std::string path = dir + "/" + audio::sfxName(ex.sfx) + ".wav";
        CHECK(audio::writeWav16(path.c_str(), buf.data(), buf.size() / 2, 2, 48000));
    }
    // Public API path (same content as above for seed 1).
    std::string p = dir + "/piece_place_api.wav";
    CHECK(audio::renderToWav(audio::Sfx::PiecePlace, p.c_str(), 1.5f));
    struct stat sb {};
    CHECK(stat(p.c_str(), &sb) == 0 && sb.st_size == 44 + 48000 * 3 / 2 * 4);
    std::remove(p.c_str());
}

TEST(audio_ambience_30s) {
    std::string dir = outDir();
    audio::OfflineStats st;
    std::vector<float> buf = audio::renderAmbienceOffline(30.0f, 1u, &st);
    Analysis a = analyzeStereo(buf);
    std::fprintf(stderr, "  ambience 30 s: peak %.2f dBFS, rms %.2f dBFS, DC %.1e, centroid %.0f Hz\n", db(a.peak), db(a.rms),
                 std::max(std::fabs(a.dcL), std::fabs(a.dcR)), a.centroid);
    CHECK(a.finite);
    CHECK_EQ(a.denormals, 0);
    CHECK(a.peak <= kMinus1dB);
    CHECK(db(a.rms) > -62.0f && db(a.rms) < -36.0f);  // very low level room tone
    CHECK(a.centroid > 80.0f && a.centroid < 2500.0f);
    CHECK(std::fabs(a.dcL) < 2e-4f && std::fabs(a.dcR) < 2e-4f);
    // No audible loop: the 10-20 s segment is not a copy of the 0-10 s one.
    size_t seg = size_t(10 * kFs) * 2;
    double dotp = 0, e1 = 0, e2 = 0;
    for (size_t i = 0; i < seg; ++i) {
        dotp += double(buf[i]) * buf[seg + i];
        e1 += double(buf[i]) * buf[i];
        e2 += double(buf[seg + i]) * buf[seg + i];
    }
    CHECK(std::fabs(dotp / std::sqrt(e1 * e2 + 1e-30)) < 0.2);
    // Slow level variation (breathing) exists: 1 s RMS spread over the file.
    float lo = 1e9f, hi = 0.0f;
    for (size_t s = 1; s < 29; ++s) {
        double e = 0;
        for (size_t i = size_t(s * kFs) * 2; i < size_t((s + 1) * kFs) * 2; ++i) e += double(buf[i]) * buf[i];
        float r = float(std::sqrt(e / (2.0 * kFs)));
        lo = std::min(lo, r);
        hi = std::max(hi, r);
    }
    std::fprintf(stderr, "  ambience 1 s RMS range: %.1f .. %.1f dBFS\n", db(lo), db(hi));
    CHECK(db(hi) - db(lo) > 1.0f);
    CHECK(st.limiterMinGain > 0.9f);
    float clicks = clickRatio(buf);
    std::fprintf(stderr, "  ambience click ratio (max |HP12k| / RMS): %.2f\n", clicks);
    CHECK(clicks < 1.0f);
    std::string path = dir + "/ambience_30s.wav";
    CHECK(audio::writeWav16(path.c_str(), buf.data(), buf.size() / 2, 2, 48000));
    CHECK(audio::renderAmbienceToWav((dir + "/ambience_api_check.wav").c_str(), 1.0f));
    std::remove((dir + "/ambience_api_check.wav").c_str());
}

TEST(audio_reverb_rt60_and_predelay) {
    audio::HallReverb rv;
    rv.prepare(kFs);
    const int n = int(5.0f * kFs);
    std::vector<float> in(size_t(n), 0.0f), L(size_t(n), 0.0f), R(size_t(n), 0.0f);
    in[0] = 1.0f;
    rv.earlyGain = 0.0f;
    rv.process(in.data(), L.data(), R.data(), n);
    // Energy decay curve (Schroeder backward integration) on L+R.
    auto rt60 = [&](const std::vector<float>& l, const std::vector<float>& r) {
        std::vector<double> edc(size_t(n) + 1, 0.0);
        for (int i = n - 1; i >= 0; --i) edc[size_t(i)] = edc[size_t(i) + 1] + double(l[size_t(i)]) * l[size_t(i)] + double(r[size_t(i)]) * r[size_t(i)];
        double e0 = edc[0];
        int i5 = -1, i35 = -1;
        for (int i = 0; i < n; ++i) {
            double d = 10.0 * std::log10(edc[size_t(i)] / e0 + 1e-30);
            if (i5 < 0 && d <= -5.0) i5 = i;
            if (i35 < 0 && d <= -35.0) { i35 = i; break; }
        }
        if (i5 < 0 || i35 < 0) return 99.0f;
        return float(i35 - i5) / kFs * 2.0f;  // 30 dB span -> 60 dB
    };
    double energy = 0.0;
    int onset = -1;
    for (int i = 0; i < n; ++i) {
        energy += double(L[size_t(i)]) * L[size_t(i)] + double(R[size_t(i)]) * R[size_t(i)];
        if (onset < 0 && (std::fabs(L[size_t(i)]) > 1e-4f || std::fabs(R[size_t(i)]) > 1e-4f)) onset = i;
    }
    float t60 = rt60(L, R);
    // Band-split: high band (> 5 kHz) must decay faster than the low band (< 1 kHz).
    std::vector<float> hl(L), hr(R), ll(L), lr(R);
    audio::dsp::Svf f1, f2, f3, f4;
    f1.set(5000, 0.7f, kFs); f2.set(5000, 0.7f, kFs); f3.set(1000, 0.7f, kFs); f4.set(1000, 0.7f, kFs);
    for (int i = 0; i < n; ++i) {
        hl[size_t(i)] = f1.hp(L[size_t(i)]);
        hr[size_t(i)] = f2.hp(R[size_t(i)]);
        ll[size_t(i)] = f3.lp(L[size_t(i)]);
        lr[size_t(i)] = f4.lp(R[size_t(i)]);
    }
    float tHigh = rt60(hl, hr), tLow = rt60(ll, lr);
    // Stereo decorrelation of the tail.
    double lr2 = 0, l2 = 0, r2 = 0;
    for (int i = int(0.1f * kFs); i < n; ++i) {
        lr2 += double(L[size_t(i)]) * R[size_t(i)];
        l2 += double(L[size_t(i)]) * L[size_t(i)];
        r2 += double(R[size_t(i)]) * R[size_t(i)];
    }
    float corr = float(lr2 / std::sqrt(l2 * r2 + 1e-30));
    std::fprintf(stderr, "  RT60 %.2f s (low %.2f s, high %.2f s), onset %.1f ms, IR energy %.3f, L/R corr %.2f\n", t60, tLow,
                 tHigh, onset * 1000.0f / kFs, energy, corr);
    CHECK(t60 > 2.0f && t60 < 2.7f);
    CHECK(tHigh < tLow * 0.8f);
    CHECK(onset >= int(0.018f * kFs) && onset <= int(0.032f * kFs));
    CHECK(energy > 0.5 && energy < 2.0);
    CHECK(std::fabs(corr) < 0.3f);
    for (float x : L) CHECK(std::isfinite(x));
}

TEST(audio_spatial_pan_itd_behind_distance) {
    using namespace audio;
    ListenerPose lis;
    lis.pos = m::vec3(0, 1.2f, 0);
    lis.fwd = m::vec3(0, 0, -1);
    lis.up = m::vec3(0, 1, 0);
    auto energy = [](const std::vector<float>& b, int ch) {
        double e = 0;
        for (size_t i = ch; i < b.size(); i += 2) e += double(b[i]) * b[i];
        return e;
    };
    // Source 0.5 m to the right: right louder, left ear delayed by the ITD.
    std::vector<float> right = renderSfxOfflineAt(Sfx::PiecePlace, 0.5f, m::vec3(0.5f, 1.2f, 0.0f), lis, 3u, false);
    double eL = energy(right, 0), eR = energy(right, 1);
    int bestLag = 0;
    double best = -1e30;
    for (int lag = -40; lag <= 40; ++lag) {
        double s = 0;
        for (size_t i = 100; i + 100 < right.size() / 2; ++i) {
            long j = long(i) + lag;
            s += double(right[2 * i + 1]) * right[size_t(2 * j)];
        }
        if (s > best) { best = s; bestLag = lag; }
    }
    std::fprintf(stderr, "  right source: R/L %.1f dB, left-ear lag %d samples\n", 10.0 * std::log10(eR / eL), bestLag);
    CHECK(10.0 * std::log10(eR / eL) > 6.0);
    CHECK(bestLag >= 20 && bestLag <= 36);  // ~0.66 ms at 48 kHz
    // Front vs behind: behind is darker.
    std::vector<float> front = renderSfxOfflineAt(Sfx::CaptureClick, 0.5f, m::vec3(0, 1.2f, -0.6f), lis, 3u, false);
    std::vector<float> back = renderSfxOfflineAt(Sfx::CaptureClick, 0.5f, m::vec3(0, 1.2f, 0.6f), lis, 3u, false);
    std::vector<float> fm(front.size() / 2), bm(back.size() / 2);
    for (size_t i = 0; i < fm.size(); ++i) { fm[i] = front[2 * i]; bm[i] = back[2 * i]; }
    float cf = spectralCentroid(fm), cb = spectralCentroid(bm);
    std::fprintf(stderr, "  centroid front %.0f Hz, behind %.0f Hz\n", cf, cb);
    CHECK(cb < cf * 0.85f);
    // Inverse distance: 0.3 m vs 1.2 m -> ~12 dB; below 0.15 m no further boost.
    auto level = [&](float d) {
        std::vector<float> b = renderSfxOfflineAt(Sfx::PiecePlace, 0.4f, m::vec3(0, 1.2f, -d), lis, 3u, false);
        return 10.0 * std::log10(energy(b, 0) + energy(b, 1));
    };
    double l03 = level(0.3f), l12 = level(1.2f), l01 = level(0.1f), l015 = level(0.15f);
    std::fprintf(stderr, "  distance: 0.3 m vs 1.2 m %.1f dB, 0.1 m vs 0.15 m %.1f dB\n", l03 - l12, l01 - l015);
    CHECK(std::fabs((l03 - l12) - 12.04) < 1.0);
    CHECK(std::fabs(l01 - l015) < 0.5);
}

TEST(audio_repeated_triggers_differ) {
    using namespace audio;
    Mixer m(42u);
    m.prepare(kFs);
    m.setAmbienceEnabled(false, true);
    m.setRoomEnabled(false);
    for (int v = 0; v < kVariants; ++v) {
        SoundBuffer* b = new SoundBuffer();
        b->samples = synthesize(Sfx::PiecePlace, 100u + uint32_t(v));
        b->sfx = int(Sfx::PiecePlace);
        b->variant = v;
        m.install(b);
    }
    PlayRequest r;
    r.sfx = Sfx::PiecePlace;
    r.pos = m::vec3(0.0275f, 0.782f, 0.0275f);  // e4
    std::vector<std::vector<float>> takes;
    for (int k = 0; k < 4; ++k) {
        m.play(r);
        std::vector<float> out(size_t(0.4f * kFs) * 2);
        m.process(out.data(), int(out.size() / 2));
        takes.push_back(out);
    }
    for (size_t a = 0; a < takes.size(); ++a)
        for (size_t b = a + 1; b < takes.size(); ++b) {
            double dotp = 0, e1 = 0, e2 = 0;
            for (size_t i = 0; i < takes[a].size(); ++i) {
                dotp += double(takes[a][i]) * takes[b][i];
                e1 += double(takes[a][i]) * takes[a][i];
                e2 += double(takes[b][i]) * takes[b][i];
            }
            CHECK(dotp / std::sqrt(e1 * e2 + 1e-30) < 0.99);
        }
}

// Windowed playback (pen strokes): only 'duration' seconds sound, with click-free edges, and the
// voice is released at the end of the window.
TEST(audio_windowed_play) {
    using namespace audio;
    Mixer m(7u);
    m.prepare(kFs);
    m.setAmbienceEnabled(false, true);
    m.setRoomEnabled(false);
    for (int v = 0; v < bankVariants(Sfx::PenWrite); ++v) {
        SoundBuffer* b = new SoundBuffer();
        b->samples = synthesize(Sfx::PenWrite, 300u + uint32_t(v));
        b->sfx = int(Sfx::PenWrite);
        b->variant = v;
        m.install(b);
    }
    for (float dur : {0.05f, 0.18f, 0.6f}) {
        PlayRequest r;
        r.sfx = Sfx::PenWrite;
        r.pos = defaultPosition(Sfx::PenWrite);
        r.duration = dur;
        CHECK(m.play(r));
        std::vector<float> out(size_t(1.0f * kFs) * 2);
        m.process(out.data(), int(out.size() / 2));
        CHECK_EQ(m.activeVoices(), 0);
        size_t lastLoud = 0;
        float peak = 0.0f, first = std::fabs(out[0]) + std::fabs(out[1]);
        for (size_t i = 0; i < out.size() / 2; ++i) {
            float a = std::max(std::fabs(out[2 * i]), std::fabs(out[2 * i + 1]));
            peak = std::max(peak, a);
            if (a > 1e-5f) lastLoud = i;
        }
        float heard = float(lastLoud) / kFs;
        std::fprintf(stderr, "  pen stroke %.2f s: heard %.3f s, peak %.1f dBFS\n", dur, heard, 20.0f * std::log10(peak + 1e-12f));
        CHECK(peak > 1e-3f);
        CHECK(first < 0.02f * peak);                                    // faded in
        CHECK(heard > dur * 0.85f && heard < dur * 1.1f + 0.003f);    // pitch jitter +-4 %
    }
}

TEST(audio_ambience_toggle_fades) {
    using namespace audio;
    Mixer m(7u);
    m.prepare(kFs);
    m.setVolumes(1, 1, 1);
    m.setAmbienceEnabled(true, true);
    std::vector<float> out(size_t(2.0f * kFs) * 2);
    m.process(out.data(), int(out.size() / 2));
    m.setAmbienceEnabled(false);
    std::vector<float> fade(size_t(4.0f * kFs) * 2);
    m.process(fade.data(), int(fade.size() / 2));
    auto rms = [](const std::vector<float>& b, size_t from, size_t to) {
        double e = 0;
        for (size_t i = from * 2; i < to * 2; ++i) e += double(b[i]) * b[i];
        return std::sqrt(e / double(2 * (to - from)));
    };
    double before = rms(out, size_t(1.5f * kFs), size_t(2.0f * kFs));
    double mid = rms(fade, size_t(0.6f * kFs), size_t(0.8f * kFs));
    double after = rms(fade, size_t(3.5f * kFs), size_t(4.0f * kFs));
    std::fprintf(stderr, "  ambience toggle: %.1f -> %.1f (0.7 s) -> %.1f dBFS (3.5 s)\n", db(float(before)), db(float(mid)),
                 db(float(after)));
    CHECK(mid < before && mid > before * 0.05);  // gradual, not a cut
    CHECK(db(float(after)) < db(float(before)) - 30.0f);
}

TEST(audio_queue_mpmc_stress) {
    audio::MpmcQueue<uint32_t, 256> q;
    std::atomic<bool> done{false};
    std::atomic<uint64_t> produced{0};
    const int kProducers = 4, kPerProducer = 20000;
    uint64_t consumedSum = 0, consumedCount = 0;
    std::thread consumer([&] {
        uint32_t v;
        for (;;) {
            if (q.pop(v)) {
                consumedSum += v;
                ++consumedCount;
            } else if (done.load()) {
                while (q.pop(v)) { consumedSum += v; ++consumedCount; }
                break;
            }
        }
    });
    std::vector<std::thread> ps;
    for (int p = 0; p < kProducers; ++p)
        ps.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                uint32_t v = uint32_t(p * kPerProducer + i + 1);
                while (!q.push(v)) std::this_thread::yield();
                produced.fetch_add(v);
            }
        });
    for (auto& t : ps) t.join();
    done.store(true);
    consumer.join();
    CHECK_EQ(consumedCount, uint64_t(kProducers * kPerProducer));
    CHECK_EQ(consumedSum, produced.load());
}

TEST(audio_mixer_cpu_cost) {
    using namespace audio;
    Mixer m(9u);
    m.prepare(kFs);
    m.setVolumes(0.9f, 1.0f, 0.7f);
    m.setAmbienceEnabled(true, true);
    const Sfx game[] = {Sfx::PiecePickup, Sfx::PiecePlace, Sfx::ClockPress, Sfx::ServoShort, Sfx::Capture};
    for (Sfx s : game)
        for (int v = 0; v < kVariants; ++v) {
            SoundBuffer* b = new SoundBuffer();
            b->samples = synthesize(s, uint32_t(v) + 1u);
            b->sfx = int(s);
            b->variant = v;
            m.install(b);
        }
    for (int v = 0; v < kVariants; ++v) {
        SoundBuffer* b = new SoundBuffer();
        b->samples = synthesize(Sfx::ChairCreak, uint32_t(v) + 1u);
        b->sfx = int(Sfx::ChairCreak);
        b->variant = v;
        m.install(b);
    }
    // Typical load: ambience + a burst of game sounds every 0.5 s; plus a stress phase with 32 voices.
    const float seconds = 20.0f;
    const int block = 480;
    std::vector<float> out(size_t(block) * 2);
    int blocks = int(seconds * kFs) / block;
    double c0 = threadCpuSeconds();
    auto w0 = std::chrono::steady_clock::now();
    int maxVoices = 0;
    for (int b = 0; b < blocks; ++b) {
        if (b % 50 == 0) {
            PlayRequest r;
            r.sfx = game[(b / 50) % 5];
            r.pos = m::vec3(0.05f * float(b % 7), 0.78f, 0.1f);
            m.play(r);
        }
        m.process(out.data(), block);
        maxVoices = std::max(maxVoices, m.activeVoices());
    }
    double cpu = threadCpuSeconds() - c0;
    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    if (cpu <= 0.0) cpu = wall;
    float typical = float(cpu / seconds);
    // Stress: 32 simultaneous voices.
    for (int k = 0; k < kMaxVoices; ++k) {
        PlayRequest r;
        r.sfx = Sfx::Capture;
        r.pos = m::vec3(0.02f * float(k), 0.8f, 0.0f);
        m.play(r);
    }
    int stressBlocks = int(0.5f * kFs) / block;
    double s0 = threadCpuSeconds();
    auto sw0 = std::chrono::steady_clock::now();
    for (int b = 0; b < stressBlocks; ++b) m.process(out.data(), block);
    double scpu = threadCpuSeconds() - s0;
    if (scpu <= 0.0) scpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - sw0).count();
    float stress = float(scpu / (stressBlocks * block / kFs));
    std::fprintf(stderr, "  mixer CPU: typical %.3f %% of one core (ambience + game sounds, max %d voices), 32 voices %.2f %%\n",
                 typical * 100.0f, maxVoices, stress * 100.0f);
    CHECK(typical < 0.02f);
    CHECK(stress < 0.10f);
}

TEST(audio_live_engine_init_shutdown) {
    audio::setMasterVolume(0.9f);
    audio::setAmbienceEnabled(true);
    bool ok = audio::init();
    audio::Stats st = audio::stats();
    std::fprintf(stderr, "  init() = %s, device %s, %d Hz, buffer %d frames\n", ok ? "true" : "false",
                 st.deviceOpen ? "open" : "none", st.sampleRate, st.bufferFrames);
    CHECK(st.running);
#ifndef _WIN32
    CHECK(ok);  // null backend always runs
#endif
    audio::setListener(m::vec3(0, 1.23f, 0.62f), m::vec3(0, -0.6f, -0.8f), m::vec3(0, 1, 0));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));  // bank synthesis
    audio::debugTakeOutputPeak();
    for (int i = 0; i < 6; ++i) {
        audio::play(audio::Sfx::PiecePlace, m::vec3(0.02f * float(i), 0.78f, 0.0f), 1.0f, 1.0f);
        audio::playUI(audio::Sfx::UIClick);
        audio::play(audio::Sfx(99), m::vec3(0, 0, 0));                  // invalid: ignored
        audio::play(audio::Sfx::PiecePlace, m::vec3(NAN, 0, 0));        // invalid: ignored
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    float peak = audio::debugTakeOutputPeak();
    st = audio::stats();
    std::fprintf(stderr, "  live: output peak %.1f dBFS, cpu %.3f %%, voices %d, underruns %u\n", db(peak), st.cpuLoad * 100.0f,
                 st.activeVoices, st.underruns);
    std::fprintf(stderr, "  bank variants re-synthesised after playing: %u\n", audio::debugBankRefreshCount());
    if (ok) {
        CHECK(peak > 0.001f);
        CHECK(peak <= kMinus1dB);
        CHECK(audio::debugBankRefreshCount() > 0u);
    }
    audio::setAmbienceEnabled(false);
    audio::setEffectsVolume(0.5f);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    audio::shutdown();
    CHECK(!audio::stats().running);
    // Calls after shutdown are harmless; init/shutdown can be repeated.
    audio::play(audio::Sfx::PiecePlace, m::vec3(0, 0.78f, 0));
    bool ok2 = audio::init();
    CHECK_EQ(ok2, ok);
    audio::playUI(audio::Sfx::UIHover);
    audio::shutdown();
    audio::shutdown();
    audio::setAmbienceEnabled(true);
    audio::setEffectsVolume(1.0f);
}

// A 26 s "filmed scene" through the full chain: ambience, seated shifts, handshake, the game
// start tone, moves with pickups/places/servos/clock presses, a capture, game end, menu clicks.
TEST(audio_scene_demo_wav) {
    using namespace audio;
    Mixer m(2024u);
    m.prepare(kFs);
    m.setVolumes(0.9f, 1.0f, 0.7f);  // default settings
    m.setAmbienceEnabled(true, true);
    m.setListener(whiteSeatListener());
    installAllSounds(m, 5u);
    auto sq = [](int f, int r) { return m::vec3((float(f) - 3.5f) * 0.055f, 0.782f, (3.5f - float(r)) * 0.055f); };
    const m::vec3 clock(0.405f, 0.825f, 0.0f), ownArm(0.22f, 1.05f, 0.45f), oppArm(-0.2f, 1.1f, -0.5f);
    const m::vec3 oppChair(0.0f, 0.46f, -0.66f), ownChair(0.0f, 0.46f, 0.66f), hands(0.0f, 1.0f, 0.0f);
    struct Ev { float t; Sfx s; m::vec3 p; float gain, pitch; bool ui; };
    const Ev evs[] = {
        {0.8f, Sfx::ChairCreak, oppChair, 0.7f, 1.0f, false},
        {1.4f, Sfx::ServoShort, oppArm, 1.0f, 1.0f, false},
        {1.5f, Sfx::ServoShort, ownArm, 1.0f, 1.05f, false},
        {2.0f, Sfx::Handshake, hands, 1.0f, 1.0f, false},
        {3.6f, Sfx::GameStart, {}, 1.0f, 1.0f, true},
        {5.0f, Sfx::ServoShort, ownArm, 1.0f, 1.0f, false},
        {5.4f, Sfx::PiecePickup, sq(4, 1), 1.0f, 1.05f, false},   // e2 pawn
        {6.3f, Sfx::PiecePlace, sq(4, 3), 1.0f, 1.05f, false},    // e4
        {7.1f, Sfx::ClockPress, clock, 1.0f, 1.0f, false},
        {9.2f, Sfx::ServoShort, oppArm, 1.0f, 0.95f, false},
        {9.6f, Sfx::PiecePickup, sq(4, 6), 1.0f, 1.05f, false},   // e7
        {10.5f, Sfx::PiecePlace, sq(4, 4), 1.0f, 1.05f, false},   // e5
        {11.3f, Sfx::ClockPress, clock, 1.0f, 0.97f, false},
        {12.2f, Sfx::ChairCreak, ownChair, 0.5f, 1.1f, false},
        {13.4f, Sfx::PiecePickup, sq(5, 2), 1.0f, 1.0f, false},   // Nf3
        {14.2f, Sfx::CaptureClick, sq(4, 4), 1.0f, 1.0f, false},  // Nxe5: the e5 pawn is taken
        {15.1f, Sfx::TablePlace, m::vec3(-0.30f, 0.76f, 0.30f), 1.0f, 1.05f, false},
        {15.9f, Sfx::PiecePlace, sq(4, 4), 1.0f, 0.97f, false},
        {16.6f, Sfx::ClockPress, clock, 1.0f, 1.0f, false},
        {18.6f, Sfx::PiecePickup, sq(3, 6), 1.0f, 1.05f, false},  // d7
        {19.4f, Sfx::PiecePlace, sq(3, 5), 1.0f, 1.05f, false},   // d6
        {20.1f, Sfx::ClockPress, clock, 1.0f, 1.03f, false},
        {21.5f, Sfx::GameEnd, {}, 1.0f, 1.0f, true},
        {24.6f, Sfx::UIHover, {}, 1.0f, 1.0f, true},
        {25.0f, Sfx::UIClick, {}, 1.0f, 1.0f, true},
    };
    const float seconds = 26.0f;
    std::vector<float> out(size_t(seconds * kFs) * 2);
    size_t next = 0, frame = 0, total = out.size() / 2;
    while (frame < total) {
        while (next < sizeof(evs) / sizeof(evs[0]) && size_t(evs[next].t * kFs) <= frame) {
            PlayRequest r;
            r.sfx = evs[next].s;
            r.pos = evs[next].p;
            r.gain = evs[next].gain;
            r.pitch = evs[next].pitch;
            r.spatial = !evs[next].ui;
            r.bus = evs[next].ui ? Bus::UI : Bus::Effects;
            CHECK(m.play(r));
            ++next;
        }
        int n = int(std::min<size_t>(240, total - frame));
        m.process(out.data() + 2 * frame, n);
        frame += size_t(n);
    }
    Analysis a = analyzeStereo(out);
    std::fprintf(stderr, "  scene demo: peak %.2f dBFS, rms %.2f dBFS, limiter min gain %.2f dB\n", db(a.peak), db(a.rms),
                 db(m.takeLimiterMinGain()));
    CHECK(a.finite);
    CHECK_EQ(a.denormals, 0);
    CHECK(a.peak <= kMinus1dB);
    std::string path = outDir() + "/scene_demo_26s.wav";
    CHECK(writeWav16(path.c_str(), out.data(), out.size() / 2, 2, 48000));
}
