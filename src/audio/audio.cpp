// Public audio API: engine lifetime, game-thread -> mixer command queue, bank builder thread,
// offline rendering and WAV output.
//
// Threads:
//   * game thread(s): play()/setListener()/volumes -> lock-free MPMC queue / atomics
//   * audio thread (backend): drains commands, installs bank buffers, runs the Mixer
//   * builder thread (low priority): synthesises the initial bank (kVariants per Sfx), then
//     re-synthesises each variant right after it is played (fresh seed), and frees retired
//     buffers so the audio thread never allocates or frees.
#include "audio.h"
#include "backend.h"
#include "mixer.h"
#include "offline.h"
#include "queue.h"
#include "synth.h"
#include "../core/log.h"
#include "../game/layout.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace audio {
namespace {

using Clock = std::chrono::steady_clock;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

struct Command {
    PlayRequest req;
    int64_t stampMs = 0;
};
struct RefreshReq {
    int sfx = 0, variant = 0;
};

// Settings survive init()/shutdown() and may be set before init().
std::atomic<float> g_master{0.9f}, g_effects{1.0f}, g_ambienceVol{0.7f};
std::atomic<bool> g_ambienceOn{true};
std::mutex g_listenerMutex;
ListenerPose g_listener;
std::atomic<uint32_t> g_listenerVersion{1};
std::atomic<float> g_outPeak{0.0f};
std::atomic<unsigned> g_refreshCount{0};

struct Engine {
    MpmcQueue<Command, 512> commands;
    MpmcQueue<SoundBuffer*, 256> incoming;   // builder -> audio thread
    MpmcQueue<SoundBuffer*, 1024> graveyard; // audio thread -> builder (free)
    MpmcQueue<RefreshReq, 256> refresh;      // audio thread -> builder
    std::unique_ptr<Mixer> mixer;
    std::unique_ptr<Backend> backend;
    std::thread builder;
    std::atomic<bool> quit{false};
    std::mutex wakeMutex;
    std::condition_variable wake;
    std::atomic<uint32_t> seedCounter{0};
    // audio-thread state
    uint32_t listenerVersion = 0;
    double loadAvg = 0.0;
    std::atomic<float> cpuLoad{0.0f};
    std::atomic<int> voices{0};
    std::atomic<bool> running{false};

