// Public audio API: engine lifetime, game-thread -> mixer command queue, bank builder thread,
// offline rendering and WAV output.
//
// Threads:
//   * game thread(s): play() -> lock-free MPMC queue, volumes -> atomics, setListener() -> mutex
//     + version (the audio thread only try_locks it)
//   * audio thread (backend): drains commands, installs bank buffers, runs the Mixer
//   * builder thread (low priority): synthesises the initial bank (bankVariants(s) per Sfx, up to
//     kVariants), then re-synthesises each variant right after it is played (fresh seed), and
//     frees retired buffers so the audio thread never allocates or frees.
//
// Speech voices (openVoice ...): the ordered operations that carry data (open, append) go through
// their own command queue, without the 250 ms staleness rule of play() (an append must never be
// lost: chunks are owned by the command); only an open older than 1.5 s is dropped (the device was
// away: do not start an out-of-context sentence), and the appends that follow it are discarded.
// Chunks are allocated by the caller and freed by the builder thread (retired list -> graveyard).
// Close, stop and pause are id-tagged flags and the pose is a mutex + version (like the listener),
// so per-frame calls never fill the queue when no device drains it. The audio thread publishes
// each slot's state (CAS on the id), speech clock and consumed chunk count as id-tagged words;
// the game-side bookkeeping is guarded by g_voiceMutex, which the audio thread never takes.
#include "audio.h"
#include "backend.h"
#include "mixer.h"
#include "offline.h"
#include "queue.h"
#include "synth.h"
#include "../core/files.h"
#include "../core/log.h"
#include "../game/layout.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
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

// ---- Speech voices: shared per-slot state (outlives the engine, so a handle stays answerable
// across restarts). Voice id = (generation << 1) | slot, generation >= 1.
static_assert(kMaxSpeech == 2, "voice ids keep the slot in their lowest bit");
constexpr int64_t kVoiceOpenMaxAgeMs = 1500;

struct VoiceShared {
    std::atomic<uint64_t> state{0};   // (id << 8) | VoiceState: openVoice allocates, the audio thread publishes
    std::atomic<uint64_t> played{0};  // (id << 32) | source frames consumed (audio thread)
    std::atomic<uint64_t> done{0};    // (id << 32) | chunks consumed or discarded (audio thread)
    std::atomic<uint32_t> closeId{0}, stopId{0}, pausedId{0};  // flags, read by the audio thread
    std::atomic<float> stopFade{0.06f};
    // Game side (g_voiceMutex).
    int64_t queued = 0;  // source frames appended
    int rate = 44100;
    uint32_t sent = 0;   // chunks appended
    bool closed = false;
    bool nonFiniteLogged = false;
    // Emitter pose (listener pattern: the audio thread try_locks when the version moved).
    std::mutex poseMutex;
    uint32_t poseId = 0;
    m::vec3 pos, facing;
    std::atomic<uint32_t> poseVersion{0};
};
VoiceShared g_voices[kMaxSpeech];
std::mutex g_voiceMutex;
uint32_t g_voiceGeneration = 0;           // g_voiceMutex
std::atomic<float> g_voiceVol{1.0f};
std::atomic<int> g_speechChunksAlive{0};  // leak probe: chunks allocated by appendVoice, not yet freed

uint64_t stateWord(uint32_t id, VoiceState s) { return (uint64_t(id) << 8) | uint64_t(s); }
uint32_t wordId(uint64_t w) { return uint32_t(w >> 8); }
VoiceState wordState(uint64_t w) { return VoiceState(uint8_t(w & 0xffu)); }
bool terminal(VoiceState s) { return s == VoiceState::Finished || s == VoiceState::Stopped || s == VoiceState::Dropped; }

// Audio thread (and shutdown): moves a slot's published state forward for 'id' only. Terminal
// states are final; Dropped only replaces Pending. A slot reallocated meanwhile is left alone.
void publishState(VoiceShared& v, uint32_t id, VoiceState st) {
    uint64_t w = v.state.load(std::memory_order_acquire);
    for (;;) {
        if (wordId(w) != id || wordState(w) == st || terminal(wordState(w))) return;
        if (st == VoiceState::Dropped && wordState(w) != VoiceState::Pending) return;
        if (v.state.compare_exchange_weak(w, stateWord(id, st), std::memory_order_acq_rel, std::memory_order_acquire)) return;
    }
}

