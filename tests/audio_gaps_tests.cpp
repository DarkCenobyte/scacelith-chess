// The audio engine, the paths tests/audio_tests.cpp leaves out: the listener read back (a pose
// with a non-finite value ignored), pen strokes (a dot is the tick alone; non-finite ones are
// ignored), voices refused for their parameters, WAV files that cannot be written, the null
// backend asked to dump into a folder that does not exist, and the mixer's retired list (a bank
// buffer of no effect, a discarded one). The engine runs on the null backend (tests/test_main.cpp
// sets SCACELITH_AUDIO=null): nothing is heard.
#include "test.h"
#include "headless.h"
#include "audio/audio.h"
#include "audio/backend.h"
#include "audio/mixer.h"
#include "audio/spatial.h"
#include "net/net_sys.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

namespace {

bool eventually(const std::function<bool()>& cond, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!cond()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

bool same(m::vec3 a, m::vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

}  // namespace

TEST(audio_gaps_listener_pen_strokes_and_voice_parameters) {
    headless::stopShared();   // a shared App's game scene would hold the audio engine
    audio::setAmbienceEnabled(false);
    audio::init();
    CHECK(audio::stats().running);
    const m::vec3 pos(0.1f, 1.2f, 0.7f);
    audio::setListener(pos, m::vec3(0, -0.6f, -0.8f), m::vec3(0, 1, 0));
    CHECK(same(audio::listenerPosition(), pos));
    audio::setListener(m::vec3(NAN, 0, 0), m::vec3(0, 0, -1), m::vec3(0, 1, 0));   // ignored
    audio::setListener(m::vec3(0, 0, 0), m::vec3(0, 0, INFINITY), m::vec3(0, 1, 0));
    CHECK(same(audio::listenerPosition(), pos));
    // Pen strokes on the scoresheet: played once the bank has the sounds.
    const m::vec3 tip(0.3f, 0.76f, 0.35f);
    audio::playPenStroke(m::vec3(NAN, 0, 0), 0.3f);       // ignored
    audio::playPenStroke(tip, 0.3f, INFINITY);           // ignored
    audio::playPenStroke(tip, 0.005f, 0.5f);             // a dot: the tick alone
    audio::playPenStroke(tip, NAN, 0.5f);                // no duration: the tick alone
    const bool played = eventually([&] {
        audio::playPenStroke(tip, 0.4f, 0.8f);
        return audio::stats().activeVoices > 0;
    }, 5000);
#ifndef _WIN32
    CHECK(played);   // the null backend always runs the mixer
#else
    (void)played;
#endif
    // Voices: refused for each parameter out of range, before anything is opened.
    audio::VoiceParams good;
    good.sampleRate = 24000;
    auto refused = [&](const std::function<void(audio::VoiceParams&)>& change) {
        audio::VoiceParams p = good;
        change(p);
        return !audio::openVoice(p);
    };
    CHECK(refused([](audio::VoiceParams& p) { p.sampleRate = 7999; }));
    CHECK(refused([](audio::VoiceParams& p) { p.sampleRate = 192001; }));
    CHECK(refused([](audio::VoiceParams& p) { p.position = m::vec3(0, NAN, 0); }));
    CHECK(refused([](audio::VoiceParams& p) { p.facing = m::vec3(0, 0, INFINITY); }));
    CHECK(refused([](audio::VoiceParams& p) { p.gain = NAN; }));
    CHECK(refused([](audio::VoiceParams& p) { p.roomSend = INFINITY; }));
    CHECK(refused([](audio::VoiceParams& p) { p.duckDb = -INFINITY; }));
    CHECK_EQ(audio::stats().activeSpeech, 0);
    if (audio::stats().running) {
        audio::VoiceId id = audio::openVoice(good);
        CHECK(bool(id));
        audio::stopVoice(id, 0.0f);
    }
    audio::shutdown();
    CHECK(!audio::stats().running);
    const audio::ListenerPose seat;
    audio::setListener(seat.pos, seat.fwd, seat.up);
    audio::setAmbienceEnabled(true);
}

TEST(audio_gaps_wav_cannot_be_written) {
    headless::TempDir dir("audio-gaps-wav");
    CHECK(!audio::renderToWav(audio::Sfx::PiecePlace, dir.file("missing/place.wav").c_str(), 0.1f));
    CHECK(!audio::renderAmbienceToWav(dir.file("missing/ambience.wav").c_str(), 0.1f));
    CHECK(!net::sys::fileExists(dir.file("missing/place.wav")));
    CHECK(audio::renderToWav(audio::Sfx::PiecePlace, dir.file("place.wav").c_str(), 0.1f));   // the folder itself takes it
}

#ifndef _WIN32
TEST(audio_gaps_null_backend_dump_unwritable) {
    // A dump into a folder that does not exist: said in the log, the backend runs all the same.
    headless::TempDir dir("audio-gaps-dump");
    const std::string dump = dir.file("missing/dump.wav");
    const char* before = std::getenv("SCACELITH_AUDIO_DUMP");
    const std::string saved = before ? before : "";
    setenv("SCACELITH_AUDIO_DUMP", dump.c_str(), 1);
    std::unique_ptr<audio::Backend> b = audio::createNullBackend();
    std::atomic<int> calls{0};
    auto render = [](void* user, float* stereo, int frames, int) {
        for (int i = 0; i < frames * 2; ++i) stereo[i] = 0.0f;
        ++*static_cast<std::atomic<int>*>(user);
    };
    CHECK(b->start(render, &calls));
    CHECK(b->status.deviceOpen.load());
    CHECK(eventually([&] { return calls.load() >= 3; }, 3000));
    b->stop();
    CHECK(!b->status.deviceOpen.load());
    CHECK(!net::sys::fileExists(dump));
    if (before) setenv("SCACELITH_AUDIO_DUMP", saved.c_str(), 1);
    else unsetenv("SCACELITH_AUDIO_DUMP");
}
#endif

TEST(audio_gaps_mixer_retired_buffers) {
    audio::Mixer mixer(5u);
    CHECK(mixer.peekRetired() == nullptr);
    // A bank buffer of no effect, or of no variant, goes straight to the retired list.
    auto* noSfx = new audio::SoundBuffer;
    noSfx->sfx = int(audio::Sfx::Count);
    mixer.install(noSfx);
    CHECK(mixer.peekRetired() == noSfx);
    mixer.dropRetired();
    delete noSfx;
    auto* noVariant = new audio::SoundBuffer;
    noVariant->variant = -1;
    mixer.install(noVariant);
    CHECK(mixer.peekRetired() == noVariant);
    mixer.dropRetired();
    delete noVariant;
    CHECK(mixer.peekRetired() == nullptr);
    // A discarded buffer too (nullptr: nothing); the mixer frees what is still retired.
    mixer.discard(nullptr);
    CHECK(mixer.peekRetired() == nullptr);
    auto* chunk = new audio::SoundBuffer;
    chunk->sfx = -1;
    mixer.discard(chunk);
    CHECK(mixer.peekRetired() == chunk);
}
