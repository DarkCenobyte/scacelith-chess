// Coach mode in the game scene (see game_scene.h): the coach game's set-up and flow, the
// coach::Stage the session performs on (the coach's voice from its mouth, the subtitles, its body:
// gaze, gestures, nods; the table: demonstration moves, rewinds and takebacks by hand, the lesson's
// positions and moves; the marks and the HUD), the coach::Analyst over the scene's Stockfish, and
// the --coach-stage-test sequence.
//
// The coach is the Stockfish seat (aiSeat()): its game moves go through updateAi like any engine
// move (Turn::AiMoving), taken from the teaching repertoire first, and wait while the session
// reviews the player's move (Session::coachMayMove) or the coach's hand is busy. Everything the
// coach does on the table for teaching runs as table jobs, one after the other, once both hands and
// every piece are at rest: a demonstration is never recorded and never uses Turn::AiMoving (an
// untimed game completes any AiMoving move once its pieces are down).
#include "game_scene.h"
#include "coach_model.h"
#include "../audio/audio.h"
#include "../character/skeleton.h"
#include "../coach/catalog.h"
#include "../coach/lesson.h"
#include "../coach/openings.h"
#include "../coach/pacing.h"
#include "../coach/repertoire.h"
#include "../coach/rewind.h"
#include "../coach/session.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "../i18n/unicode.h"
#include "../platform/platform.h"
#include "../tts/tts.h"
#include "../ui/ui_font.h"
#include "layout.h"
#include "look_up.h"
#include "settings.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <set>
#include <thread>

using namespace m;
using namespace chess;

namespace game {

namespace {

// Demonstration moves: slower than a move in play (anim::Timing), so the player can follow them.
constexpr float kDemoReach = 0.55f, kDemoLift = 0.20f, kDemoCarry = 0.60f, kDemoTake = 0.25f, kDemoPlace = 0.30f,
                kDemoDiscard = 0.50f;
// A lesson position set up behind a short fade.
constexpr float kSetupFadeOut = 0.30f, kSetupFadeIn = 0.45f;
// Arrival of a speaking gesture's apex after its start (the animator's approach).
constexpr float kGestureApproach = anim::Timing::PointApproach;
constexpr float kBeatDuration = 1.35f;    // three strokes; the first one lands after 0.27 s
constexpr float kNodApex = 0.2f;          // a nod is at its lowest this long after it starts
// The view's lift to the coach's face (lead decision §4.1): after the pointer has rested this long.
constexpr float kFaceLiftIdle = 1.5f;
// Failsafes of the end of a coach game, should the session never get there.
constexpr float kHandshakeFailsafe = 45.0f, kEndCardFailsafe = 300.0f;

anim::Task task(anim::TaskType t, int pieceId = -1, vec3 pos = vec3(0), float height = 0.0f, float duration = 0.0f) {
    anim::Task k;
    k.type = t;
    k.pieceId = pieceId;
    k.position = pos;
    k.height = height;
    k.duration = duration;
    return k;
}

std::string playerName() {
    const std::string& n = settings().playerName;
    return n.empty() || n == "Human" ? std::string(i18n::tr("player.default_name")) : n;
}

float smooth01(float x) { return x <= 0.0f ? 0.0f : x >= 1.0f ? 1.0f : x * x * (3.0f - 2.0f * x); }

}  // namespace

// ==============================================================================================
// Runtime state
// ==============================================================================================

// A job of the coach's hands on the table (Stage calls), run one after the other.
struct TableJob {
    enum class Kind { Demo, Rewind, TakeBack, SetPosition, LessonMove } kind = Kind::Demo;
    std::string uci;        // Demo, LessonMove
    float pause = 0.0f;     // Demo: stillness after the piece is placed
    int plies = 0;          // Rewind, TakeBack
    bool fast = false;      // Rewind: Space was pressed
    std::string fen;        // SetPosition
};

// A mark the stage test shows by itself (the director's marks come from Director::marks()).
struct TestMark {
    coach::ShownMark mark;
    float on = 0.0f, off = 1e9f;   // game time
};

struct CoachRuntime {
    explicit CoachRuntime(GameScene& s);
    ~CoachRuntime();

    coach::Session session;
    std::unique_ptr<CoachStage> stage;
    std::unique_ptr<CoachAnalyst> analyst;
    bool sessionRunning = false;
    uint64_t seed = 1;

    // The voice: the TTS worker (restarted when the lesson's slower speed is wanted) and the one
    // utterance playing from the coach's mouth.
    tts::Worker worker;
    bool workerStarted = false;
    float workerSpeed = 1.0f;
    std::set<uint32_t> speechIds;                     // requested, not taken nor cancelled
    std::map<uint32_t, std::vector<float>> speechDone;   // finished, not taken yet (empty = failed)
    audio::VoiceId voice;
    bool voiceActive = false;
    bool voiceStarted = false;                        // an utterance was started (voiceClock >= 0 after it)
    double voiceQueued = 0.0;                         // seconds of the utterance
    double voiceGameT = 0.0;                          // game time since it started (paused: stops)
    bool voicePausedByDirector = false, voicePausedByScene = false;
    float level = 0.0f, quiet = 0.0f;                 // speech level (mouth) and silence, for blinks
    bool loud = false;

    // Subtitles, HUD
    std::string subText;
    float subAge = 0.0f, subHold = 0.0f;
    bool offerShown = false, skipHint = false;
    float offerAge = 0.0f;                            // seconds the takeback card has been up
    bool forceSubtitles = false;                      // the stage test shows its line whatever the option

    // Body
    coach::Look look = coach::Look::Player;
    Square lookSquare = NoSquare;
    float lookHold = 0.0f;                            // the look holds while the coach talks, then this long
    bool handOut = false;                             // gestures given since the last retract
    std::vector<std::pair<float, bool>> headMoves;    // (game time, nod?) still to come
    vec2 lastPointer{-1.0f, -1.0f};
    float pointerIdle = 0.0f;

    // The table
    std::deque<TableJob> jobs;
    bool jobRunning = false;
    TableJob job;
    int phase = 0;
    struct Ply {
        Position target;   // the table after this ply is taken back
        int seat = 0;      // the hand for the pieces on the board
        bool demo = false;
    };
    std::vector<Ply> plies;  // still to take back in this job (next = back())
    std::vector<coach::PieceTrip> trips;
    size_t tripNext = 0;
    int batchSeat = -1;
    Position demoPos;                                 // the table's position (the game + the demonstrations)
    std::vector<Position> demoBefore;                 // before each demonstration move on the table
    Move lessonMove;

    // Marks, analyses, draw offer, end of the game
    std::map<int, int> markPieces;                    // square of a Piece mark -> the piece's id
    std::vector<TestMark> testMarks;
    std::set<uint32_t> analyses;                      // ids the session asked for
    uint32_t drawAnalysis = 0;
    int drawPly = -1;
    bool held = false;                                // paused or focus lost (the session is held)
    bool handshakeReported = false, resultsSaved = false;

    // --coach-stage-test
    bool test = false;
    std::string testKind;
    int testStep = 0;
    float testTime = 0.0f, testClock = 0.0f;
    uint32_t testSpeech = 0;
    coach::Catalog::Rendered testSpoken, testWritten;
    std::vector<float> testPcm;
    float testSq = 0.0f, testSq2 = 0.0f;

    std::thread warm;                                 // the opening book, built off the render thread
    std::set<std::string> prewarmedLanguages;
};

// ==============================================================================================
// The Stage
// ==============================================================================================

class CoachStage final : public coach::Stage {
public:
    explicit CoachStage(GameScene& s) : s_(s) {}
    CoachRuntime& rt() const { return *s_.coach_; }
    anim::Animator& hand() const { return s_.anim_[s_.aiSeat()]; }