// Frees a buffer off the audio thread (builder thread, shutdown), counting speech chunks.
void freeBuffer(SoundBuffer* b) {
    if (!b) return;
    if (b->sfx < 0) g_speechChunksAlive.fetch_sub(1, std::memory_order_relaxed);
    delete b;
}

struct VoiceCmd {
    enum Op : uint8_t { Open, Append } op = Open;
    uint32_t id = 0;
    SoundBuffer* chunk = nullptr;  // Append: ownership travels with the command
    VoiceParams params;            // Open
    int64_t stampMs = 0;
};

struct Engine {
    MpmcQueue<Command, 512> commands;
    MpmcQueue<VoiceCmd, 256> voiceCmds;
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
    uint32_t speechId[kMaxSpeech] = {};    // voice id held by each mixer speech slot
    bool speechPaused[kMaxSpeech] = {};
    uint32_t poseSeen[kMaxSpeech] = {};
    double loadAvg = 0.0;
    std::atomic<float> cpuLoad{0.0f};
    std::atomic<int> voices{0};
    std::atomic<int> speech{0};
    std::atomic<bool> running{false};

    uint32_t nextSeed() { return seedCounter.fetch_add(0x9E3779B9u) ^ 0xA5A5F00Du; }
};
std::atomic<Engine*> g_engine{nullptr};

void onRefresh(void* user, int sfx, int variant) {
    Engine* e = static_cast<Engine*>(user);
    e->refresh.push({sfx, variant});  // dropped if full: the old variant simply stays
}

// Audio thread, before Mixer::process: speech commands, flags, pauses and poses.
void speechCommands(Engine& e, Mixer& m, int64_t now) {
    // The close/stop flags are read before the queue is drained: an append made before a close is
    // then guaranteed to be found in the queue (its push happened before the flag was set).
    uint32_t closeIds[kMaxSpeech], stopIds[kMaxSpeech];
    float fades[kMaxSpeech];
    for (int k = 0; k < kMaxSpeech; ++k) {
        closeIds[k] = g_voices[k].closeId.load(std::memory_order_acquire);
        stopIds[k] = g_voices[k].stopId.load(std::memory_order_acquire);
        fades[k] = g_voices[k].stopFade.load(std::memory_order_relaxed);
    }
    VoiceCmd vc;
    while (e.voiceCmds.pop(vc)) {
        const int k = int(vc.id & 1u);
        if (vc.op == VoiceCmd::Open) {
            if (now - vc.stampMs > kVoiceOpenMaxAgeMs) {
                publishState(g_voices[k], vc.id, VoiceState::Dropped);
                continue;  // its appends no longer match the slot and are discarded
            }
            m.speechOpen(k, speechParams(vc.params));
            e.speechId[k] = vc.id;
            e.speechPaused[k] = false;
        } else if (vc.id == e.speechId[k] && vc.id != 0) {
            m.speechAppend(k, vc.chunk);
        } else {
            m.discard(vc.chunk);
        }
    }
    for (int k = 0; k < kMaxSpeech; ++k) {
        const uint32_t id = e.speechId[k];
        if (!id) continue;
        VoiceShared& v = g_voices[k];
        if (stopIds[k] == id) m.speechStop(k, fades[k]);
        if (closeIds[k] == id) m.speechClose(k);
        const bool paused = v.pausedId.load(std::memory_order_acquire) == id;
        if (paused != e.speechPaused[k]) {
            m.speechPause(k, paused);
            e.speechPaused[k] = paused;
        }
        const uint32_t pv = v.poseVersion.load(std::memory_order_acquire);
        if (pv != e.poseSeen[k] && v.poseMutex.try_lock()) {  // contended: picked up next callback
            const uint32_t pid = v.poseId;
            const m::vec3 pos = v.pos, facing = v.facing;
            v.poseMutex.unlock();
            if (pid == id) m.speechPose(k, pos, facing);
            // A pose for a voice whose open is still in flight is kept for the next callback.
            const uint64_t w = v.state.load(std::memory_order_acquire);
            if (pid == id || pid != wordId(w) || terminal(wordState(w))) e.poseSeen[k] = pv;
        }
    }
    m.setVoiceVolume(g_voiceVol.load(std::memory_order_relaxed));
}