    uint32_t nextSeed() { return seedCounter.fetch_add(0x9E3779B9u) ^ 0xA5A5F00Du; }
};
std::atomic<Engine*> g_engine{nullptr};

void onRefresh(void* user, int sfx, int variant) {
    Engine* e = static_cast<Engine*>(user);
    e->refresh.push({sfx, variant});  // dropped if full: the old variant simply stays
}

void renderCallback(void* user, float* out, int frames, int sampleRate) {
    Engine& e = *static_cast<Engine*>(user);
    auto t0 = Clock::now();
    Mixer& m = *e.mixer;
    if (m.sampleRate() != float(sampleRate)) m.prepare(float(sampleRate));

    SoundBuffer* b = nullptr;
    while (e.incoming.pop(b)) m.install(b);

    uint32_t lv = g_listenerVersion.load(std::memory_order_acquire);
    if (lv != e.listenerVersion && g_listenerMutex.try_lock()) {
        ListenerPose p = g_listener;
        g_listenerMutex.unlock();
        m.setListener(p);
        e.listenerVersion = lv;
    }
    m.setVolumes(g_master.load(std::memory_order_relaxed), g_effects.load(std::memory_order_relaxed),
                 g_ambienceVol.load(std::memory_order_relaxed));
    m.setAmbienceEnabled(g_ambienceOn.load(std::memory_order_relaxed));

    Command c;
    int64_t now = nowMs();
    while (e.commands.pop(c))
        if (now - c.stampMs < 250) m.play(c.req);  // stale requests (device was closed) are dropped

    m.process(out, frames);

    while (SoundBuffer* r = m.peekRetired()) {
        if (!e.graveyard.push(r)) break;
        m.dropRetired();
    }

    float peak = 0.0f;
    for (int i = 0; i < 2 * frames; ++i) peak = std::max(peak, std::fabs(out[i]));
    if (peak > g_outPeak.load(std::memory_order_relaxed)) g_outPeak.store(peak, std::memory_order_relaxed);

    double used = std::chrono::duration<double>(Clock::now() - t0).count();
    double avail = double(frames) / double(sampleRate);
    e.loadAvg += (used / avail - e.loadAvg) * 0.02;
    e.cpuLoad.store(float(e.loadAvg), std::memory_order_relaxed);
    e.voices.store(m.activeVoices(), std::memory_order_relaxed);
}

SoundBuffer* makeVariant(Sfx s, int variant, uint32_t seed) {
    SoundBuffer* b = new SoundBuffer();
    b->samples = synthesize(s, seed);
    b->sfx = int(s);
    b->variant = variant;
    return b;
}

void builderMain(Engine* e) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    auto t0 = Clock::now();
    auto pushOrWait = [e](SoundBuffer* b) {
        while (!e->incoming.push(b)) {
            if (e->quit.load()) {
                delete b;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    for (int v = 0; v < kVariants && !e->quit.load(); ++v)
        for (int s = 0; s < int(Sfx::Count) && !e->quit.load(); ++s)
            if (v < bankVariants(Sfx(s))) pushOrWait(makeVariant(Sfx(s), v, e->nextSeed()));
    if (!e->quit.load())
        LOGI("audio: sound bank synthesised (%d sounds, up to %d variants each) in %.0f ms", int(Sfx::Count), kVariants,
             std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    while (!e->quit.load()) {
        bool busy = false;
        SoundBuffer* dead = nullptr;
        while (e->graveyard.pop(dead)) delete dead;
        RefreshReq r;
        if (e->refresh.pop(r)) {
            pushOrWait(makeVariant(Sfx(r.sfx), r.variant, e->nextSeed()));
            g_refreshCount.fetch_add(1);
            busy = true;
        }
        if (!busy) {
            std::unique_lock<std::mutex> lk(e->wakeMutex);
            e->wake.wait_for(lk, std::chrono::milliseconds(30), [e] { return e->quit.load(); });
        }
    }
}

void pushPlay(const PlayRequest& r) {
    Engine* e = g_engine.load(std::memory_order_acquire);
    if (!e) return;
    Command c;
    c.req = r;
    c.stampMs = nowMs();
    e->commands.push(c);  // full queue (device stalled): dropped
}

bool finiteVec(m::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

}  // namespace

// ---------------------------------------------------------------------------------------------
bool init() {
    if (Engine* e = g_engine.load()) return e->backend && e->backend->status.deviceOpen.load();
    Engine* e = new Engine();
    uint32_t t = uint32_t(Clock::now().time_since_epoch().count());
    e->seedCounter.store(t * 2654435761u);
    e->mixer.reset(new Mixer(e->nextSeed()));
    e->mixer->setRefreshHook(&onRefresh, e);
    e->builder = std::thread(builderMain, e);
    e->backend = createBackend();
    g_engine.store(e, std::memory_order_release);
    bool ok = e->backend->start(&renderCallback, e);
    e->running.store(true);
    if (ok) LOGI("audio: started (%d Hz)", e->backend->status.sampleRate.load());
    else LOGW("audio: no output device, running silent");
    return ok;
}

void shutdown() {
    Engine* e = g_engine.exchange(nullptr);
    if (!e) return;
    if (e->backend) e->backend->stop();
    e->quit.store(true);
    e->wake.notify_all();
    if (e->builder.joinable()) e->builder.join();
    SoundBuffer* b = nullptr;
    while (e->incoming.pop(b)) delete b;
    while (e->graveyard.pop(b)) delete b;
    if (e->mixer)
        while (SoundBuffer* r = e->mixer->peekRetired()) {
            delete r;
            e->mixer->dropRetired();
        }
    delete e;
    LOGI("audio: stopped");
}

void setListener(m::vec3 position, m::vec3 forward, m::vec3 up) {
    if (!finiteVec(position) || !finiteVec(forward) || !finiteVec(up)) return;
    std::lock_guard<std::mutex> lk(g_listenerMutex);
    g_listener.pos = position;
    g_listener.fwd = forward;
    g_listener.up = up;
    g_listenerVersion.fetch_add(1, std::memory_order_release);
}

m::vec3 listenerPosition() {
    std::lock_guard<std::mutex> lk(g_listenerMutex);
    return g_listener.pos;
}

void play(Sfx s, m::vec3 position, float gain, float pitch) {
    if (int(s) < 0 || int(s) >= int(Sfx::Count) || !finiteVec(position) || !std::isfinite(gain) || !std::isfinite(pitch)) return;
    PlayRequest r;
    r.sfx = s;
    r.pos = position;
    r.gain = gain;
    r.pitch = pitch;
    r.bus = sfxInfo(s).ui ? Bus::UI : Bus::Effects;
    r.spatial = true;
    pushPlay(r);
}

void playFor(Sfx s, m::vec3 position, float seconds, float gain, float pitch) {
    if (int(s) < 0 || int(s) >= int(Sfx::Count) || !finiteVec(position) || !std::isfinite(gain) || !std::isfinite(pitch) ||
        !(seconds > 0.0f) || !std::isfinite(seconds))
        return;
    PlayRequest r;
    r.sfx = s;
    r.pos = position;
    r.gain = gain;
    r.pitch = pitch;
    r.bus = sfxInfo(s).ui ? Bus::UI : Bus::Effects;
    r.spatial = true;
    r.duration = seconds;
    pushPlay(r);
}

int penStrokeRequests(m::vec3 tip, float seconds, float gain, PlayRequest out[2]) {
    PlayRequest r;
    r.sfx = Sfx::PenTap;
    r.pos = tip;
    r.gain = gain;
    r.bus = Bus::Effects;
    r.spatial = true;
    out[0] = r;
    // Very short strokes (dots) are mostly the tick; the friction needs a few milliseconds to sound.
    if (!(seconds > 0.015f) || !std::isfinite(seconds)) return 1;
    r.sfx = Sfx::PenWrite;
    r.duration = seconds;
    out[1] = r;
    return 2;
}

void playPenStroke(m::vec3 tip, float seconds, float gain) {
    if (!finiteVec(tip) || !std::isfinite(gain)) return;
    PlayRequest r[2];
    const int n = penStrokeRequests(tip, seconds, gain, r);
    for (int i = 0; i < n; ++i) pushPlay(r[i]);
}

void playUI(Sfx s, float gain) {
    if (int(s) < 0 || int(s) >= int(Sfx::Count) || !std::isfinite(gain)) return;
    PlayRequest r;
    r.sfx = s;
    r.gain = gain;
    r.bus = Bus::UI;
    r.spatial = false;
    pushPlay(r);
}

void setAmbienceEnabled(bool on) { g_ambienceOn.store(on); }
void setMasterVolume(float v) { if (std::isfinite(v)) g_master.store(dsp::clampf(v, 0.0f, 2.0f)); }
void setEffectsVolume(float v) { if (std::isfinite(v)) g_effects.store(dsp::clampf(v, 0.0f, 2.0f)); }
void setAmbienceVolume(float v) { if (std::isfinite(v)) g_ambienceVol.store(dsp::clampf(v, 0.0f, 2.0f)); }

Stats stats() {
    Stats s;
    Engine* e = g_engine.load(std::memory_order_acquire);
    if (!e) return s;
    s.running = e->running.load();
    if (e->backend) {
        s.deviceOpen = e->backend->status.deviceOpen.load();
        s.sampleRate = e->backend->status.sampleRate.load();
        s.bufferFrames = e->backend->status.bufferFrames.load();
        s.underruns = e->backend->status.underruns.load();
        s.deviceRestarts = e->backend->status.restarts.load();
    }
    s.activeVoices = e->voices.load();
    s.cpuLoad = e->cpuLoad.load();
    return s;
}

float debugTakeOutputPeak() { return g_outPeak.exchange(0.0f); }
unsigned debugBankRefreshCount() { return g_refreshCount.load(); }

// ---------------------------------------------------------------------------------------------
// Offline rendering
ListenerPose whiteSeatListener() {
    ListenerPose p;
    p.pos = m::vec3(0.0f, layout::EYE_HEIGHT, layout::PLAYER_PELVIS_Z + 0.02f);
    p.fwd = m::normalize(m::vec3(0.0f, layout::BOARD_TOP_Y, 0.0f) - p.pos);
    p.up = m::vec3(0.0f, 1.0f, 0.0f);
    return p;
}

m::vec3 defaultPosition(Sfx s) {
    switch (s) {
        case Sfx::PiecePickup:
        case Sfx::PiecePlace: return layout::squareCenter(4, 3);  // e4
        case Sfx::Capture:
        case Sfx::CaptureClick: return layout::squareCenter(3, 4);  // d5
        case Sfx::TablePlace: return m::vec3(-layout::CAPTURE_X0, layout::TABLE_TOP_Y, 0.25f);
        case Sfx::ClockPress:
            return m::vec3(layout::CLOCK_OFFSET_X, layout::TABLE_TOP_Y + layout::CLOCK_HEIGHT, layout::CLOCK_Z);
        case Sfx::Handshake: return m::vec3(0.0f, 1.0f, 0.0f);
        case Sfx::ServoShort: return m::vec3(0.2f, 1.1f, -0.55f);   // opponent's right shoulder
        case Sfx::ChairCreak: return m::vec3(0.0f, layout::SEAT_HEIGHT, -layout::CHAIR_Z);
        case Sfx::PenWrite:
        case Sfx::PenTap:
        case Sfx::PageTurn:
        case Sfx::PageFlap:  // White's scoresheet (clock on +X)
            return m::vec3(-layout::SCORESHEET_X, layout::TABLE_TOP_Y + layout::SCORESHEET_THICKNESS, layout::SCORESHEET_Z);
        default: return whiteSeatListener().pos;
    }
}

namespace {
void installAll(Mixer& m, Sfx s, uint32_t seed) {
    for (int v = 0; v < bankVariants(s); ++v) {
        SoundBuffer* b = new SoundBuffer();
        b->samples = synthesize(s, seed * 7919u + uint32_t(v) * 104729u + uint32_t(s));
        b->sfx = int(s);
        b->variant = v;
        m.install(b);
    }
}

void renderFrames(Mixer& m, std::vector<float>& out, size_t frames) {
    out.assign(frames * 2, 0.0f);
    size_t done = 0;
    while (done < frames) {
        int n = int(std::min<size_t>(480, frames - done));
        m.process(out.data() + 2 * done, n);
        done += size_t(n);
    }
}

void fadeEdges(std::vector<float>& buf, float fadeInSec, float fadeOutSec) {
    size_t frames = buf.size() / 2;
    size_t fi = std::min(frames, size_t(fadeInSec * kBankRate)), fo = std::min(frames, size_t(fadeOutSec * kBankRate));
    for (size_t i = 0; i < fi; ++i) {
        float g = 0.5f - 0.5f * std::cos(dsp::kPi * float(i) / float(fi));
        buf[2 * i] *= g;
        buf[2 * i + 1] *= g;
    }
    for (size_t i = 0; i < fo; ++i) {
        float g = 0.5f - 0.5f * std::cos(dsp::kPi * float(i) / float(fo));
        size_t k = frames - 1 - i;
        buf[2 * k] *= g;
        buf[2 * k + 1] *= g;
    }
}
}  // namespace

std::vector<float> renderSfxOfflineAt(Sfx s, float seconds, m::vec3 pos, const ListenerPose& lis, uint32_t seed,
                                      bool withRoom) {
    Mixer m(seed);
    m.prepare(float(kBankRate));
    m.setVolumes(1.0f, 1.0f, 1.0f);
    m.setAmbienceEnabled(false, true);
    m.setRoomEnabled(withRoom);
    m.setListener(lis);
    installAll(m, s, seed);
    PlayRequest r;
    r.sfx = s;
    r.pos = pos;
    r.bus = sfxInfo(s).ui ? Bus::UI : Bus::Effects;
    r.spatial = true;
    m.play(r);
    std::vector<float> out;
    renderFrames(m, out, size_t(std::max(0.01f, seconds) * kBankRate));
    return out;
}

std::vector<float> renderSfxOffline(Sfx s, float seconds, uint32_t seed, OfflineStats* stats) {
    Mixer m(seed);
    m.prepare(float(kBankRate));
    m.setVolumes(1.0f, 1.0f, 1.0f);
    m.setAmbienceEnabled(false, true);
    m.setListener(whiteSeatListener());
    installAll(m, s, seed);
    PlayRequest r;
    r.sfx = s;
    if (sfxInfo(s).ui) {
        r.bus = Bus::UI;
        r.spatial = false;
    } else {
        r.pos = defaultPosition(s);
    }
    m.play(r);
    std::vector<float> out;
    renderFrames(m, out, size_t(std::max(0.01f, seconds) * kBankRate));
    fadeEdges(out, 0.0f, std::min(0.05f, seconds * 0.1f));
    if (stats) stats->limiterMinGain = m.takeLimiterMinGain();
    return out;
}

std::vector<float> renderAmbienceOffline(float seconds, uint32_t seed, OfflineStats* stats) {
    Mixer m(seed);
    m.prepare(float(kBankRate));
    m.setVolumes(1.0f, 1.0f, 1.0f);
    m.setAmbienceEnabled(true, true);
    m.setListener(whiteSeatListener());
    installAll(m, Sfx::ChairCreak, seed);
    std::vector<float> out;
    renderFrames(m, out, size_t(std::max(0.01f, seconds) * kBankRate));
    fadeEdges(out, 0.05f, 0.05f);
    if (stats) stats->limiterMinGain = m.takeLimiterMinGain();
    return out;
}

bool writeWav16(const char* path, const float* data, size_t frames, int channels, int sampleRate) {
    if (!path || !data || channels <= 0) return false;
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        LOGW("audio: cannot write %s", path);
        return false;
    }
    auto u32 = [f](uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; std::fwrite(b, 1, 4, f); };
    auto u16 = [f](uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; std::fwrite(b, 1, 2, f); };
    uint32_t dataBytes = uint32_t(frames * size_t(channels) * 2u);
    std::fwrite("RIFF", 1, 4, f);
    u32(36u + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);
    u16(uint16_t(channels));
    u32(uint32_t(sampleRate));
    u32(uint32_t(sampleRate * channels * 2));
    u16(uint16_t(channels * 2));
    u16(16);
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    dsp::Rng rng(12345u);
    std::vector<uint8_t> chunk;
    chunk.reserve(8192);
    size_t n = frames * size_t(channels);
    for (size_t i = 0; i < n; ++i) {
        float v = data[i];
        if (!std::isfinite(v)) v = 0.0f;
        float dither = (rng.uni() - rng.uni());  // TPDF, +-1 LSB
        long q = std::lrint(double(v) * 32767.0 + double(dither));
        q = q > 32767 ? 32767 : (q < -32768 ? -32768 : q);
        uint16_t u = uint16_t(int16_t(q));
        chunk.push_back(uint8_t(u));
        chunk.push_back(uint8_t(u >> 8));
        if (chunk.size() >= 8192) {
            std::fwrite(chunk.data(), 1, chunk.size(), f);
            chunk.clear();
        }
    }
    if (!chunk.empty()) std::fwrite(chunk.data(), 1, chunk.size(), f);
    bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

bool renderToWav(Sfx s, const char* path, float seconds) {
    std::vector<float> buf = renderSfxOffline(s, seconds, 1u);
    return writeWav16(path, buf.data(), buf.size() / 2, 2, kBankRate);
}

bool renderAmbienceToWav(const char* path, float seconds) {
    std::vector<float> buf = renderAmbienceOffline(seconds, 1u);
    return writeWav16(path, buf.data(), buf.size() / 2, 2, kBankRate);
}

}  // namespace audio