    // ---- Voice
    // Heard: the model files, a worker that loaded them, and an output device (without one the
    // audio engine runs silently, Windows retrying in the background: the lines go to subtitles).
    bool voiceAvailable() const override {
        if (!s_.coachVoiceFiles_ || !rt().workerStarted || rt().worker.failed()) return false;
        audio::Stats a = audio::stats();
        return a.running && a.deviceOpen;
    }
    void ensureWorker(float speed) {
        CoachRuntime& r = rt();
        if (!s_.coachVoiceFiles_) return;
        // A worker that failed to load is tried again (the model files may have arrived since).
        bool retry = r.workerStarted && r.worker.failed();
        if (r.workerStarted && !retry && std::fabs(speed - r.workerSpeed) < 0.005f) return;
        // A new speed restarts the worker: only between lines (nothing queued would be lost).
        if (r.workerStarted && (!r.speechIds.empty() || r.worker.pending() > 0)) return;
        const Settings& st = settings();
        tts::setArchCap(st.ttsArch.c_str());
        tts::Options o;
        o.threads = st.ttsThreads > 0 ? st.ttsThreads : 2;
        o.voice = st.ttsVoice;
        o.steps = st.ttsSteps;
        o.speed = speed;
        r.worker.stop();
        r.worker.start(o);
        r.workerStarted = true;
        r.workerSpeed = speed;
        LOGI("coach: voice worker started (%s, %d threads, speed %.2f, voice %d, %d steps)", tts::modelDirectory().c_str(),
             o.threads, speed, o.voice, o.steps);
    }
    uint32_t requestSpeech(const std::string& text, const std::string& lang, float speed, int priority) override {
        CoachRuntime& r = rt();
        ensureWorker(speed);
        if (!r.workerStarted || r.worker.failed() || text.empty()) return 0;
        uint32_t id = r.worker.request(text, lang, priority);
        if (id) r.speechIds.insert(id);
        return id;
    }
    // Moves a finished request's samples into speechDone (screenshot runs wait for it: the frames
    // do not follow the wall clock).
    void poll(uint32_t id) const {
        CoachRuntime& r = rt();
        if (!r.speechIds.count(id) || r.speechDone.count(id)) return;
        if (s_.ctx_->screenshotMode)
            for (int i = 0; i < 12000 && !r.worker.done(id) && !r.worker.failed(); ++i) plat::sleepMs(5);
        std::vector<float> pcm;
        if (r.worker.take(id, pcm)) r.speechDone[id] = std::move(pcm);
    }
    bool takeSpeech(uint32_t id, std::vector<float>& pcm) override {
        CoachRuntime& r = rt();
        poll(id);
        auto it = r.speechDone.find(id);
        if (it == r.speechDone.end() || it->second.empty()) return false;
        pcm = std::move(it->second);
        r.speechDone.erase(it);
        r.speechIds.erase(id);
        return true;
    }
    bool speechFailed(uint32_t id) const override {
        CoachRuntime& r = rt();
        if (!r.workerStarted || r.worker.failed() || !r.speechIds.count(id)) return true;
        poll(id);
        auto it = r.speechDone.find(id);
        return it != r.speechDone.end() && it->second.empty();
    }
    void cancelSpeech(uint32_t id) override {
        CoachRuntime& r = rt();
        if (!r.workerStarted) return;
        r.worker.cancel(id);
        if (id == 0) {
            r.speechIds.clear();
            r.speechDone.clear();
        } else {
            r.speechIds.erase(id);
            r.speechDone.erase(id);
        }
    }
    int speechSampleRate() const override { return 44100; }
    void mouth(vec3& position, vec3& facing) const {
        const mat4& head = hand().globals()[character::Head];
        position = transformPoint(head, vec3(0.0f, 0.0195f, 0.08f));
        facing = normalize(head.c[2].xyz());
    }
    bool startVoice(std::vector<float>&& pcm) override {
        CoachRuntime& r = rt();
        if (pcm.empty()) return true;
        stopVoice();
        audio::VoiceParams p;
        mouth(p.position, p.facing);
        p.sampleRate = speechSampleRate();
        audio::VoiceId id = audio::openVoice(p);
        if (!id) return false;   // pcm untouched: the director retries
        double seconds = double(pcm.size()) / double(speechSampleRate());
        if (audio::appendVoice(id, std::move(pcm)) < 0.0) {
            audio::stopVoice(id, 0.0f);
            return false;
        }
        audio::closeVoice(id);
        LOGD("coach: voice of %.2f s", seconds);
        r.voice = id;
        r.voiceActive = true;
        r.voiceStarted = true;
        r.voiceQueued = seconds;
        r.voiceGameT = 0.0;
        if (r.voicePausedByDirector || r.voicePausedByScene) audio::setVoicePaused(id, true);
        return true;
    }
    void stopVoice() override {
        CoachRuntime& r = rt();
        if (r.voiceActive) audio::stopVoice(r.voice, 0.08f);
        r.voiceActive = false;
    }
    void pauseVoice(bool paused) override {
        CoachRuntime& r = rt();
        r.voicePausedByDirector = paused;
        applyPause();
    }
    void applyPause() {
        CoachRuntime& r = rt();
        if (r.voiceActive) audio::setVoicePaused(r.voice, r.voicePausedByDirector || r.voicePausedByScene);
    }
    bool voicePaused() const { return rt().voicePausedByDirector || rt().voicePausedByScene; }
    double voiceClock(bool* finished) const override {
        CoachRuntime& r = rt();
        if (finished) *finished = false;
        if (!r.voiceActive) {
            // Over (heard, stopped or dropped): the end, as long as nothing new starts.
            if (finished) *finished = true;
            return r.voiceStarted ? r.voiceQueued : -1.0;
        }
        audio::VoiceStatus st = audio::voiceStatus(r.voice);
        audio::Stats as = audio::stats();
        // The device's clock when one plays the voice; else (no device, screenshot runs, warps) the
        // game time, which follows the frames.
        bool heard = st.state == audio::VoiceState::Playing || st.state == audio::VoiceState::Starved ||
                     st.state == audio::VoiceState::Paused || st.state == audio::VoiceState::Finished;
        bool deviceClock = as.deviceOpen && !s_.ctx_->screenshotMode && heard;
        double t;
        if (deviceClock) {
            double latency = as.sampleRate > 0 ? double(as.bufferFrames) / double(as.sampleRate) : 0.0;
            t = std::max(0.0, st.played - latency);
        } else {
            t = std::min(r.voiceGameT, r.voiceQueued);
        }
        bool done;
        if (s_.ctx_->screenshotMode || !as.deviceOpen) {
            done = r.voiceGameT >= r.voiceQueued;
        } else {
            done = st.state == audio::VoiceState::Finished || st.state == audio::VoiceState::Stopped ||
                   st.state == audio::VoiceState::Dropped || st.state == audio::VoiceState::None ||
                   r.voiceGameT > r.voiceQueued + 1.0;   // watchdog (a voice left Pending)
        }
        if (done) {
            if (finished) *finished = true;
            t = r.voiceQueued;
        }
        return t;
    }

    // ---- Subtitles
    void showSubtitle(const std::string& written, float holdSeconds) override {
        CoachRuntime& r = rt();
        LOGD("coach: subtitle \"%s\" (%.1f s)", written.c_str(), holdSeconds);
        r.subText = written;
        r.subAge = 0.0f;
        r.subHold = holdSeconds;
    }
    float readingTime(const std::string& written) const override { return ui::subtitleDuration(written, 0.0f); }

    // ---- Body
    void look(coach::Look look, Square target) override {
        CoachRuntime& r = rt();
        r.look = look;
        r.lookSquare = target;
        r.lookHold = 2.0f;
    }
    vec3 squarePoint(Square sq) const { return s_.board_.squareBase(sq) + vec3(0.0f, 0.004f, 0.0f); }
    vec3 objectPoint(coach::TableObject o) const {
        Color mine = s_.colorOfSeat(s_.aiSeat());
        switch (o) {
        case coach::TableObject::Clock:
            return transformPoint(s_.world_.clockTransform(), vec3(0.0f, layout::CLOCK_HEIGHT, 0.0f));
        case coach::TableObject::CoachSheet: return s_.glanceTarget(s_.aiSeat());
        case coach::TableObject::PlayerSheet: return s_.glanceTarget(s_.humanSeat());
        case coach::TableObject::CaptureArea: return s_.board_.captureSlot(mine, 2);
        case coach::TableObject::Reserve: return s_.board_.reserveSlot(mine);
        default: return vec3(0.0f, layout::BOARD_TOP_Y, 0.0f);
        }
    }
    vec3 listenerEyes() const { return s_.anim_[s_.humanSeat()].eyeCameraTransform().c[3].xyz(); }
    void gesture(const coach::Gesture& g, float apexIn) override {
        CoachRuntime& r = rt();
        anim::Animator& a = hand();
        float now = a.time();
        auto startAt = [&](float approach) {
            float t = now + std::max(0.0f, apexIn) - approach;
            return t > now + 0.01f ? t : -1.0f;
        };
        anim::Task t;
        switch (g.kind) {
        case coach::GestureKind::PointSquare:
        case coach::GestureKind::PointPiece:
        case coach::GestureKind::PointObject: {
            t.type = anim::TaskType::Point;
            if (g.kind == coach::GestureKind::PointObject) {
                t.position = objectPoint(g.object);
            } else {
                if (g.square == NoSquare) return;
                int id = g.kind == coach::GestureKind::PointPiece ? s_.board_.idAt(g.square) : -1;
                if (id >= 0) t.pieceId = id;
                else t.position = squarePoint(g.square);
            }
            t.emphasis = g.emphasis;
            t.gazeHold = 1.2f;
            t.notBefore = startAt(kGestureApproach);
            break;
        }
        case coach::GestureKind::Trace: {
            if (g.path.empty()) return;
            t.type = anim::TaskType::Trace;
            if (g.path.size() == 2 && g.path[0] != NoSquare && g.path[1] != NoSquare) {
                t.path = anim::moveTracePath(g.path[0], g.path[1]);   // a knight's jump: its L
            } else {
                for (Square sq : g.path)
                    if (sq != NoSquare) t.path.push_back(squarePoint(sq));
            }
            if (t.path.empty()) return;
            t.notBefore = startAt(kGestureApproach);
            break;
        }
        case coach::GestureKind::Present:
            t.type = anim::TaskType::Gesture;
            t.shape = anim::HandShape::Present;
            t.position = g.square != NoSquare ? squarePoint(g.square) : vec3(0.0f);
            t.gazeHold = 1.0f;
            t.notBefore = startAt(kGestureApproach);
            break;
        case coach::GestureKind::Beat:
            t.type = anim::TaskType::Gesture;
            t.shape = anim::HandShape::Beat;
            t.duration = kBeatDuration;
            t.notBefore = startAt(0.6f * kBeatDuration / 3.0f);
            break;
        case coach::GestureKind::Open:
            t.type = anim::TaskType::Gesture;
            t.shape = anim::HandShape::Open;
            t.position = listenerEyes();
            t.notBefore = startAt(kGestureApproach);
            break;
        case coach::GestureKind::Nod:
        case coach::GestureKind::ShakeHead:
            r.headMoves.push_back({s_.time_ + std::max(0.0f, apexIn - kNodApex), g.kind == coach::GestureKind::Nod});
            return;
        }
        a.enqueue(t);
        r.handOut = true;
    }
    void endGestures() override {
        CoachRuntime& r = rt();
        anim::Animator& a = hand();
        a.endHold();
        if (r.handOut) a.enqueue(task(anim::TaskType::Retract));
        r.handOut = false;
    }
    void speechLevel(float level) override {
        CoachRuntime& r = rt();
        r.level = std::clamp(level, 0.0f, 1.0f);
        hand().setSpeechLevel(r.level);
    }