// Audio thread, after Mixer::process: speech clock and consumed chunks first, then the state, so a
// reader that sees Finished also sees the final clock.
void speechPublish(Engine& e, Mixer& m) {
    for (int k = 0; k < kMaxSpeech; ++k) {
        const uint32_t id = e.speechId[k];
        if (!id) continue;
        const SpeechInfo info = m.speechInfo(k);
        VoiceShared& v = g_voices[k];
        const uint64_t played = uint64_t(std::min<int64_t>(std::max<int64_t>(info.played, 0), 0xffffffffLL));
        v.played.store((uint64_t(id) << 32) | played, std::memory_order_release);
        v.done.store((uint64_t(id) << 32) | info.chunksDone, std::memory_order_release);
        publishState(v, id, info.state);
    }
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

    speechCommands(e, m, now);
    m.process(out, frames);
    speechPublish(e, m);

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
    e.speech.store(m.activeSpeech(), std::memory_order_relaxed);
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
        while (e->graveyard.pop(dead)) freeBuffer(dead);
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
    VoiceCmd vc;
    while (e->voiceCmds.pop(vc))
        if (vc.op == VoiceCmd::Append) freeBuffer(vc.chunk);
    if (e->mixer)
        for (int k = 0; k < kMaxSpeech; ++k) e->mixer->speechStop(k, 0.0f);  // hands its chunks back
    while (e->incoming.pop(b)) freeBuffer(b);
    while (e->graveyard.pop(b)) freeBuffer(b);
    if (e->mixer)
        while (SoundBuffer* r = e->mixer->peekRetired()) {
            freeBuffer(r);
            e->mixer->dropRetired();
        }
    // Every voice not yet over ends Stopped; its handle stays answerable.
    for (VoiceShared& v : g_voices) {
        uint64_t w = v.state.load(std::memory_order_acquire);
        while (wordId(w) != 0 && !terminal(wordState(w)) &&
               !v.state.compare_exchange_weak(w, stateWord(wordId(w), VoiceState::Stopped), std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        }
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
void setVoiceVolume(float v) { if (std::isfinite(v)) g_voiceVol.store(dsp::clampf(v, 0.0f, 2.0f)); }

// ---- Speech voices ----------------------------------------------------------------------------

VoiceId openVoice(const VoiceParams& p) {
    Engine* e = g_engine.load(std::memory_order_acquire);
    if (!e) return {};
    if (!finiteVec(p.position) || !finiteVec(p.facing) || p.sampleRate < 8000 || p.sampleRate > 192000 ||
        !std::isfinite(p.gain) || !std::isfinite(p.roomSend) || !std::isfinite(p.duckDb)) {
        LOGW("audio: openVoice: invalid parameters");
        return {};
    }
    std::lock_guard<std::mutex> lk(g_voiceMutex);
    // A free slot first (never used or over); else one whose voice was asked to stop (its fade is
    // then cut short, declicked, by the new voice).
    for (int pass = 0; pass < 2; ++pass)
        for (int k = 0; k < kMaxSpeech; ++k) {
            VoiceShared& v = g_voices[k];
            uint64_t w = v.state.load(std::memory_order_acquire);
            for (;;) {
                const VoiceState st = wordState(w);
                const bool usable = pass == 0 ? (st == VoiceState::None || terminal(st))
                                              : (wordId(w) != 0 && v.stopId.load(std::memory_order_relaxed) == wordId(w));
                if (!usable) break;
                if (++g_voiceGeneration >= (1u << 31)) g_voiceGeneration = 1;
                const uint32_t id = (g_voiceGeneration << 1) | uint32_t(k);
                // The audio thread may publish a new state for the old id meanwhile: re-check then.
                if (!v.state.compare_exchange_strong(w, stateWord(id, VoiceState::Pending), std::memory_order_acq_rel,
                                                     std::memory_order_acquire))
                    continue;
                v.queued = 0;
                v.rate = p.sampleRate;
                v.sent = 0;
                v.closed = false;
                v.nonFiniteLogged = false;
                {
                    std::lock_guard<std::mutex> pl(v.poseMutex);
                    v.poseId = id;
                    v.pos = p.position;
                    v.facing = p.facing;
                }
                VoiceCmd c;
                c.op = VoiceCmd::Open;
                c.id = id;
                c.params = p;
                c.stampMs = nowMs();
                if (!e->voiceCmds.push(c)) {  // queue full (device stalled)
                    v.state.store(stateWord(id, VoiceState::Dropped), std::memory_order_release);
                    return {};
                }
                return VoiceId{id};
            }
        }
    return {};
}

double appendVoice(VoiceId id, std::vector<float>&& mono) {
    Engine* e = g_engine.load(std::memory_order_acquire);
    if (!id || !e) return -1.0;
    VoiceShared& v = g_voices[id.v & 1u];
    std::lock_guard<std::mutex> lk(g_voiceMutex);
    const uint64_t w = v.state.load(std::memory_order_acquire);
    if (wordId(w) != id.v || terminal(wordState(w)) || v.closed || v.stopId.load(std::memory_order_relaxed) == id.v)
        return -1.0;
    const double start = double(v.queued) / double(v.rate);
    if (mono.empty()) return start;
    const uint64_t dw = v.done.load(std::memory_order_acquire);
    const uint32_t done = uint32_t(dw >> 32) == id.v ? uint32_t(dw) : 0u;
    if (v.sent - done >= uint32_t(kSpeechChunks)) return -1.0;  // the mixer FIFO is full: retry later
    // The only float input the API cannot range-check: one NaN/Inf would latch in the hall reverb
    // and the DC blockers and silence all audio for the session, so it becomes silence here.
    int nonFinite = 0;
    for (float& x : mono)
        if (!std::isfinite(x)) {
            x = 0.0f;
            ++nonFinite;
        }
    if (nonFinite && !v.nonFiniteLogged) {
        LOGW("audio: appendVoice: %d non-finite samples replaced with silence", nonFinite);
        v.nonFiniteLogged = true;
    }
    SoundBuffer* b = new SoundBuffer();
    b->samples = std::move(mono);
    b->sfx = -1;
    const int64_t frames = int64_t(b->samples.size());
    VoiceCmd c;
    c.op = VoiceCmd::Append;
    c.id = id.v;
    c.chunk = b;
    c.stampMs = nowMs();
    g_speechChunksAlive.fetch_add(1, std::memory_order_relaxed);
    if (!e->voiceCmds.push(c)) {  // the caller keeps its audio and may retry
        mono = std::move(b->samples);
        freeBuffer(b);
        return -1.0;
    }
    v.queued += frames;
    ++v.sent;
    return start;
}

void closeVoice(VoiceId id) {
    if (!id) return;
    VoiceShared& v = g_voices[id.v & 1u];
    std::lock_guard<std::mutex> lk(g_voiceMutex);
    const uint64_t w = v.state.load(std::memory_order_acquire);
    if (wordId(w) != id.v || terminal(wordState(w)) || v.closed) return;
    v.closed = true;
    v.closeId.store(id.v, std::memory_order_release);
}

VoiceId playVoice(std::vector<float>&& mono, const VoiceParams& p) {
    VoiceId id = openVoice(p);
    if (!id) return {};
    if (appendVoice(id, std::move(mono)) < 0.0) {
        stopVoice(id, 0.0f);
        return {};
    }
    closeVoice(id);
    return id;
}

void stopVoice(VoiceId id, float fadeSeconds) {
    if (!id) return;
    VoiceShared& v = g_voices[id.v & 1u];
    std::lock_guard<std::mutex> lk(g_voiceMutex);
    const uint64_t w = v.state.load(std::memory_order_acquire);
    if (wordId(w) != id.v || terminal(wordState(w))) return;
    v.stopFade.store(std::isfinite(fadeSeconds) ? dsp::clampf(fadeSeconds, 0.0f, 10.0f) : 0.0f, std::memory_order_relaxed);
    v.stopId.store(id.v, std::memory_order_release);
}

void setVoicePaused(VoiceId id, bool paused) {
    if (!id) return;
    VoiceShared& v = g_voices[id.v & 1u];
    std::lock_guard<std::mutex> lk(g_voiceMutex);
    const uint64_t w = v.state.load(std::memory_order_acquire);
    if (wordId(w) != id.v || terminal(wordState(w))) return;
    if (paused) {
        v.pausedId.store(id.v, std::memory_order_release);
    } else {
        uint32_t expected = id.v;
        v.pausedId.compare_exchange_strong(expected, 0u, std::memory_order_acq_rel);
    }
}

void setVoicePose(VoiceId id, m::vec3 position, m::vec3 facing) {
    if (!id || !finiteVec(position) || !finiteVec(facing)) return;
    VoiceShared& v = g_voices[id.v & 1u];
    std::lock_guard<std::mutex> pl(v.poseMutex);
    if (v.poseId != id.v) return;  // stale handle: the slot holds another voice
    v.pos = position;
    v.facing = facing;
    v.poseVersion.fetch_add(1, std::memory_order_release);
}

VoiceStatus voiceStatus(VoiceId id) {
    VoiceStatus s;
    if (!id) return s;
    VoiceShared& v = g_voices[id.v & 1u];
    std::lock_guard<std::mutex> lk(g_voiceMutex);
    const uint64_t w = v.state.load(std::memory_order_acquire);
    if (wordId(w) != id.v) return s;  // stale: None
    s.state = wordState(w);
    const uint64_t pw = v.played.load(std::memory_order_acquire);
    const double rate = double(v.rate);
    s.played = uint32_t(pw >> 32) == id.v ? double(uint32_t(pw)) / rate : 0.0;
    s.queued = double(v.queued) / rate;
    s.closed = v.closed;
    return s;
}

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
    s.activeSpeech = e->speech.load();
    s.cpuLoad = e->cpuLoad.load();
    return s;
}

float debugTakeOutputPeak() { return g_outPeak.exchange(0.0f); }
unsigned debugBankRefreshCount() { return g_refreshCount.load(); }
int debugSpeechChunksAlive() { return g_speechChunksAlive.load(); }

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

m::vec3 coachMouthDefault() {
    // Black's robot, head up: the mouth line sits ~6 cm below the eyes, at the pelvis plane.
    return m::vec3(0.0f, layout::EYE_HEIGHT - 0.06f, -layout::PLAYER_PELVIS_Z);
}

std::vector<float> renderVoiceOffline(const std::vector<float>& mono, int srcRate, float seconds, m::vec3 pos,
                                      const ListenerPose& lis, bool ambience, OfflineStats* stats, m::vec3 facing,
                                      bool withRoom) {
    Mixer m(1u);
    m.prepare(float(kBankRate));
    m.setVolumes(1.0f, 1.0f, 1.0f);
    m.setVoiceVolume(1.0f);
    m.setAmbienceEnabled(ambience, true);
    m.setRoomEnabled(withRoom);
    m.setListener(lis);
    if (ambience) installAll(m, Sfx::ChairCreak, 1u);
    VoiceParams vp;
    vp.position = pos;
    vp.facing = facing;
    vp.sampleRate = srcRate;
    m.speechOpen(0, speechParams(vp));
    SoundBuffer* b = new SoundBuffer();
    b->samples = mono;
    b->sfx = -1;
    m.speechAppend(0, b);
    m.speechClose(0);
    std::vector<float> out;
    renderFrames(m, out, size_t(std::max(0.01f, seconds) * kBankRate));
    if (stats) stats->limiterMinGain = m.takeLimiterMinGain();
    return out;
}

// The path is UTF-8: opened as a wide path on Windows (u8path), whatever the process code page.
bool writeWav16(const char* path, const float* data, size_t frames, int channels, int sampleRate) {
    if (!path || !data || channels <= 0) return false;
    FILE* f = files::create(path);
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