    // ---- The table
    void demoMove(const std::string& uci, float pause) override {
        TableJob j;
        j.kind = TableJob::Kind::Demo;
        j.uci = uci;
        j.pause = std::max(0.0f, pause);
        rt().jobs.push_back(j);
    }
    void rewindDemo(int plies, bool fast) override {
        CoachRuntime& r = rt();
        // A rewind asked for while one runs (Space): the one running goes faster.
        if (fast) hurryTable();
        if (plies <= 0) return;
        TableJob j;
        j.kind = TableJob::Kind::Rewind;
        j.plies = plies;
        j.fast = fast;
        r.jobs.push_back(j);
    }
    // Space during a rewind: the rest of it (and the rewinds queued) at the pace of play.
    void hurryTable() override {
        CoachRuntime& r = rt();
        if (r.jobRunning && (r.job.kind == TableJob::Kind::Rewind || r.job.kind == TableJob::Kind::TakeBack)) r.job.fast = true;
        for (TableJob& j : r.jobs)
            if (j.kind == TableJob::Kind::Rewind) j.fast = true;
    }
    void takeBack(int plies) override {
        if (plies <= 0) return;
        TableJob j;
        j.kind = TableJob::Kind::TakeBack;
        j.plies = plies;
        rt().jobs.push_back(j);
    }
    void setPosition(const std::string& fen) override {
        TableJob j;
        j.kind = TableJob::Kind::SetPosition;
        j.fen = fen;
        rt().jobs.push_back(j);
    }
    void playLessonMove(const std::string& uci) override {
        TableJob j;
        j.kind = TableJob::Kind::LessonMove;
        j.uci = uci;
        rt().jobs.push_back(j);
    }
    bool tableBusy() const override { return rt().jobRunning || !rt().jobs.empty(); }
    bool bodyBusy() const override {
        return hand().busy() || tableBusy() || s_.turn_ == GameScene::Turn::AiMoving || !rt().headMoves.empty();
    }

    // ---- HUD
    void showTakebackOffer(bool shown) override { rt().offerShown = shown; }
    void showSkipHint(bool shown) override { rt().skipHint = shown; }
    void prewarmGlyphs(const std::string& written) override {
        std::vector<std::pair<int, uint32_t>> glyphs;
        for (char32_t c : uni::shapeArabic(uni::decode(written)).text)  // the forms the shaper draws
            if (c > 0x20) glyphs.push_back({ui::font::FACE_TEXT, uint32_t(c)});
        if (!glyphs.empty()) ui::font::prewarm(glyphs);
    }

private:
    GameScene& s_;
};

// ==============================================================================================
// The Analyst
// ==============================================================================================

class CoachAnalyst final : public coach::Analyst {
public:
    explicit CoachAnalyst(GameScene& s) : s_(s) {}
    uint32_t analyse(const ai::AnalysisRequest& request) override {
        if (!s_.engineOk_) return 0;
        uint32_t id = s_.engine_.requestAnalysis(request);
        if (id) s_.coach_->analyses.insert(id);
        return id;
    }
    bool takeAnalysis(uint32_t id, ai::Analysis& out) override {
        std::set<uint32_t>& mine = s_.coach_->analyses;
        if (!mine.count(id)) return false;
        // Screenshot runs wait for the engine, as the warp does: the frames do not follow the clock.
        if (s_.ctx_->screenshotMode)
            for (int i = 0; i < 6000 && !s_.engine_.analysisReady(id) && s_.engine_.analysisPending(id); ++i) plat::sleepMs(5);
        if (!s_.engine_.takeAnalysis(id, out)) return false;
        mine.erase(id);
        return true;
    }
    void stopAnalysis(uint32_t id) override {
        if (s_.coach_->analyses.count(id)) s_.engine_.stopAnalysis(id);
    }
    void cancelAnalysis(uint32_t id) override {
        std::set<uint32_t>& mine = s_.coach_->analyses;
        if (id == 0) {
            for (uint32_t a : mine) s_.engine_.cancelAnalysis(a);
            mine.clear();
        } else if (mine.count(id)) {
            s_.engine_.cancelAnalysis(id);
            mine.erase(id);
        }
    }
    bool idle() const override { return s_.engine_.idle(); }

private:
    GameScene& s_;
};

CoachRuntime::CoachRuntime(GameScene& s) : stage(new CoachStage(s)), analyst(new CoachAnalyst(s)) {}

CoachRuntime::~CoachRuntime() {
    if (warm.joinable()) warm.join();
    worker.stop();
}

// ==============================================================================================
// Set-up
// ==============================================================================================

GameScene::GameScene() = default;

GameScene::~GameScene() {
    if (coach_) coach_->session.stop();
}

bool GameScene::legalHints() const { return settings().showLegalMoves || lesson(); }

void GameScene::initCoachArgs() {
    coachArgs_ = parseCoachArgs(ctx_->args);
    for (const std::string& p : coachArgs_.problems) LOGW("command line: %s", p.c_str());
    if (!coachArgs_.dir.empty()) tts::setModelDirectory(coachArgs_.dir);
    coachModelInit();   // the voice model download: the Coach entry's prompt (coach_model.h)
    coachVoiceFiles_ = coachVoiceWanted();
    LOGI("coach: voice files %s in %s", coachVoiceFiles_ ? "found" : "missing (subtitles only)", tts::modelDirectory().c_str());
}

void GameScene::refreshCoachVoice() {
    // The model files may come and go while the game runs (downloaded from the menu), and the
    // voice can be switched off (Options > Audio > Coach voice). Files that are all there but do
    // not load are checked by the next download (coach_model.h).
    if (coachVoiceFiles_ && coach_ && coach_->workerStarted && coach_->worker.failed()) coachModelLoadFailed();
    coachVoiceFiles_ = coachVoiceWanted();
}

bool GameScene::coachVoiceExpected() const {
    return coachVoiceFiles_ && !(coach_ && coach_->workerStarted && coach_->worker.failed());
}

CoachRuntime& GameScene::coachRuntime() {
    if (coach_) return *coach_;
    coach_.reset(new CoachRuntime(*this));
    CoachRuntime& rt = *coach_;
    // The catalog on this thread (a few ms; Session::start installs its opening resolver); the
    // opening book and texts are built on a thread of their own (~40 ms) before the first move
    // needs them.
    coach::Catalog::shared();
    rt.warm = std::thread([] {
        coach::OpeningBook::instance();
        coach::OpeningTexts::instance();
    });
    rt.stage->ensureWorker(1.0f);   // the voice loads while the lights go down
    return rt;
}

void GameScene::setupCoachGame() {
    refreshCoachVoice();
    CoachRuntime& rt = coachRuntime();
    leaveCoachGame();
    const Settings& s = settings();
    coachLevel_ = coachArgs_.level >= 0 ? coachArgs_.level : std::clamp(s.coachLevel, 0, ai::kCoachLevels - 1);
    int colour = coachArgs_.colour >= 0 ? coachArgs_.colour
                 : s.coachColour == 0 || s.coachColour == 1 ? s.coachColour
                                                             : (s.coachNextColour == 1 ? 1 : 0);
    rt.test = coachArgs_.stageTest;
    // The rules lesson is played with White; so is the stage test unless told otherwise (its line
    // points at White's g1 knight).
    humanColor_ = coachLevel_ == 0 || (rt.test && coachArgs_.colour < 0) ? White : Color(colour);
    rt.seed = (uint64_t(rng_.next()) << 32) | rng_.next();
    rt.testKind = ctx_->argValue("--coach-stage-test");
    if (rt.testKind.compare(0, 2, "--") == 0) rt.testKind.clear();
    rt.testStep = 0;
    rt.testTime = 0.0f;
    rt.forceSubtitles = rt.test;
    LOGI("New game (coach): level %d%s, human plays %s%s", coachLevel_, coachLevel_ == 0 ? " (the rules lesson)" : "",
         humanColor_ == White ? "White" : "Black", rt.test ? ", stage test" : "");
}

void GameScene::configureCoachSeats() {
    const Settings& s = settings();
    for (int i = 0; i < 2; ++i) {
        Seat& st = seats_[i];
        character::Side hand = st.playHand;  // set by initAnimators()
        st = Seat();
        st.color = colorOfSeat(i);
        st.playHand = hand;
        if (st.color == humanColor_) {
            st.controller = Controller::Human;
            st.name = playerName();
            st.elo = s.playerElo;
            st.provisional = s.playerRecord().provisional();
        } else {
            // The coach: its name in the interface language on the scoresheets and in the PGN, the
            // rating of its level. Its searches keep the player's threads and hash; no humanised
            // thinking time (the coach's pace follows its speech).
            st.controller = Controller::Stockfish;
            st.name = i18n::tr("menu.coach");
            st.presetName = "Coach";
            st.engine = ai::coachLevelSettings(coachLevel_);
            st.engine.threads = std::max(1, s.engineThreads);
            st.engine.hashMB = std::max(16, s.engineHashMB);
            st.engine.humanize = false;
            st.elo = ai::coachLevelElo(coachLevel_);
        }
    }
}

void GameScene::startCoachGame() {
    CoachRuntime& rt = coachRuntime();
    Settings& s = settings();
    // Alternating colours: the next coach game is played with the other colour.
    if (coachLevel_ > 0 && coachArgs_.colour < 0 && s.coachColour == 2 && !rt.test) {
        s.coachNextColour = humanColor_ == White ? 1 : 0;
        s.save();
    }
    rt.handshakeReported = rt.resultsSaved = false;
    rt.demoPos = game_.position();
    rt.demoBefore.clear();
    coach::SessionConfig c;
    c.level = coachLevel_;
    c.human = humanColor_;
    c.director.uiLanguage = i18n::language();
    c.director.subtitles = s.subtitles;
    c.director.speed = coachLevel_ == 0 ? coach::kLessonSpeechSpeed : 1.0f;
    c.introduceLevel = s.coachHistory.empty() || s.coachHistory.back().level != coachLevel_;
    c.offersEnabled = true;
    c.seed = rt.seed;
    for (const Settings::CoachGame& g : s.coachHistory) c.history.push_back({g.level, g.result, g.accuracy});
    c.accuracyExplained = s.coachAccuracyExplained;
    c.lessonChapter = s.coachLessonChapter;
    rt.stage->ensureWorker(c.director.speed);
    // Every glyph the coach's lines can show in this language, once (subtitles never wait).
    std::string ui = i18n::language();
    if (!rt.prewarmedLanguages.count(ui)) {
        rt.prewarmedLanguages.insert(ui);
        std::vector<std::pair<int, uint32_t>> glyphs;
        for (uint32_t cp : coach::Catalog::shared().codepoints(ui)) glyphs.push_back({ui::font::FACE_TEXT, cp});
        for (char32_t cp : uni::shapeArabic(uni::decode(i18n::tr("coach.speaker"))).text)
            glyphs.push_back({ui::font::FACE_TITLE, uint32_t(cp)});
        ui::font::prewarm(glyphs);
    }
    if (rt.test) {
        LOGI("coach stage test: the session stays silent, the stage performs its sequence");
        return;
    }
    rt.session.start(*rt.stage, *rt.analyst, game_, c);
    rt.sessionRunning = true;
}

void GameScene::leaveCoachGame() {
    if (!coach_) return;
    CoachRuntime& rt = *coach_;
    if (rt.sessionRunning) {
        if (lesson() && !rt.resultsSaved) {
            // The lesson resumes where the player left it.
            settings().coachLessonChapter = rt.session.lessonCompleted() ? 0 : rt.session.lessonChapter();
            settings().save();
        }
        rt.session.stop();
        rt.sessionRunning = false;
    }
    rt.analyst->cancelAnalysis(0);
    if (rt.drawAnalysis && engineOk_) engine_.cancelAnalysis(rt.drawAnalysis);
    rt.drawAnalysis = 0;
    rt.stage->cancelSpeech(0);
    rt.stage->stopVoice();
    rt.voiceStarted = false;
    rt.subText.clear();
    rt.offerShown = rt.skipHint = false;
    rt.jobs.clear();
    rt.jobRunning = false;
    rt.plies.clear();
    rt.trips.clear();
    rt.demoBefore.clear();
    rt.testMarks.clear();
    rt.markPieces.clear();
    rt.headMoves.clear();
    rt.handOut = false;
    rt.test = false;
    rt.forceSubtitles = false;
    coachFade_ = 0.0f;
    coachFaceLift_ = 0.0f;
}

void GameScene::shutdownCoach() {
    if (!coach_) return;
    leaveCoachGame();
    coach_->worker.stop();   // before audio::shutdown()
    if (coach_->warm.joinable()) coach_->warm.join();
}

// ==============================================================================================
// Flow
// ==============================================================================================

void GameScene::coachMoveCompleted() {
    CoachRuntime& rt = coachRuntime();
    rt.demoPos = game_.position();
    rt.demoBefore.clear();
    if (rt.sessionRunning) rt.session.onMove(game_);
    // The lesson's positions may end (a mate exercise): it has no end-of-game flow of its own.
    if (!lesson() && game_.status() != GameStatus::Ongoing) {
        endGame();
        return;
    }
    beginTurn();
}

void GameScene::coachGameOver() {
    CoachRuntime& rt = coachRuntime();
    rt.handshakeReported = false;
    bool resigned = game_.endReason() == GameEndReason::Resignation;
    if (rt.sessionRunning) rt.session.onGameOver(game_, resigned);
}

void GameScene::endLesson() {
    CoachRuntime& rt = coachRuntime();
    LOGI("coach: the rules lesson is over");
    if (turn_ == Turn::HumanTouched) humanRelease();
    turn_ = Turn::None;
    state_ = State::GameOver;
    stateTime_ = 0.0f;
    gameOverShown_ = false;
    endHandshakeDone_ = false;
    rt.handshakeReported = false;
    resultText_ = i18n::tr("coach.lesson.done");
    reasonText_ = i18n::tr("coach.level.0.name");
    playerWon_ = true;
    isDraw_ = false;
    // The next coach game is the first real one (W8: the lesson's end card offers it).
    Settings& s = settings();
    s.coachRulesDone = true;
    s.coachLevel = 1;
    s.coachLessonChapter = 0;
    s.save();
    coachArgs_.level = -1;
}

bool GameScene::coachHandshakeWanted() const {
    if (!coach_ || !coach_->sessionRunning) return true;
    if (coach_->jobRunning || !coach_->jobs.empty()) return false;   // the table is put back first
    return coach_->session.handshakeWanted() || stateTime_ > kHandshakeFailsafe;
}

bool GameScene::coachEndCardReady() const {
    if (stateTime_ < 1.2f) return false;
    if (!coach_ || !coach_->sessionRunning) return endHandshakeDone_ || stateTime_ > 5.0f;
    return (coach_->handshakeReported && coach_->session.finished()) || stateTime_ > kEndCardFailsafe;
}

void GameScene::persistCoachResults() {
    if (!coach_ || coach_->resultsSaved || !coach_->sessionRunning) return;
    CoachRuntime& rt = *coach_;
    rt.resultsSaved = true;
    Settings& s = settings();
    s.coachHistory.clear();
    for (const coach::GameRecord& g : rt.session.history()) s.coachHistory.push_back({g.level, g.result, g.accuracy});
    if (s.coachHistory.size() > size_t(Settings::kCoachHistoryMax))
        s.coachHistory.erase(s.coachHistory.begin(), s.coachHistory.end() - Settings::kCoachHistoryMax);
    s.coachAccuracyExplained = rt.session.accuracyExplained();
    LOGI("coach: results saved (%d games in the history)", int(s.coachHistory.size()));
    if (lesson()) s.coachLessonChapter = rt.session.lessonCompleted() ? 0 : rt.session.lessonChapter();
    // The level the coach suggested is the Coach page's choice next time ("Play again" keeps this
    // game's level).
    int suggested = rt.session.suggestedLevel();
    if (!lesson() && suggested > 0 && suggested != coachLevel_) {
        LOGI("coach: suggests level %d (was %d)", suggested, coachLevel_);
        s.coachLevel = std::clamp(suggested, 1, ai::kCoachLevels - 1);
    }
    s.save();
}

bool GameScene::coachBookMove(Move& mv) {
    if (!coach_ || coachLevel_ <= 0) return false;
    Move m = coach::repertoireMove(game_, coachLevel_, coach_->seed);
    if (!m.valid()) return false;
    mv = m;
    return true;
}

bool GameScene::coachHoldsMove() const {
    if (!coach_) return false;
    const CoachRuntime& rt = *coach_;
    if (rt.test) return true;   // the stage test owns the table
    if (rt.sessionRunning && !rt.session.coachMayMove()) return true;
    if (rt.jobRunning || !rt.jobs.empty()) return true;
    // Its hand first finishes what it shows (a gesture's hold ends with the line, see updateCoach).
    return anim_[aiSeat()].busy();
}

bool GameScene::coachMayTouch() const {
    if (!coach_) return true;
    const CoachRuntime& rt = *coach_;
    if (rt.jobRunning || !rt.jobs.empty() || rt.offerShown) return false;
    // The session's word: the human's turn without the takeback card; the lesson, while an
    // exercise waits for the move (between them the coach has the floor).
    if (rt.sessionRunning) return rt.session.playerMayMove(game_);
    return !rt.test;
}

void GameScene::coachPlayerTouched() {
    if (coach_ && coach_->sessionRunning) coach_->session.onPlayerActive();
}

void GameScene::coachIllegalAttempt(Square from, Square to) {
    LOGI("coach: %s-%s refused (not a legal move)", squareName(from).c_str(), squareName(to).c_str());
    if (coach_ && coach_->sessionRunning) coach_->session.onIllegalAttempt(game_, from, to);
}

bool GameScene::coachCanTakeBack() const {
    if (!coach_ || !coach_->sessionRunning || lesson()) return false;
    const CoachRuntime& rt = *coach_;
    if (rt.jobRunning || !rt.jobs.empty() || turn_ == Turn::AiMoving || turn_ == Turn::HumanPlacing ||
        turn_ == Turn::HumanPromotion || turn_ == Turn::HumanPlaced || !rt.session.canTakeBack(game_))
        return false;
    // Back to the player's last move: possible only while neither scoresheet has begun writing it
    // (the write limit keeps it and the coach's reply off the sheets until the player's next move;
    // lead decision §4.2).
    int n = int(game_.moves().size());
    int last = n - 1;
    if (last >= 0 && game_.positionAt(size_t(last)).sideToMove() != humanColor_) --last;
    if (last < 0) return false;
    const ScoreLedger& l = scorekeeper_.ledger();
    return l.next(0) <= last && l.next(1) <= last;
}

void GameScene::coachOfferDraw() {
    CoachRuntime& rt = coachRuntime();
    int ply = int(game_.moves().size());
    if (drawOfferPly_ == ply || rt.drawAnalysis) {
        ui::notify(i18n::tr("notify.draw_already_offered"), 2.5f);
        return;
    }
    drawOfferPly_ = ply;
    // The coach answers from a full-strength evaluation of the position (its own play is weakened).
    ai::AnalysisRequest r;
    if (game_.startPosition().fen() != Position().fen()) r.startFen = game_.startPosition().fen();
    r.moves = game_.uciMoves();
    r.multiPV = 1;
    r.depth = 12;
    r.moveTimeMs = 1000;
    r.priority = 2;
    rt.drawAnalysis = engineOk_ ? engine_.requestAnalysis(r) : 0;
    rt.drawPly = ply;
    if (!rt.drawAnalysis) {
        ui::notify(i18n::tr("notify.draw_declined"), 3.0f);
        if (rt.sessionRunning) rt.session.onDrawAnswer(false);
    }
}

// ==============================================================================================
// Every frame
// ==============================================================================================

void GameScene::updateCoach(float dt) {
    if (!coach() || !coach_) return;
    CoachRuntime& rt = *coach_;
    CoachStage& stage = *rt.stage;
    const bool frozen = paused_ && state_ == State::Playing;
    const bool focusLost = !plat::hasFocus() && !ctx_->screenshotMode;
    const bool hold = frozen || focusLost;
    if (hold != rt.held) {
        // The pause menu, or the window in the background: the voice pauses, the director holds
        // between beats (the game has no clock to stop; lead decision §4.3).
        rt.held = hold;
        if (rt.sessionRunning) rt.session.setPaused(hold);
        rt.voicePausedByScene = hold;
        stage.applyPause();
    }
    if (engineOk_) engine_.idle();   // pumps the engine's queue (analyses start from a call)

    if (!hold) {
        if (rt.voiceActive) rt.voiceGameT += dt;
        rt.subAge += dt;
    }
    if (!rt.subText.empty() && rt.subAge > rt.subHold + 0.4f) rt.subText.clear();
    if (rt.voiceActive) {
        bool finished = false;
        stage.voiceClock(&finished);
        if (finished) {
            // Heard to the end by the game's clock (no device, screenshot runs, the watchdog): the
            // audio engine may still hold it (Pending without a device), which would keep its slot.
            audio::VoiceState vs = audio::voiceStatus(rt.voice).state;
            if (vs != audio::VoiceState::Finished && vs != audio::VoiceState::Stopped && vs != audio::VoiceState::Dropped &&
                vs != audio::VoiceState::None)
                audio::stopVoice(rt.voice, 0.02f);
            rt.voiceActive = false;
        }
    }

    // The session: what the coach says and does (not while paused). It hears how long the engine
    // has been searching the coach's move (a filler line after a long search).
    if (rt.sessionRunning && !hold && (state_ == State::Playing || state_ == State::GameOver)) {
        bool searching = state_ == State::Playing && turn_ == Turn::AiThinking && aiRequested_ && !aiHasMove_;
        rt.session.onCoachThinking(searching ? aiElapsed_ : 0.0f);
        rt.session.update(game_, dt);
    }
    if (rt.test && !hold && state_ == State::Playing) runStageTest(dt);
    if (!frozen) runCoachTable(dt);
    // A dip to hide a correction of the table fades back (a lesson set-up runs its own fade).
    if (!(rt.jobRunning && rt.job.kind == TableJob::Kind::SetPosition) && coachFade_ > 0.0f)
        coachFade_ = std::max(0.0f, coachFade_ - dt / kSetupFadeIn);

    // --coach-auto-answer: the card answers itself (scripted runs, where nobody clicks).
    bool offerUp = rt.offerShown || (rt.sessionRunning && rt.session.offerOpen());
    rt.offerAge = offerUp && !hold ? rt.offerAge + dt : offerUp ? rt.offerAge : 0.0f;
    if (offerUp && coachArgs_.autoAnswer >= 0 && rt.offerAge > 1.5f && state_ == State::Playing) {
        LOGI("coach: takeback offer %s (--coach-auto-answer)", coachArgs_.autoAnswer ? "accepted" : "declined");
        rt.offerShown = false;
        rt.offerAge = 0.0f;
        if (rt.sessionRunning) rt.session.onOfferAnswer(game_, coachArgs_.autoAnswer == 1);
    }

    // The draw offer's evaluation.
    if (rt.drawAnalysis && engine_.analysisReady(rt.drawAnalysis)) {
        ai::Analysis a;
        bool accept = false;
        if (engine_.takeAnalysis(rt.drawAnalysis, a) && a.ok && !a.lines.empty() && rt.drawPly == int(game_.moves().size())) {
            const ai::Score& sc = a.lines[0].score;
            bool coachToMove = game_.position().sideToMove() != humanColor_;
            int cp = sc.mate != 0 ? (sc.mate > 0 ? 100000 : -100000) : sc.cp;
            if (sc.matedNow) cp = -100000;
            int coachCp = coachToMove ? cp : -cp;
            accept = engine_.acceptsDraw(coachCp, int(game_.moves().size()));
            LOGI("coach: draw offer %s (%d cp for the coach)", accept ? "accepted" : "declined", coachCp);
        }
        rt.drawAnalysis = 0;
        if (state_ == State::Playing && rt.drawPly == int(game_.moves().size())) {
            ui::notify(i18n::tr(accept ? "notify.draw_accepted" : "notify.draw_declined"), 3.0f);
            if (rt.sessionRunning) rt.session.onDrawAnswer(accept);
            if (accept) {
                game_.agreeDraw();
                endGame();
            }
        }
    }

    // The coach's body: head gestures on their word, a blink at each phrase end, the look.
    anim::Animator& a = anim_[aiSeat()];
    for (size_t i = 0; i < rt.headMoves.size();) {
        if (time_ >= rt.headMoves[i].first) {
            if (rt.headMoves[i].second) a.nod();
            else a.shakeHead();
            rt.headMoves.erase(rt.headMoves.begin() + long(i));
        } else {
            ++i;
        }
    }
    if (!rt.voiceActive && rt.level > 0.0f) stage.speechLevel(0.0f);
    if (rt.level > 0.15f) {
        rt.loud = true;
        rt.quiet = 0.0f;
    } else if (rt.loud && (rt.quiet += dt) > 0.15f) {
        a.blink();
        rt.loud = false;
    }
    bool talking = rt.voiceActive || (rt.sessionRunning && !rt.session.director().idle()) || !rt.subText.empty();
    if (talking) rt.lookHold = std::max(rt.lookHold, 0.8f);
    else rt.lookHold = std::max(0.0f, rt.lookHold - dt);
    // A gesture's hold outlives the line only when nobody ends it: the hand goes back to rest.
    if (rt.handOut && !talking && !rt.jobRunning && rt.jobs.empty() &&
        (a.runningTask(anim::TaskType::Point) || a.runningTask(anim::TaskType::Trace) || a.runningTask(anim::TaskType::Gesture)))
        stage.endGestures();

    // The view rises gently to the coach's face while it talks to the player, once the pointer has
    // rested for a moment and nothing is in hand (lead decision §4.1).
    const plat::Input& in = plat::input();
    vec2 p = cursorPixels();
    if (length(p - rt.lastPointer) > 2.0f || in.mouseDown[plat::MOUSE_RIGHT]) rt.pointerIdle = 0.0f;
    else rt.pointerIdle += dt;
    rt.lastPointer = p;
    bool aiming = turn_ == Turn::HumanTouched || turn_ == Turn::HumanPlacing || turn_ == Turn::HumanPromotion;
    bool lift = state_ == State::Playing && !paused_ && rt.voiceActive && rt.look == coach::Look::Player && !aiming &&
                !dragging_ && !glance_ && rt.pointerIdle > kFaceLiftIdle;
    coachFaceLift_ = clamp(coachFaceLift_ + (lift ? dt / 1.6f : -dt / 0.5f), 0.0f, 1.0f);

    // The voice comes from the coach's mouth, turned where its head turns.
    if (rt.voiceActive) {
        vec3 pos, facing;
        stage.mouth(pos, facing);
        audio::setVoicePose(rt.voice, pos, facing);
    }
    // Board coordinates: the option, forced on for the lesson and the first levels (§4.4).
    world_.setBoardCoordinates(settings().showCoordinates || coachLevel_ <= 2);

    // The end of the game: the handshake once wanted (simulate enqueues it), then the appraisal.
    if (state_ == State::GameOver && endHandshakeDone_ && !rt.handshakeReported && !anim_[0].busy() && !anim_[1].busy() &&
        stateTime_ > 1.0f) {
        rt.handshakeReported = true;
        LOGI("coach: handshake done, the appraisal follows");
        if (rt.sessionRunning) rt.session.onHandshakeDone(game_);
    }
    if (state_ == State::GameOver && coachEndCardReady()) persistCoachResults();
    if (lesson() && state_ == State::Playing && rt.sessionRunning && rt.session.handshakeWanted()) endLesson();
}

void GameScene::coachGazeTarget(vec3& target) {
    if (!coach_ || state_ == State::Menu || state_ == State::Loading) return;
    CoachRuntime& rt = *coach_;
    if (rt.jobRunning && aiMoveTo_ != NoSquare &&
        (rt.job.kind == TableJob::Kind::Demo || rt.job.kind == TableJob::Kind::LessonMove)) {
        target = board_.squareBase(aiMoveTo_) + vec3(0.0f, 0.02f, 0.0f);   // its hand's move
        return;
    }
    if (rt.lookHold <= 0.0f) return;
    switch (rt.look) {
    case coach::Look::Player: target = anim_[humanSeat()].eyeCameraTransform().c[3].xyz(); break;
    case coach::Look::Board: target = vec3(0.0f, layout::BOARD_TOP_Y + 0.02f, 0.0f); break;
    case coach::Look::Target:
        if (rt.lookSquare != NoSquare) target = board_.squareBase(rt.lookSquare) + vec3(0.0f, 0.02f, 0.0f);
        break;
    }
}

// ==============================================================================================
// Input and HUD
// ==============================================================================================

void GameScene::coachHudFrame() {
    CoachRuntime& rt = coachRuntime();
    ui::CoachHud hud;
    hud.offer = rt.offerShown || (rt.sessionRunning && rt.session.offerOpen());
    hud.skippable = rt.skipHint || (rt.sessionRunning && rt.session.director().skippable());
    bool cardUp = state_ == State::GameOver && gameOverShown_ && !ui::gameOverFolded();
    if (cardUp) hud.skippable = false;
    ui::CoachHudAction act = ui::coachHud(hud);
    const plat::Input& in = plat::input();
    // Backspace takes the move back; touching one of your pieces plays on (the card's own words).
    if (hud.offer && act == ui::CoachHudAction::None && !ui::wantsKeyboard() && in.keyPressed[plat::KEY_BACKSPACE])
        act = ui::CoachHudAction::TakeBack;
    if (hud.offer && act == ui::CoachHudAction::None && in.mousePressed[plat::MOUSE_LEFT] && !ui::wantsMouse() && !dragging_) {
        int pid = pickPiece(mouseRay());
        const PieceObject* p = pid >= 0 ? board_.byId(pid) : nullptr;
        if (p && p->color == humanColor_) {
            // Touching a piece plays on (the session's own "Let's play on"); the touch itself then
            // goes through below, the card gone.
            LOGI("coach: takeback offer declined (a piece touched)");
            rt.offerShown = false;
            if (rt.sessionRunning) rt.session.onPlayerActive();
        }
    }
    if (act != ui::CoachHudAction::None) {
        bool accept = act == ui::CoachHudAction::TakeBack;
        LOGI("coach: takeback offer %s", accept ? "accepted" : "declined");
        rt.offerShown = false;
        if (rt.sessionRunning) rt.session.onOfferAnswer(game_, accept);
    }
    // Space skips what the coach says (never the clock: a coach game has none to press).
    if (!cardUp && !ui::wantsKeyboard() && in.keyPressed[plat::KEY_SPACE]) {
        if (rt.sessionRunning) rt.session.skip();
        if (rt.test) rt.stage->hurryTable();
    }
}

void GameScene::coachPauseMenuFrame() {
    CoachRuntime& rt = coachRuntime();
    ui::CoachPause cp;
    cp.canTakeBack = coachCanTakeBack();
    cp.canOfferDraw = !lesson() && drawOfferPly_ != int(game_.moves().size()) && !rt.drawAnalysis;
    cp.canClaimDraw = !lesson() && (game_.canClaimThreefold() || game_.canClaimFiftyMove());
    cp.canResign = !lesson();
    switch (menuChoice(ui::coachPauseMenu(cp))) {
    case ui::MenuAction::Resume: paused_ = false; break;
    case ui::MenuAction::TakeBack:
        paused_ = false;
        if (turn_ == Turn::HumanTouched) humanRelease();
        if (rt.sessionRunning) rt.session.onTakeBackRequested(game_);
        break;
    case ui::MenuAction::OfferDraw:
        paused_ = false;
        coachOfferDraw();
        break;
    case ui::MenuAction::ClaimDraw:
        paused_ = false;
        game_.claimDraw();
        if (game_.status() != GameStatus::Ongoing) endGame();
        else ui::notify(i18n::tr("notify.no_draw_claim"));
        break;
    case ui::MenuAction::Resign:
        paused_ = false;
        game_.resign(humanColor_);
        endGame();
        break;
    case ui::MenuAction::OptionsChanged: {
        applySettings(true);
        if (rt.sessionRunning) {
            coach::DirectorConfig c = rt.session.director().config();
            c.subtitles = settings().subtitles;
            c.uiLanguage = i18n::language();
            rt.session.director().setConfig(c);
        }
        break;
    }
    case ui::MenuAction::BackToMainMenu:
        // A coach game is never rated: leaving abandons it (the lesson resumes at its chapter).
        // It is saved unfinished ("*"; never the rules lesson).
        paused_ = false;
        archiveGame(game_.isOver());
        leaveCoachGame();
        clock_.stop();
        state_ = State::FadeToMenu;
        stateTime_ = 0.0f;
        break;
    default: break;
    }
}

void GameScene::updateCoachInput() {
    const plat::Input& in = plat::input();
    coachRuntime();
    // Esc opens the pause menu; once open, the menu handles Esc itself (back / resume).
    if (!paused_ && in.keyPressed[plat::KEY_ESCAPE] && turn_ != Turn::HumanPromotion) {
        paused_ = true;
        if (dragging_) {
            dragging_ = false;
            plat::setMouseCaptured(false);
        }
    }
    if (paused_) {
        coachPauseMenuFrame();
        return;
    }
    coachHudFrame();
    if (in.keyPressed[plat::KEY_TAB] && !ui::wantsKeyboard()) showMoveList_ = !showMoveList_;
    if (state_ == State::Playing && isHumanTurn()) updateHumanInput();
}

void GameScene::updateCoachGameOver() {
    if (!coach_) return;
    coachHudFrame();
}

// ==============================================================================================
// Marks, subtitles
// ==============================================================================================

void GameScene::coachMarks(std::vector<PieceHighlight>& highlights, std::vector<CoachMark>& marks) {
    if (!coach_ || state_ == State::Menu || state_ == State::Loading) return;
    CoachRuntime& rt = *coach_;
    std::vector<coach::ShownMark> shown;
    if (rt.sessionRunning) shown = rt.session.director().marks();
    for (const TestMark& t : rt.testMarks) {
        coach::ShownMark m = t.mark;
        float in = smooth01((time_ - t.on) / 0.25f), out = 1.0f - smooth01((time_ - t.off) / 0.4f);
        m.strength = std::min(in, out);
        m.age = std::max(0.0f, time_ - t.on);
        if (m.strength > 0.0f) shown.push_back(m);
    }
    std::set<int> used;
    for (const coach::ShownMark& m : shown) {
        if (m.strength <= 0.0f) continue;
        CoachMark c;
        c.strength = m.strength;
        c.age = m.age;
        switch (m.kind) {
        case coach::Mark::Kind::Square:
            c.kind = CoachMark::Square;
            c.sq = m.square;
            marks.push_back(c);
            break;
        case coach::Mark::Kind::Arrow:
            c.kind = CoachMark::Arrow;
            c.from = m.from;
            c.via = m.via;
            c.to = m.to;
            marks.push_back(c);
            break;
        case coach::Mark::Kind::Piece: {
            // The piece is known by its id from its first frame on, so the light stays on it while
            // a hand lifts it; its square is outlined too (W3: unmistakable on both colours).
            if (m.square == NoSquare) break;
            used.insert(int(m.square));
            auto it = rt.markPieces.find(int(m.square));
            int id = it != rt.markPieces.end() ? it->second : board_.idAt(m.square);
            if (it == rt.markPieces.end() && id >= 0) rt.markPieces[int(m.square)] = id;
            const PieceObject* p = id >= 0 ? board_.byId(id) : nullptr;
            if (p) highlights.push_back({id, m.strength});
            c.kind = CoachMark::Square;
            c.sq = p && p->square != NoSquare ? p->square : m.square;
            marks.push_back(c);
            break;
        }
        }
    }
    for (auto it = rt.markPieces.begin(); it != rt.markPieces.end();) {
        if (!used.count(it->first)) it = rt.markPieces.erase(it);
        else ++it;
    }
}

void GameScene::drawCoachSubtitles() {
    if (!coach() || !coach_) return;
    CoachRuntime& rt = *coach_;
    const std::string ui = i18n::language();
    bool shown = rt.forceSubtitles ||
                 coachSubtitlesShown(settings().subtitles, ui, coach::speechLanguage(ui), rt.stage->voiceAvailable());
    bool blocked = paused_ || ui::optionsOpen() || turn_ == Turn::HumanPromotion;
    ui::Subtitle sub;
    if (shown && !blocked && !rt.subText.empty()) {
        sub.text = rt.subText;
        sub.age = rt.subAge;
        sub.duration = rt.subHold;
    }
    if (state_ == State::GameOver && gameOverShown_ && ui::gameOverFolded()) sub.bottom = ui::viewSize().y - 110.0f;
    ui::subtitles(sub);
}

// ==============================================================================================
// The table
// ==============================================================================================

namespace {

// The hand whose half holds an off-board spot (pos.z > 0 is White's, seat 0), or 'fallback'.
int handForTrip(const coach::PieceTrip& t, int fallback) {
    if (t.from.kind != coach::RestKind::Square) return t.from.pos.z > 0.0f ? 0 : 1;
    if (t.to.kind != coach::RestKind::Square) return t.to.pos.z > 0.0f ? 0 : 1;
    return fallback;
}

}  // namespace

void GameScene::runCoachTable(float dt) {
    CoachRuntime& rt = *coach_;
    const int coachSeat = aiSeat();
    anim::Animator& coachHand = anim_[coachSeat];
    auto handsIdle = [&] { return dest_.empty() && !anim_[0].busy() && !anim_[1].busy(); };

    if (!rt.jobRunning) {
        if (rt.jobs.empty()) return;
        // A gesture still held gives way to the table.
        if (rt.handOut) rt.stage->endGestures();
        bool playerBusy = turn_ == Turn::HumanTouched || turn_ == Turn::HumanPlacing || turn_ == Turn::HumanPromotion ||
                          turn_ == Turn::HumanPlaced || turn_ == Turn::HumanPressing || turn_ == Turn::AiMoving;
        if (playerBusy || !handsIdle()) return;
        rt.job = rt.jobs.front();
        rt.jobs.pop_front();
        rt.jobRunning = true;
        rt.phase = 0;
        rt.plies.clear();
        rt.trips.clear();
        rt.tripNext = 0;
        rt.batchSeat = -1;
        if (rt.demoBefore.empty()) rt.demoPos = game_.position();
    }
    TableJob& job = rt.job;
    const bool slow = !job.fast;
    auto finish = [&] {
        rt.jobRunning = false;
        coachHand.enqueue(task(anim::TaskType::Retract));
    };

    switch (job.kind) {
    case TableJob::Kind::Demo: {
        if (rt.phase == 1) {
            if (!handsIdle()) return;   // placed, and the pause is over
            rt.jobRunning = false;
            // The hand goes back to rest unless the table has more for it (a rewind usually
            // follows the line about the demonstration: it reaches out again then).
            if (rt.jobs.empty()) coachHand.enqueue(task(anim::TaskType::Retract));
            return;
        }
        const Position& pos = rt.demoPos;
        Move mv = pos.parseUCI(job.uci);
        if (!mv.valid()) {
            LOGW("coach: demonstration move '%s' is not legal on the table (%s)", job.uci.c_str(), pos.fen().c_str());
            finish();
            return;
        }
        Color side = pos.sideToMove();
        Color coachColor = colorOfSeat(coachSeat);
        int moverId = board_.idAt(mv.from);
        int victimId = board_.idAt(mv.to);
        if (mv.flags & MoveEnPassant) victimId = board_.idAt(Square(mv.to + (side == White ? -8 : 8)));
        PieceObject* mover = board_.byId(moverId);
        if (!mover) {
            LOGW("coach: no piece on the table for the demonstration %s", job.uci.c_str());
            finish();
            return;
        }
        auto d = [&](float slowDuration) { return slow ? slowDuration : 0.0f; };
        std::vector<anim::Task> ts;
        vec3 toPos = jitteredSquare(mv.to);
        ts.push_back(task(anim::TaskType::Reach, moverId, vec3(0), 0.0f, d(kDemoReach)));
        ts.push_back(task(anim::TaskType::Lift, moverId, vec3(0), carryHeight(mover->basePos, toPos, moverId, victimId), d(kDemoLift)));
        ts.push_back(task(anim::TaskType::Carry, moverId, toPos, 0.0f, d(kDemoCarry)));
        if (victimId >= 0) ts.push_back(task(anim::TaskType::TakeCaptured, victimId, vec3(0), 0.0f, d(kDemoTake)));
        ts.push_back(task(anim::TaskType::Place, moverId, toPos, 0.0f, d(kDemoPlace)));
        dest_[moverId].push_back({mv.to, toPos, false});
        if (victimId >= 0) {
            // Victims go beside the board in the coach's own half, the only one its hand reaches.
            vec3 slot = coach::demoCaptureSlot(board_, victimId, coachColor);
            ts.push_back(task(anim::TaskType::Discard, victimId, slot, 0.0f, d(kDemoDiscard)));
            dest_[victimId].push_back({NoSquare, slot, true});
        }
        if (mv.flags & (MoveCastleKing | MoveCastleQueen)) {
            int rank = rankOf(mv.from);
            bool king = (mv.flags & MoveCastleKing) != 0;
            int rookId = board_.idAt(makeSquare(king ? 7 : 0, rank));
            PieceObject* rook = board_.byId(rookId);
            if (rook) {
                Square rookTo = makeSquare(king ? 5 : 3, rank);
                vec3 rp = jitteredSquare(rookTo);
                ts.push_back(task(anim::TaskType::Reach, rookId, vec3(0), 0.0f, d(kDemoReach)));
                ts.push_back(task(anim::TaskType::Lift, rookId, vec3(0), carryHeight(rook->basePos, rp, rookId, moverId), d(kDemoLift)));
                ts.push_back(task(anim::TaskType::Carry, rookId, rp, 0.0f, d(kDemoCarry)));
                ts.push_back(task(anim::TaskType::Place, rookId, rp, 0.0f, d(kDemoPlace)));
                dest_[rookId].push_back({rookTo, rp, false});
            }
        }
        if (mv.promotion != NoPiece) {
            // The pawn goes beside the board in the coach's half, the new piece comes from the
            // captured pieces or the reserve (rewind.h: the rewind brings it back the same way).
            vec3 slot = board_.nextCaptureSlot(coachColor, moverId);
            vec3 sqPos = board_.squareBase(mv.to);
            ts.push_back(task(anim::TaskType::Reach, moverId, vec3(0), 0.0f, d(kDemoReach)));
            ts.push_back(task(anim::TaskType::Lift, moverId, vec3(0), 0.03f, d(kDemoLift)));
            ts.push_back(task(anim::TaskType::Carry, moverId, slot, 0.0f, d(kDemoCarry)));
            ts.push_back(task(anim::TaskType::Place, moverId, slot, 0.0f, d(kDemoPlace)));
            dest_[moverId].push_back({NoSquare, slot, true});
            int spareId = board_.takeSpare(mv.promotion, side);
            PieceObject* spare = board_.byId(spareId);
            if (spare) {
                vec3 target = jitteredSquare(mv.to);
                ts.push_back(task(anim::TaskType::Reach, spareId, vec3(0), 0.0f, d(kDemoReach)));
                ts.push_back(task(anim::TaskType::Lift, spareId, vec3(0), carryHeight(spare->basePos, sqPos, spareId, moverId), d(kDemoLift)));
                ts.push_back(task(anim::TaskType::Carry, spareId, target, 0.0f, d(kDemoCarry)));
                ts.push_back(task(anim::TaskType::Place, spareId, target, 0.0f, d(kDemoPlace)));
                dest_[spareId].push_back({mv.to, target, false});
            }
        }
        if (job.pause > 0.0f) ts.push_back(task(anim::TaskType::Wait, -1, vec3(0), 0.0f, job.pause));
        coachHand.enqueue(ts);
        rt.demoBefore.push_back(rt.demoPos);
        rt.demoPos.makeMove(mv);
        aiMoveTo_ = mv.to;
        rt.phase = 1;
        LOGI("coach: demonstrates %s on the table", job.uci.c_str());
        return;
    }

    case TableJob::Kind::Rewind:
    case TableJob::Kind::TakeBack: {
        if (rt.phase == 0) {
            // The plies to take back, last first: the demonstrations still on the table, then (a
            // takeback) the game's moves, each by the hand of the player who made it.
            for (size_t i = rt.demoBefore.size(); i-- > 0;) {
                if (job.kind == TableJob::Kind::Rewind && int(rt.demoBefore.size() - i) > job.plies) break;
                rt.plies.insert(rt.plies.begin(), {rt.demoBefore[i], coachSeat, true});
            }
            if (job.kind == TableJob::Kind::TakeBack) {
                int n = int(game_.moves().size());
                int k = std::min(job.plies, lesson() ? n : n);
                if (k <= 0) {
                    finish();
                    return;
                }
                std::vector<CoachRuntime::Ply> back;
                for (int j = n - 1; j >= n - k; --j) {
                    const Position& before = game_.positionAt(size_t(j));
                    back.push_back({before, seatOf(before.sideToMove()), false});
                }
                // Game: the record as if the moves had never been played; the sheets forget them
                // (they were never written: the write limit, §4.2).
                if (turn_ == Turn::HumanTouched) humanRelease();
                game_.undo(k);
                // The saved game's move times follow the game: one per move played.
                moveElapsedMs_.resize(game_.moves().size());
                moveClockMs_.resize(game_.moves().size());
                plyElapsedMs_ = 0.0;
                arbiter_.reset(game_);
                if (!lesson() && !scorekeeper_.dropMoves(n - k))
                    LOGW("coach: takeback of %d plies: a scoresheet has begun writing them already", k);
                turn_ = Turn::CoachTable;
                aiRequested_ = aiHasMove_ = false;
                LOGI("coach: takes back %d plies (to move %d)", k, n - k);
                // The plies vector is taken from its back: demonstrations first, then the game's.
                std::vector<CoachRuntime::Ply> order;
                for (auto it = back.rbegin(); it != back.rend(); ++it) order.push_back(*it);
                for (const CoachRuntime::Ply& p : rt.plies) order.push_back(p);
                rt.plies = order;
            }
            rt.phase = 1;
        }
        // One ply at a time: plan it once every piece is at rest, then its trips in batches, one
        // hand at a time (a hand only reaches its own half beside the board).
        if (!handsIdle()) return;
        if (rt.tripNext >= rt.trips.size()) {
            if (!rt.trips.empty() || rt.batchSeat == -2) {
                // The ply is taken back: check the table.
                const CoachRuntime::Ply& done = rt.plies.back();
                if (!coach::tableMatches(board_, done.target)) {
                    LOGW("coach: the table does not show the position after the rewind: set up behind a fade");
                    board_.syncTo(done.target);
                    coachFade_ = std::max(coachFade_, 0.6f);   // a dip, back in updateCoach
                }
                if (done.demo && !rt.demoBefore.empty()) {
                    rt.demoPos = rt.demoBefore.back();
                    rt.demoBefore.pop_back();
                }
                rt.plies.pop_back();
                rt.trips.clear();
                rt.tripNext = 0;
                rt.batchSeat = -1;
            }
            if (rt.plies.empty()) {
                if (job.kind == TableJob::Kind::TakeBack) {
                    rt.demoPos = game_.position();
                    clock_.start(game_.position().sideToMove());   // unseen: it follows the side to move
                    beginTurn();
                }
                finish();
                return;
            }
            rt.trips = coach::planRewind(board_, rt.plies.back().target);
            rt.tripNext = 0;
            if (rt.trips.empty()) {
                rt.batchSeat = -2;   // nothing to carry: the ply is done at the next step
                return;
            }
        }
        // The next batch: consecutive trips of one hand.
        int ownerSeat = rt.plies.back().seat;
        int seat = handForTrip(rt.trips[rt.tripNext], ownerSeat);
        std::vector<anim::Task> ts;
        auto d = [&](float slowDuration) { return slow ? slowDuration : 0.0f; };
        while (rt.tripNext < rt.trips.size() && handForTrip(rt.trips[rt.tripNext], ownerSeat) == seat) {
            coach::PieceTrip t = rt.trips[rt.tripNext++];
            if (t.created) t.pieceId = board_.addSpare(t.type, t.color, t.from.pos);
            PieceObject* p = board_.byId(t.pieceId);
            if (!p) continue;
            float h = std::max(0.03f, carryHeight(p->basePos, t.to.pos, t.pieceId, -1));
            ts.push_back(task(anim::TaskType::Reach, t.pieceId, vec3(0), 0.0f, d(kDemoReach)));
            ts.push_back(task(anim::TaskType::Lift, t.pieceId, vec3(0), h, d(kDemoLift)));
            ts.push_back(task(anim::TaskType::Carry, t.pieceId, t.to.pos, 0.0f, d(kDemoCarry)));
            ts.push_back(task(anim::TaskType::Place, t.pieceId, t.to.pos, 0.0f, d(kDemoPlace)));
            Destination dst;
            dst.pos = t.to.pos;
            dst.square = t.to.kind == coach::RestKind::Square ? t.to.square : NoSquare;
            dst.captured = t.to.kind == coach::RestKind::Captured;
            dst.reserve = t.to.kind == coach::RestKind::Reserve;
            dest_[t.pieceId].push_back(dst);
        }
        if (seat != coachSeat) ts.push_back(task(anim::TaskType::Retract));   // the player's robot rests again
        if (!ts.empty()) anim_[seat].enqueue(ts);
        rt.batchSeat = seat;
        return;
    }

    case TableJob::Kind::SetPosition: {
        if (rt.phase == 0) {
            if (turn_ == Turn::HumanTouched) humanRelease();
            turn_ = Turn::CoachTable;
            coachFade_ = std::min(1.0f, coachFade_ + dt / kSetupFadeOut);
            if (coachFade_ < 1.0f) return;
            Game g;
            g.setEndDetection(game_.endDetection());   // off in the lesson: two kings alone are no draw
            if (!g.resetFromFEN(job.fen)) {
                LOGW("coach: lesson position '%s' is not valid", job.fen.c_str());
            } else {
                game_ = g;
                moveElapsedMs_.clear();   // a new game record: no move times yet
                moveClockMs_.clear();
                plyElapsedMs_ = 0.0;
                arbiter_.reset(game_);
                board_.syncTo(game_.position());
                cameraCut_ = true;
            }
            rt.demoBefore.clear();
            rt.demoPos = game_.position();
            rt.phase = 1;
            return;
        }
        coachFade_ = std::max(0.0f, coachFade_ - dt / kSetupFadeIn);
        if (coachFade_ > 0.0f) return;
        rt.jobRunning = false;
        clock_.start(game_.position().sideToMove());
        beginTurn();
        return;
    }

    case TableJob::Kind::LessonMove: {
        if (rt.phase == 0) {
            Move mv = game_.position().parseUCI(job.uci);
            if (!mv.valid()) {
                LOGW("coach: lesson move '%s' is not legal (%s)", job.uci.c_str(), game_.position().fen().c_str());
                finish();
                return;
            }
            Color side = game_.position().sideToMove();
            int moverId = board_.idAt(mv.from);
            int victimId = board_.idAt(mv.to);
            if (mv.flags & MoveEnPassant) victimId = board_.idAt(Square(mv.to + (side == White ? -8 : 8)));
            Square rookFrom = NoSquare, rookTo = NoSquare;
            if (mv.flags & (MoveCastleKing | MoveCastleQueen)) {
                int rank = rankOf(mv.from);
                bool king = (mv.flags & MoveCastleKing) != 0;
                rookFrom = makeSquare(king ? 7 : 0, rank);
                rookTo = makeSquare(king ? 5 : 3, rank);
            }
            std::vector<anim::Task> ts;
            ts.push_back(task(anim::TaskType::Reach, moverId));
            planPlacement(ts, moverId, mv.to, victimId, rookFrom, rookTo);
            if (mv.promotion != NoPiece) planPromotionSwap(ts, moverId, mv.to, mv.promotion);
            ts.push_back(task(anim::TaskType::Retract));
            coachHand.enqueue(ts);
            rt.lessonMove = mv;
            aiMoveTo_ = mv.to;
            turn_ = Turn::CoachTable;
            rt.phase = 1;
            return;
        }
        if (!dest_.empty()) return;
        // The move is on the board: the lesson game records it (no scoresheet, no clock).
        game_.play(rt.lessonMove);
        arbiter_.reset(game_);
        clock_.start(game_.position().sideToMove());
        rt.demoPos = game_.position();
        rt.jobRunning = false;
        LOGI("coach: lesson move %s", job.uci.c_str());
        if (rt.sessionRunning) rt.session.onMove(game_);
        beginTurn();
        return;
    }
    }
}

// ==============================================================================================
// --coach-stage-test: the Stage without the session
// ==============================================================================================

void GameScene::runStageTest(float dt) {
    CoachRuntime& rt = *coach_;
    CoachStage& st = *rt.stage;
    rt.testTime += dt;
    float t = rt.testTime;
    auto next = [&](const char* what) {
        ++rt.testStep;
        rt.testClock = 0.0f;
        LOGI("coach stage test: step %d at %.2f s (game time %.2f): %s", rt.testStep, t, time_, what);
    };
    rt.testClock += dt;
    const std::string ui = i18n::language();
    const std::string speech = coach::speechLanguage(ui);
    bool lessonTest = rt.testKind == "lesson";

    switch (rt.testStep) {
    case 0: {
        if (t < 0.5f) return;
        if (lessonTest) {
            // The lesson's first set-up, as its first chapter begins.
            coach::Lesson lesson;
            const coach::Script& beats = lesson.chapters().front().beats;
            for (const coach::Beat& b : beats)
                if (b.kind == coach::BeatKind::SetPosition) {
                    st.setPosition(b.fen);
                    LOGI("coach stage test: lesson position %s", b.fen.c_str());
                    break;
                }
        }
        // A line with two squares: the coach points at the first and traces the knight's jump on
        // the second, marks light on the words.
        coach::Line line;
        line.key = "lesson.first.reply";
        line.with("sq", coach::Arg::ofSquare(parseSquare("g1"))).with("sq2", coach::Arg::ofSquare(parseSquare("f3")));
        const coach::Catalog& cat = coach::Catalog::shared();
        rt.testSpoken = cat.render(line, ui, true, 7);
        rt.testWritten = cat.render(line, ui, false, 7);
        st.prewarmGlyphs(rt.testWritten.text);
        rt.testSpeech = st.voiceAvailable() ? st.requestSpeech(rt.testSpoken.text, speech, 1.0f, 1) : 0;
        LOGI("coach stage test: \"%s\" (spoken [%s] \"%s\", request %u)", rt.testWritten.text.c_str(), speech.c_str(),
             rt.testSpoken.text.c_str(), rt.testSpeech);
        next("line requested");
        return;
    }
    case 1: {
        if (st.tableBusy()) return;
        float duration;
        if (rt.testSpeech) {
            if (!st.takeSpeech(rt.testSpeech, rt.testPcm)) {
                if (!st.speechFailed(rt.testSpeech)) return;
                LOGW("coach stage test: the synthesis failed, subtitle only");
                rt.testSpeech = 0;
                return;
            }
            coach::SpeechTiming timing = coach::estimateTiming(rt.testPcm, st.speechSampleRate(), rt.testSpoken);
            rt.testSq = coach::anchorTime(timing, rt.testSpoken, "sq", 0.3f);
            rt.testSq2 = coach::anchorTime(timing, rt.testSpoken, "sq2", 0.7f);
            duration = timing.duration;
            LOGI("coach stage test: %.2f s of speech, %d pauses, anchors sq %.2f s, sq2 %.2f s", timing.duration,
                 int(timing.pauses.size()), rt.testSq, rt.testSq2);
            std::vector<float> pcm = rt.testPcm;
            if (!st.startVoice(std::move(pcm))) return;   // retried next frame
        } else {
            duration = st.readingTime(rt.testWritten.text);
            rt.testSq = 0.35f * duration;
            rt.testSq2 = 0.7f * duration;
        }
        st.showSubtitle(rt.testWritten.text, ui::subtitleDuration(rt.testWritten.text, duration));
        st.look(coach::Look::Target, parseSquare("g1"));
        coach::Gesture point;
        point.kind = coach::GestureKind::PointPiece;
        point.square = parseSquare("g1");
        point.emphasis = true;
        st.gesture(point, rt.testSq);
        coach::Gesture trace;
        trace.kind = coach::GestureKind::Trace;
        trace.path = {parseSquare("g1"), parseSquare("f3")};
        st.gesture(trace, rt.testSq2);
        TestMark piece, square, arrow;
        piece.mark.kind = coach::Mark::Kind::Piece;
        piece.mark.square = parseSquare("g1");
        piece.on = time_ + rt.testSq;
        square.mark.kind = coach::Mark::Kind::Square;
        square.mark.square = parseSquare("f3");
        square.on = time_ + rt.testSq2;
        arrow.mark.kind = coach::Mark::Kind::Arrow;
        arrow.mark.from = parseSquare("g1");
        arrow.mark.via = parseSquare("g3");
        arrow.mark.to = parseSquare("f3");
        arrow.on = time_ + rt.testSq2;
        rt.testMarks = {piece, square, arrow};
        next("speaking, pointing at g1, tracing g1-f3 on their words");
        return;
    }
    case 2: {
        bool finished = false;
        double clock = st.voiceClock(&finished);
        if (rt.testClock > 0.0f && int(rt.testClock * 2.0f) != int((rt.testClock - dt) * 2.0f))
            LOGI("coach stage test: voice clock %.2f s%s", clock, finished ? " (finished)" : "");
        if (!finished && rt.voiceActive) return;
        if (rt.subAge < 0.5f * rt.subHold && !rt.testSpeech) return;
        st.endGestures();
        for (TestMark& m : rt.testMarks) m.off = std::min(m.off, time_ + 0.4f);
        st.look(coach::Look::Board, NoSquare);
        next("line heard: gestures end, marks fade");
        return;
    }
    case 3:
        if (rt.testClock < 1.0f || st.bodyBusy()) return;
        if (lessonTest) {
            // The lesson's first position stays for the screenshot.
            next("lesson position shown");
            rt.testStep = 99;
            return;
        }
        st.showSkipHint(true);
        st.demoMove("e2e4", 0.4f);
        st.demoMove("e7e5", 0.4f);
        next("two demonstration moves");
        return;
    case 4:
        if (st.tableBusy()) return;
        LOGI("coach stage test: table after the demonstration: %s", rt.demoPos.fen().c_str());
        st.rewindDemo(2, false);
        next("rewind");
        return;
    case 5:
        if (st.tableBusy()) return;
        LOGI("coach stage test: rewound, the table %s the game's position", coach::tableMatches(board_, game_.position()) ? "shows" : "does NOT show");
        st.showSkipHint(false);
        st.showTakebackOffer(true);
        next("the takeback card");
        return;
    case 6:
        if (!rt.offerShown) {
            next("card answered");
            rt.testStep = 99;
        }
        return;
    default: return;
    }
}

}  // namespace game
