// Analysis mode in the game scene (GameMode::Analysis, see game_scene.h): a game reviewed by
// Stockfish while the player steps through it at the table.
//
//   - The game comes from the Analysis page (a saved game, a PGN pasted, a game of the online
//     history: MenuAction::StartAnalysis), from the game over card of a game just played ("Analyse
//     the game": analyseGameJustPlayed, which declines the rematch), or from --analyse <file>
//     [--game N]. It is a replay (replaying()): the record's players in the robot seats, its start
//     position, its clocks, the observer camera; but nothing moves by itself, the player says where
//     to go.
//   - Moving about (analysisGoTo): one move forward is played by the robot of the side to move;
//     one move back is taken back by hand (the piece back to its square, a captured piece back
//     from beside the board, a promotion undone), as the coach takes moves back; two moves or more
//     away, the board is set at once behind a short dip of the lights (setReplayPosition): the one
//     place in the game where the board changes without the robots' hands. Steps asked for while
//     one runs are queued; a far jump cuts it short. Play (K) steps forward by itself, waiting for
//     each comment to be decided and said.
//   - The review (analysis::GameReview) asks the scene's Stockfish for every position: first those
//     the comment waited for needs final (and, playing, the next position's), then the ones around
//     the board (a quick pass, then a deep one); its evaluations are kept in
//     <application data>/analysis/ (analysis/cache.h), so a game opened again shows them at once.
//   - The commentator (analysis::Commentator) speaks on the key moments when a forward step
//     reaches them: the comment waits for its evaluations as long as its position stays on the
//     board (analysis::CommentWait), never said from provisional ones, never twice. It speaks
//     through a coach::Director performing on an AnalysisStage: the coach's TTS worker (one model
//     loaded), a centred narrator's voice, the subtitles; its marks light in the coach's colours.
//     N switches the comments off, M the voice, B the arrows of the better moves.
//   - The board shows the move that led to it: its symbol as a badge with a tint of its square, and
//     the better move's arrow when the move was an inaccuracy, a mistake or a blunder.
//   - The scoresheets carry the whole game from the start (the players wrote it); the clocks show
//     the record's values at the position; nothing is rated or saved.
#include "game_scene.h"
#include "coach_model.h"
#include "game_saving.h"
#include "game_scene_detail.h"
#include "../analysis/cache.h"
#include "../analysis/commentary.h"
#include "../analysis/review.h"
#include "../audio/audio.h"
#include "../coach/catalog.h"
#include "../coach/director.h"
#include "../coach/openings.h"
#include "../coach/rewind.h"
#include "../coach/tactics.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "../i18n/unicode.h"
#include "../platform/platform.h"
#include "../ui/ui_font.h"
#include "layout.h"
#include "settings.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>

using namespace m;
using namespace chess;

namespace game {

using namespace scene_detail;

namespace {

// Play: the stillness between two moves, without a comment and after one.
constexpr float kPlayPause = 1.2f, kPlayPauseAfterComment = 0.8f;
// A board set at once: the dip of the lights (setReplayPosition's is 0.4).
constexpr float kJumpDip = 0.3f;
// The review's searches in screenshot runs (--shot with --warp): short, and waited for, so that
// the frames are reproducible.
constexpr int kShotQuickDepth = 8, kShotDeepDepth = 12, kShotMoveTimeMs = 1500;

float smooth01(float x) { return x <= 0.0f ? 0.0f : x >= 1.0f ? 1.0f : x * x * (3.0f - 2.0f * x); }

vec3 toVec3(const analysis::Rgb& c) { return vec3(c.r, c.g, c.b); }

// "1/2-1/2" as the cards write it.
std::string resultLabel(const std::string& r) { return r == "1/2-1/2" ? std::string("\xC2\xBD-\xC2\xBD") : r; }

}  // namespace

// ==============================================================================================
// Runtime state
// ==============================================================================================

struct AnalysisRuntime {
    explicit AnalysisRuntime(GameScene& s);

    analysis::GameReview review;
    analysis::Commentator commentator;
    std::vector<Move> moves;                  // the record's moves, legal from its start position
    replay::Timeline timeline;                // its clocks
    std::string opening;                      // the opening's name for the overlay ("" = none)
    std::string cacheFolder;
    bool loaded = false;
    bool cacheDirty = false;                  // evaluations found since the cache was read or written

    // The engine's search being run for the review.
    uint32_t request = 0;
    int requestPos = -1;
    uint64_t version = 1;                     // grows with every result: the overlay's rows are rebuilt

    // Moving about.
    enum class Step { None, Forward, Back };
    Step step = Step::None;
    int target = 0;                           // the position wanted
    bool playing = false;                     // forward by itself
    float still = 0.0f;                       // seconds the board has been still (Play's pace)
    // The step back by hand: the position it leads to, the hand of the player who made the move,
    // its trips in batches of one hand.
    Position backTarget;
    int backSeat = 0;
    bool backPlanned = false;
    std::vector<coach::PieceTrip> trips;
    size_t tripNext = 0;

    // The marks of the position on the board.
    int marksPos = -1;
    float marksAge = 0.0f;
    std::map<int, int> markPieces;            // square of a commentator's Piece mark -> its piece

    // Comments.
    std::unique_ptr<AnalysisStage> stage;
    coach::Director director;
    analysis::CommentWait comments;           // a forward step reached it: its comment, once decided
    bool commented = false;                   // the last step's comment was played (Play's pause)
    bool welcomed = false;                    // the start's comment ("Let's look back at this game")

    // The narrator's voice (the TTS worker is the coach's: GameScene::coachVoice()).
    audio::VoiceId voice;
    bool voiceActive = false, voiceStarted = false;
    double voiceQueued = 0.0, voiceGameT = 0.0;
    bool voicePausedByDirector = false, voicePausedByScene = false;
    // The subtitle.
    std::string subText;
    float subAge = 0.0f, subHold = 0.0f;

    // The overlay's rows, rebuilt when the review changes.
    uint64_t hudVersion = 0;
    std::vector<ui::AnalysisMove> hudMoves;
    bool summaryKnown = false;
    analysis::SideSummary summary[2];
    std::set<std::string> prewarmedLanguages;
};

// ==============================================================================================
// The Stage: the commentator's voice and subtitles (no body, nothing on the table)
// ==============================================================================================

class AnalysisStage final : public coach::Stage {
public:
    explicit AnalysisStage(GameScene& s) : s_(s) {}
    AnalysisRuntime& rt() const { return *s_.analysis_; }
    coach::Stage& tts() const { return s_.coachVoice(); }

    // ---- Voice: the coach's worker, a centred narrator's voice
    bool voiceAvailable() const override { return settings().analysisVoice && tts().voiceAvailable(); }
    uint32_t requestSpeech(const std::string& text, const std::string& lang, float speed, int priority) override {
        return tts().requestSpeech(text, lang, speed, priority);
    }
    bool takeSpeech(uint32_t id, std::vector<float>& pcm) override { return tts().takeSpeech(id, pcm); }
    bool speechFailed(uint32_t id) const override { return tts().speechFailed(id); }
    void cancelSpeech(uint32_t id) override { tts().cancelSpeech(id); }
    int speechSampleRate() const override { return tts().speechSampleRate(); }
    bool startVoice(std::vector<float>&& pcm) override {
        AnalysisRuntime& r = rt();
        if (pcm.empty()) return true;
        stopVoice();
        audio::VoiceParams p;
        p.sampleRate = speechSampleRate();
        p.spatial = false;      // a narrator: nobody at the table says it
        p.roomSend = 0.06f;     // close, as in a commentator's booth
        audio::VoiceId id = audio::openVoice(p);
        if (!id) return false;  // pcm untouched: the director retries
        double seconds = double(pcm.size()) / double(speechSampleRate());
        if (audio::appendVoice(id, std::move(pcm)) < 0.0) {
            audio::stopVoice(id, 0.0f);
            return false;
        }
        audio::closeVoice(id);
        r.voice = id;
        r.voiceActive = r.voiceStarted = true;
        r.voiceQueued = seconds;
        r.voiceGameT = 0.0;
        r.voicePausedByDirector = false;
        if (r.voicePausedByScene) audio::setVoicePaused(id, true);
        return true;
    }
    void stopVoice() override {
        AnalysisRuntime& r = rt();
        if (r.voiceActive) audio::stopVoice(r.voice, 0.08f);
        r.voiceActive = false;
    }
    void pauseVoice(bool paused) override {
        rt().voicePausedByDirector = paused;
        applyPause();
    }
    void applyPause() {
        AnalysisRuntime& r = rt();
        if (r.voiceActive) audio::setVoicePaused(r.voice, r.voicePausedByDirector || r.voicePausedByScene);
    }
    double voiceClock(bool* finished) const override {
        // As the coach's (CoachStage::voiceClock): the device's clock when it plays the voice, else
        // the game time (screenshot runs, no device).
        AnalysisRuntime& r = rt();
        if (finished) *finished = false;
        if (!r.voiceActive) {
            if (finished) *finished = true;
            return r.voiceStarted ? r.voiceQueued : -1.0;
        }
        audio::VoiceStatus st = audio::voiceStatus(r.voice);
        audio::Stats as = audio::stats();
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
    void showSubtitle(const std::string& written, float holdSeconds, bool) override {
        AnalysisRuntime& r = rt();
        r.subText = written;
        r.subAge = 0.0f;
        r.subHold = holdSeconds;
    }
    float readingTime(const std::string& written) const override { return ui::subtitleDuration(written, 0.0f); }

    // ---- No body: the commentator is a voice
    void look(coach::Look, Square) override {}
    void gesture(const coach::Gesture&, float) override {}
    void endGestures() override {}
    void speechLevel(float) override {}

    // ---- Nothing on the table (the commentator's scripts hold Say beats only)
    void demoMove(const std::string& uci, float) override { LOGW("analysis: no demonstration (%s)", uci.c_str()); }
    void rewindDemo(int, bool) override {}
    void takeBack(int) override {}
    void setPosition(const std::string&) override {}
    void playLessonMove(const std::string&) override {}
    bool tableBusy() const override { return false; }
    bool bodyBusy() const override { return false; }

    // ---- HUD
    void showTakebackOffer(bool) override {}
    void showSkipHint(bool) override {}
    void prewarmGlyphs(const std::string& written) override {
        std::vector<std::pair<int, uint32_t>> glyphs;
        for (char32_t cp : uni::decode(written)) glyphs.push_back({ui::font::FACE_TEXT, uint32_t(cp)});
        ui::font::prewarm(glyphs);
    }

private:
    GameScene& s_;
};

AnalysisRuntime::AnalysisRuntime(GameScene& s) : stage(new AnalysisStage(s)) {}

void GameScene::AnalysisRuntimeDelete::operator()(AnalysisRuntime* r) const { delete r; }

AnalysisRuntime& GameScene::analysisRuntime() {
    if (!analysis_) analysis_.reset(new AnalysisRuntime(*this));
    return *analysis_;
}

// ==============================================================================================
// Set-up
// ==============================================================================================

bool GameScene::loadAnalysis(const ui::ReplaySetup& choice) {
    // The player's side at the bottom of the bar and in front of the camera: Black's when the
    // record names them as Black (on this PC, or signed in to the online server).
    auto sideOfPlayer = [this]() {
        const std::string black = replayRecord_.tag("Black"), white = replayRecord_.tag("White");
        const std::string me = localPlayerName(), online = onlineSession().savedUsername();
        bool mine = (black == me || (!online.empty() && black == online)) && white != me && white != online;
        analysisWhiteBottom_ = !mine;
    };
    if (!choice.path.empty()) {
        if (!loadReplay(choice.path, choice.game)) return false;
        sideOfPlayer();
        return true;
    }
    // A PGN text (pasted, or a game of the online history): its first game read without error.
    chess::pgn::Result<chess::pgn::ParsedGame> r = chess::pgn::read(choice.pgn);
    std::string error = r.error;
    for (chess::pgn::ParsedGame& g : r.games) {
        if (g.ok()) {
            replayRecord_ = std::move(g.record);
            sideOfPlayer();
            return true;
        }
        if (error.empty()) error = g.error.text();
    }
    if (error.empty()) error = "no game";
    LOGW("analysis: the PGN text cannot be read: %s", error.c_str());
    ui::notify(i18n::trf("library.error.game", {error}), 5.0f);
    return false;
}

bool GameScene::playedRecord(chess::pgn::Record& out, bool& finished, archive::Mode& mode) const {
    mode = saving::archiveMode(mode_, directMatch_);
    // A direct match: the authority's moves, times and ending (the local game may lag behind).
    saving::DirectRecord direct;
    const Game* game = &game_;
    archive::GameInfo info;
    if (mode == archive::Mode::Direct) {
        if (!saving::directMatchRecord(og_, direct)) return false;
        game = &direct.game;
        info = direct.info;
        finished = direct.finished;
    } else if (mode == archive::Mode::Server) {
        // The server's result and ending (the board may not show them: a resignation, a flag).
        info.result = saving::onlineResult(og_.status);
        info.endKey = saving::onlineEndKey(og_.reason);
    }
    info.mode = mode;
    info.white = seats_[0].name;
    info.black = seats_[1].name;
    // Ratings: Stockfish's and the coach's level rating; the player's in a game that counts for
    // it (against Stockfish, a rated hot-seat game); none in a direct match or for the player of
    // a coach game.
    for (int i = 0; i < 2; ++i) {
        const Seat& st = seats_[i];
        bool shown = st.controller == Controller::Stockfish || (mode == archive::Mode::Play && engineOk_) ||
                     (mode == archive::Mode::HotSeat && hsPlayers_.rated);
        (i == 0 ? info.whiteElo : info.blackElo) = shown && mode != archive::Mode::Direct ? st.elo : 0;
    }
    info.started = gameStartedAt_;
    if (mode != archive::Mode::Direct) {
        info.timeControl = clock_.timeControl().pgnTag();
        info.elapsedMs = moveElapsedMs_;
        info.clockMs = moveClockMs_;
    }
    if (mode == archive::Mode::Coach) info.coachLevel = coachLevel_;
    out = archive::makeRecord(*game, info);
    return true;
}

void GameScene::analyseGameJustPlayed() {
    chess::pgn::Record rec;
    bool finished = game_.isOver();
    archive::Mode mode;
    if (!playedRecord(rec, finished, mode) || rec.plies.empty()) {
        LOGW("analysis: the game just played has no moves to analyse");
        return;
    }
    // The player's side of the board (a hot-seat game: White's).
    analysisWhiteBottom_ = hotSeat() || humanColor_ == White;
    // Equivalent to declining the rematch: an offer received is declined, the online game left
    // (archiveGame ran at its end already), the coach silent.
    if (online()) {
        if (rematchOffered_ && link_) link_->rematch(false);
        archiveGame(true);
        onlineSession().leaveGame();
        link_ = nullptr;
    }
    if (coach()) leaveCoachGame();
    clock_.stop();
    replayRecord_ = std::move(rec);
    LOGI("analysis: the game just played (%s, %d plies, %s)", archive::modeName(mode), int(replayRecord_.plies.size()),
         replayRecord_.result.c_str());
    mode_ = GameMode::Analysis;
    analysisAtEnd_ = true;
    state_ = State::FadeToGame;
    stateTime_ = 0.0f;
}

namespace {

// The opening's name for the overlay: the last named position of the book the game went through,
// in the interface language (else its English name, else the record's tag).
std::string openingName(const analysis::GameReview& review, const pgn::Record& record) {
    std::string name;
    const coach::OpeningBook& book = coach::OpeningBook::instance();
    int named = -1;
    for (int i = 1; i < review.positions(); ++i) {
        coach::OpeningBook::Hit hit;
        if (!book.lookup(review.positionAt(i).hash(), &hit)) break;
        if (hit.name >= 0) named = hit.name;
    }
    if (named >= 0) {
        int v = book.variationOfName(named), f = book.familyOfName(named);
        std::string ref = v >= 0 ? "variation:" + book.variations()[size_t(v)].id
                          : f >= 0 ? "family:" + book.families()[size_t(f)].id
                                   : std::string();
        if (!ref.empty()) name = coach::OpeningTexts::instance().arg(ref, "nom", i18n::language(), false);
        if (name.empty()) name = book.name(named);
    }
    if (name.empty()) {
        const std::string tag = record.tag("Opening");
        if (!tag.empty() && tag != "?") name = tag;
    }
    return name;
}

}  // namespace

void GameScene::setupAnalysis() {
    AnalysisRuntime& a = analysisRuntime();
    const Settings& s = settings();
    a.loaded = true;
    // The record's moves from its start position (set up already by setupNewGame).
    a.moves.clear();
    Position pos = game_.position();
    for (const chess::pgn::Ply& p : replayRecord_.plies) {
        Move mv = pos.findLegal(p.move.from, p.move.to, p.move.promotion);
        if (!mv.valid()) {
            LOGW("analysis: move %d (%s) cannot be played: the game stops before it", int(a.moves.size()) + 1, p.san.c_str());
            break;
        }
        pos.makeMove(mv);
        a.moves.push_back(mv);
    }
    replayRecord_.plies.resize(a.moves.size());
    a.timeline.build(replayRecord_);

    analysis::Settings rs;
    if (ctx_->screenshotMode) {
        rs.quickDepth = kShotQuickDepth;
        rs.deepDepth = kShotDeepDepth;
        rs.deepMoveTimeMs = rs.quickMoveTimeMs = kShotMoveTimeMs;
    }
    a.review.reset(game_.position(), a.moves, rs);
    a.cacheFolder = plat::appDataDirectory() + "analysis/";
    a.cacheDirty = false;
    std::vector<analysis::PositionEval> saved;
    int savedDeep = 0;
    if (!ctx_->screenshotMode && analysis::loadCache(a.cacheFolder, a.review.key(), a.review.positions(), saved, &savedDeep)) {
        a.review.restore(saved, savedDeep);
        LOGI("analysis: evaluations of an earlier review restored (%.0f%% final)", double(a.review.progress()) * 100.0);
    }
    analysis::GameInfo gi;
    gi.result = replayRecord_.result;
    gi.endReasonKey = replay::endReasonKey(replayRecord_);
    a.commentator.reset(gi);
    a.request = 0;
    a.requestPos = -1;
    ++a.version;

    a.opening = openingName(a.review, replayRecord_);

    // The scoresheets: the whole game, as the players wrote it.
    std::vector<std::string> sheet(size_t(replaySheetOffset()), std::string("..."));
    const std::vector<std::string>& san = a.review.game().sanMoves();
    sheet.insert(sheet.end(), san.begin(), san.end());
    if (!sheet.empty()) scorekeeper_.writeMovesInstantly(sheet);
    // The lever is down on the side of the player who would have moved last.
    Color last = opposite(game_.position().sideToMove());
    leverSide_ = leverTarget_ = world_.clockHalfForSeat(seatOf(last) == 0 ? 1.0f : -1.0f) == 1 ? 1.0f : -1.0f;

    // The commentator.
    coach::DirectorConfig dc;
    dc.uiLanguage = i18n::language();
    dc.subtitles = s.subtitles;
    a.director.reset(a.stage.get(), dc);
    analysisOptionsChanged();
    if (s.analysisVoice) ensureCoachVoiceWorker();
    // The voice was never offered (the Coach page, the option): offered here.
    offerVoiceForAnalysis();

    // Where it opens: the start, the last position (a game just played), --analysis-at N.
    a.step = AnalysisRuntime::Step::None;
    a.playing = false;
    a.still = 0.0f;
    a.comments.clear();
    a.subText.clear();
    int at = analysisAtArg_ >= 0 && round_ == 1 ? analysisAtArg_ : analysisAtEnd_ ? int(a.moves.size()) : 0;
    analysisAtEnd_ = false;
    a.target = std::clamp(at, 0, int(a.moves.size()));
    if (a.target > 0) setReplayPosition(a.target);
    a.marksPos = a.target;
    a.marksAge = 1.0f;
    // Opened at the start: the commentator's welcome (a game opened elsewhere hears it when Play
    // starts from the start).
    a.welcomed = false;
    if (a.target == 0 && settings().analysisComments) {
        a.welcomed = true;
        a.comments.ask(0);
    }

    // The view: from the player's side, above the board (unless --cam says otherwise).
    if (!pendingCamArg_) {
        pendingViewpoint_ = -1;
        const float f = analysisWhiteBottom_ ? 1.0f : -1.0f;
        observer_.setPose(CameraPose::looking(vec3(-0.06f * f, 1.47f, 0.50f * f), vec3(-0.045f * f, layout::BOARD_TOP_Y, -0.02f * f),
                                              kFov));
        cameraCut_ = true;
    }
    LOGI("Analysis: %s vs %s, %d plies, %s, from position %d%s", replayRecord_.tag("White", "?").c_str(),
         replayRecord_.tag("Black", "?").c_str(), int(a.moves.size()), replayRecord_.result.c_str(), a.target,
         engineOk_ ? "" : " (no engine: no review)");
}

void GameScene::leaveAnalysis() {
    if (!analysis_) return;
    AnalysisRuntime& a = *analysis_;
    if (a.request && engineOk_) engine_.cancelAnalysis(a.request);
    if (a.request) a.review.forget(a.requestPos);
    a.request = 0;
    a.requestPos = -1;
    if (a.loaded && a.cacheDirty && !ctx_->screenshotMode) {
        if (analysis::saveCache(a.cacheFolder, a.review.key(), a.review.evaluations(), a.review.settings().deepDepth)) a.cacheDirty = false;
        else LOGW("analysis: the evaluations could not be saved in %s", a.cacheFolder.c_str());
    }
    a.director.clear();
    a.stage->cancelSpeech(0);
    a.stage->stopVoice();
    a.voiceStarted = false;
    a.subText.clear();
    a.playing = false;
    a.step = AnalysisRuntime::Step::None;
    a.trips.clear();
    a.comments.clear();
    a.markPieces.clear();
    a.loaded = false;
}

// ==============================================================================================
// Frame update
// ==============================================================================================

namespace {

// The commentator's comment as a script: one line a beat, each mark on the first line that names
// its anchor (else the first line).
coach::Script scriptOf(const analysis::Comment& c) {
    coach::Script s;
    for (const coach::Line& l : c.lines) {
        coach::Beat b;
        b.kind = coach::BeatKind::Say;
        b.line = l;
        b.look = coach::Look::Board;
        b.priority = coach::Priority::Normal;
        s.push_back(b);
    }
    for (const coach::Mark& m : c.marks) {
        if (s.empty()) break;
        size_t at = 0;
        for (size_t i = 0; i < s.size(); ++i)
            if (!m.anchor.empty() && s[i].line.arg(m.anchor)) {
                at = i;
                break;
            }
        coach::Mark mk = m;
        if (!s[at].line.arg(mk.anchor)) mk.anchor.clear();
        s[at].marks.push_back(mk);
    }
    return s;
}

}  // namespace

void GameScene::updateAnalysis(float dt) {
    AnalysisRuntime& a = analysisRuntime();
    // The dip of a board set at once.
    if (fadeDip_ > 0.0f) {
        fade_ = std::max(fade_, fadeDip_);
        fadeDip_ = 0.0f;
    }
    if (state_ == State::Playing) fade_ = std::max(0.0f, fade_ - dt / 1.2f);
    const int plies = int(a.moves.size());
    auto handsIdle = [&] { return dest_.empty() && !anim_[0].busy() && !anim_[1].busy(); };

    // ---- The review: one search at a time, what the comment waited for needs first, then the
    // board's surroundings.
    if (engineOk_) {
        int focus = int(game_.moves().size());
        if (!a.request) {
            std::vector<int> urgent = a.comments.urgent(a.review, a.commentator);
            // Playing: the next position's comment too, decided by the time its move is played.
            if (a.playing && settings().analysisComments && focus < plies)
                for (const int i : a.commentator.needs(a.review, focus + 1))
                    if (std::find(urgent.begin(), urgent.end(), i) == urgent.end()) urgent.push_back(i);
            ai::AnalysisRequest req;
            int p = -1;
            if (a.review.nextRequest(focus, req, p, urgent)) {
                req.priority = -1;   // background work: never ahead of anything else
                a.request = engine_.requestAnalysis(req);
                a.requestPos = p;
                if (!a.request) {
                    a.review.fail(p);
                    a.requestPos = -1;
                }
            }
        }
        if (a.request) {
            // Screenshot runs wait for it: their frames do not follow the wall clock.
            if (ctx_->screenshotMode)
                for (int i = 0; i < 4000 && !engine_.analysisReady(a.request); ++i) plat::sleepMs(5);
            ai::Analysis res;
            if (engine_.takeAnalysis(a.request, res)) {
                if (res.ok) a.review.accept(a.requestPos, res);
                else a.review.fail(a.requestPos);
                a.request = 0;
                a.requestPos = -1;
                a.cacheDirty = true;
                ++a.version;
                // A review completed: saved now (the window may be closed without leaving).
                if (a.review.complete() && !ctx_->screenshotMode &&
                    analysis::saveCache(a.cacheFolder, a.review.key(), a.review.evaluations(), a.review.settings().deepDepth))
                    a.cacheDirty = false;
            }
        }
    }

    if (state_ != State::Playing) return;
    const int cur = int(game_.moves().size());

    // ---- A step forward: the robot's move is completed as its last piece is released (no clock).
    if (a.step == AnalysisRuntime::Step::Forward && turn_ == Turn::AiMoving && dest_.empty())
        completeMove(seatOf(game_.position().sideToMove()));

    // ---- A step back: its trips, one ply, in batches of one hand.
    if (a.step == AnalysisRuntime::Step::Back && handsIdle()) {
        if (!a.backPlanned) {
            a.trips = coach::planRewind(board_, a.backTarget);
            a.tripNext = 0;
            a.backPlanned = true;
        }
        if (a.tripNext >= a.trips.size()) {
            if (!coach::tableMatches(board_, a.backTarget)) {
                LOGW("analysis: the table does not show the position after the step back: set up behind a dip");
                board_.syncTo(a.backTarget);
                fadeDip_ = std::max(fadeDip_, kJumpDip);
            }
            a.trips.clear();
            a.step = AnalysisRuntime::Step::None;
            beginTurn();
            // Through the eyes of the player to move: back into the eyes of the side to move again.
            const int toMove = seatOf(game_.position().sideToMove());
            if (followEyes_ && eyesSeat_ != toMove) followEyesAfterMove(1 - toMove);
            analysisStepDone(false);
        } else {
            int seat = handForTrip(a.trips[a.tripNext], a.backSeat);
            std::vector<anim::Task> ts;
            while (a.tripNext < a.trips.size() && handForTrip(a.trips[a.tripNext], a.backSeat) == seat) {
                coach::PieceTrip t = a.trips[a.tripNext++];
                if (t.created) t.pieceId = board_.addSpare(t.type, t.color, t.from.pos);
                PieceObject* p = board_.byId(t.pieceId);
                if (!p) continue;
                float h = std::max(0.03f, carryHeight(p->basePos, t.to.pos, t.pieceId, -1));
                ts.push_back(task(anim::TaskType::Reach, t.pieceId));
                ts.push_back(task(anim::TaskType::Lift, t.pieceId, vec3(0), h));
                ts.push_back(task(anim::TaskType::Carry, t.pieceId, t.to.pos));
                ts.push_back(task(anim::TaskType::Place, t.pieceId, t.to.pos));
                Destination dst;
                dst.pos = t.to.pos;
                dst.square = t.to.kind == coach::RestKind::Square ? t.to.square : NoSquare;
                dst.captured = t.to.kind == coach::RestKind::Captured;
                dst.reserve = t.to.kind == coach::RestKind::Reserve;
                dest_[t.pieceId].push_back(dst);
            }
            if (!ts.empty()) {
                ts.push_back(task(anim::TaskType::Retract));
                anim_[seat].enqueue(ts);
            }
        }
    }

    // ---- The next step: towards the target, or Play's next move once the comment is said.
    if (a.step == AnalysisRuntime::Step::None && turn_ != Turn::AiMoving && handsIdle() && !paused_) {
        if (a.target != cur) {
            analysisGoTo(a.target);
        } else if (a.playing) {
            if (cur >= plies) {
                a.playing = false;
            } else if (!a.comments.waitingComment() && a.director.idle()) {
                a.still += dt;
                if (a.still >= (a.commented ? kPlayPauseAfterComment : kPlayPause)) analysisGoTo(cur + 1);
            }
        }
    }

    // ---- A comment waiting for the review of its moves: as long as its position is on the board.
    if (a.comments.waiting()) {
        analysis::Comment c;
        if (!settings().analysisComments) {
            a.comments.clear();
        } else if (a.comments.poll(a.review, a.commentator, int(game_.moves().size()), !a.director.idle(), c)) {
            LOGI("analysis: comment at position %d (%d line%s)", c.position, int(c.lines.size()), c.lines.size() == 1 ? "" : "s");
            a.director.play(scriptOf(c));
            a.commented = true;
        }
    }

    // ---- The voice, the subtitle, the marks.
    a.director.setPaused(paused_);
    a.voicePausedByScene = paused_;
    a.stage->applyPause();
    if (a.voiceActive && !paused_) a.voiceGameT += dt;
    a.director.update(dt, int(game_.moves().size()));
    a.subAge += dt;
    a.marksAge += dt;
    for (auto& an : anim_) an.setThinking(false);

    // --replay-keys: the next key once the board is still.
    if (replayKeysPos_ < replayKeys_.size()) {
        replayKeyWait_ -= dt;
        const bool still = a.step == AnalysisRuntime::Step::None && turn_ != Turn::AiMoving && handsIdle() &&
                           a.target == int(game_.moves().size());
        if (still && replayKeyWait_ <= 0.0f) {
            const std::string& k = replayKeys_[replayKeysPos_++];
            LOGI("--replay-keys: %s (at position %d)", k.c_str(), int(game_.moves().size()));
            if (k == "Leave") {
                paused_ = true;
                scriptMenu_ = ui::MenuAction::BackToMainMenu;
            } else if (k.compare(0, 4, "Wait") == 0) {
                // "Wait:3": three more seconds of stillness (a comment said, the review going on).
                replayKeyWait_ = float(std::atof(k.c_str() + std::min<size_t>(k.size(), 5)));
                return;
            } else if (!analysisKey(k)) {
                LOGW("--replay-keys: '%s' is not J, K, L, Home, End, N, M, B, Goto:N, Wait:S or Leave", k.c_str());
            }
            replayKeyWait_ = 0.6f;
        }
    }
}

void GameScene::analysisGoTo(int position) {
    AnalysisRuntime& a = analysisRuntime();
    const int plies = int(a.moves.size());
    position = std::clamp(position, 0, plies);
    a.target = position;
    const int cur = int(game_.moves().size());
    if (a.step != AnalysisRuntime::Step::None || turn_ == Turn::AiMoving) {
        // A step runs: the next one follows it; a jump two moves away or more from where it leads
        // cuts it short.
        int dest = a.step == AnalysisRuntime::Step::Forward ? cur + 1 : cur;
        if (std::abs(position - dest) < 2) return;
    } else if (position == cur) {
        return;
    }
    a.director.clear();
    a.comments.clear();
    a.still = 0.0f;
    a.commented = false;
    if (std::abs(position - cur) == 1 && a.step == AnalysisRuntime::Step::None && turn_ != Turn::AiMoving) {
        a.marksPos = -1;
        if (position > cur) {
            // One move forward: the robot of the side to move plays it.
            a.step = AnalysisRuntime::Step::Forward;
            playRobotMove(seatOf(game_.position().sideToMove()), a.moves[size_t(cur)]);
        } else {
            // One move back: taken back by the hand of the player who made it.
            a.step = AnalysisRuntime::Step::Back;
            a.backTarget = game_.positionAt(size_t(cur - 1));
            a.backSeat = seatOf(a.backTarget.sideToMove());
            a.backPlanned = false;
            a.trips.clear();
            a.tripNext = 0;
            game_.undo(1);
            arbiter_.reset(game_);
            turn_ = Turn::None;
        }
        return;
    }
    // Two moves away or more: the board at once.
    a.step = AnalysisRuntime::Step::None;
    a.trips.clear();
    setReplayPosition(position);
    fadeDip_ = std::min(fadeDip_, kJumpDip);
    a.marksPos = position;
    a.marksAge = 0.0f;
    a.markPieces.clear();
}

void GameScene::analysisOptionsChanged() {
    // The language and the subtitles of the options (set up, or changed in the pause menu): the
    // commentator's lines, their glyphs, the opening's name.
    if (!analysis_) return;
    AnalysisRuntime& a = *analysis_;
    coach::DirectorConfig dc = a.director.config();
    dc.uiLanguage = i18n::language();
    dc.subtitles = settings().subtitles;
    a.director.setConfig(dc);
    const std::string ui = i18n::language();
    if (!a.prewarmedLanguages.count(ui)) {
        a.prewarmedLanguages.insert(ui);
        std::vector<std::pair<int, uint32_t>> glyphs;
        for (uint32_t cp : coach::Catalog::shared().codepoints(ui)) glyphs.push_back({ui::font::FACE_TEXT, cp});
        ui::font::prewarm(glyphs);
    }
    if (a.loaded) a.opening = openingName(a.review, replayRecord_);
    if (settings().analysisVoice) refreshCoachVoice();
}

bool GameScene::analysisLoaded() const { return analysis_ && analysis_->loaded; }

void GameScene::holdAnalysis() {
    // updateAnalysis does not run behind the pause menu: the commentator's line waits there.
    if (!analysis_ || !analysis_->loaded) return;
    AnalysisRuntime& a = *analysis_;
    a.director.setPaused(true);
    a.voicePausedByScene = true;
    a.stage->applyPause();
}

void GameScene::analysisStepDone(bool forward) {
    AnalysisRuntime& a = analysisRuntime();
    const int cur = int(game_.moves().size());
    a.marksPos = cur;
    a.marksAge = 0.0f;
    a.markPieces.clear();
    a.still = 0.0f;
    a.commented = false;
    // A move played forward: its comment, once its review allows (never on a step back).
    if (forward && cur > 0 && settings().analysisComments && engineOk_) a.comments.ask(cur);
}

void GameScene::completeAnalysisMove(int seat, const Arbiter::Verdict& v) {
    AnalysisRuntime& a = analysisRuntime();
    if (!v.legal) {
        // Never expected (the record's moves are legal): the board is set after the move.
        LOGW("analysis: move %d refused by the arbiter: set up at once", int(game_.moves().size()) + 1);
        a.step = AnalysisRuntime::Step::None;
        setReplayPosition(int(game_.moves().size()) + 1);
        return;
    }
    game_.play(v.move);
    a.step = AnalysisRuntime::Step::None;
    beginTurn();
    followEyesAfterMove(seat);
    analysisStepDone(true);
}

// ==============================================================================================
// Controls
// ==============================================================================================

void GameScene::updateAnalysisInput() {
    // Free keys of the viewer (its camera takes the arrows, WASD / ZQSD, E, C, Space, Ctrl, Shift,
    // PageUp / PageDown, 0-9, H, Tab): J K L as in video players, Home and End, N M B.
    const plat::Input& in = plat::input();
    if (in.keyPressed['K']) analysisKey("K");
    if (in.keyPressed['J']) analysisKey("J");
    if (in.keyPressed['L']) analysisKey("L");
    if (in.keyPressed[plat::KEY_HOME]) analysisKey("Home");
    if (in.keyPressed[plat::KEY_END]) analysisKey("End");
    if (in.keyPressed['N']) analysisKey("N");
    if (in.keyPressed['M']) analysisKey("M");
    if (in.keyPressed['B']) analysisKey("B");
}

bool GameScene::analysisKey(const std::string& key) {
    AnalysisRuntime& a = analysisRuntime();
    Settings& s = settings();
    const int plies = int(a.moves.size());
    // The board's keys wait for the table (the intro, the fade): a move started before the game
    // plays would never be completed.
    const bool boardKey = key == "K" || key == "J" || key == "L" || key == "Home" || key == "End" ||
                          key.compare(0, 5, "Goto:") == 0;
    if (boardKey && state_ != State::Playing) return false;
    if (key == "K") {
        a.playing = !a.playing && a.target < plies;
        a.still = kPlayPause;   // the first move at once
        if (a.playing && a.target == 0 && int(game_.moves().size()) == 0 && !a.welcomed && s.analysisComments &&
            a.step == AnalysisRuntime::Step::None) {
            // From the start: the welcome first, the first move once it is said.
            a.welcomed = true;
            a.comments.ask(0);
        }
    } else if (key == "J" || key == "L") {
        a.playing = false;      // stepping pauses
        analysisGoTo(a.target + (key == "L" ? 1 : -1));
    } else if (key == "Home" || key == "End") {
        a.playing = false;
        analysisGoTo(key == "Home" ? 0 : plies);
    } else if (key.compare(0, 5, "Goto:") == 0) {
        a.playing = false;
        analysisGoTo(std::atoi(key.c_str() + 5));
    } else if (key == "N") {
        s.analysisComments = !s.analysisComments;
        if (!s.analysisComments) {
            a.director.clear();
            a.comments.clear();
        }
        s.save();
    } else if (key == "M") {
        s.analysisVoice = !s.analysisVoice;
        if (s.analysisVoice) ensureCoachVoiceWorker();
        else a.stage->stopVoice();   // the line goes on as a subtitle (the director's watchdog)
        s.save();
    } else if (key == "B") {
        s.analysisArrows = !s.analysisArrows;
        s.save();
    } else {
        return false;
    }
    return true;
}

// ==============================================================================================
// The board's marks
// ==============================================================================================

void GameScene::analysisMarks(std::vector<AnalysisMark>& marks, std::vector<PieceHighlight>& highlights,
                              std::vector<CoachMark>& coachMarks) {
    if (!analysis_ || !analysis_->loaded || state_ != State::Playing) return;
    AnalysisRuntime& a = *analysis_;
    // The move that led to the position on the board, once it stands there.
    const int p = a.marksPos;
    if (p > 0 && p == int(game_.moves().size()) && a.step == AnalysisRuntime::Step::None) {
        analysis::Verdict v = a.review.verdict(p - 1);
        const Move& mv = a.moves[size_t(p - 1)];
        const float in = smooth01(a.marksAge / 0.3f);
        if (v.known && v.nag != analysis::Nag::None) {
            const vec3 color = toVec3(analysis::nagColor(v.nag));
            AnalysisMark tint;
            tint.kind = AnalysisMark::Tint;
            tint.sq = mv.to;
            tint.color = color;
            tint.strength = in;
            tint.age = a.marksAge;
            marks.push_back(tint);
            AnalysisMark badge = tint;
            badge.kind = AnalysisMark::Badge;
            badge.glyph = int(v.nag);
            badge.strength = 1.0f;
            marks.push_back(badge);
        }
        const bool worse = v.nag == analysis::Nag::Dubious || v.nag == analysis::Nag::Mistake || v.nag == analysis::Nag::Blunder;
        if (settings().analysisArrows && v.known && worse && !v.betterUci.empty()) {
            Move b = a.review.positionAt(p - 1).parseUCI(v.betterUci);
            if (b.valid()) {
                AnalysisMark arrow;
                arrow.kind = AnalysisMark::Arrow;
                arrow.from = b.from;
                arrow.to = b.to;
                arrow.via = coach::knightCorner(b.from, b.to);
                arrow.color = toVec3(analysis::betterMoveColor());
                arrow.strength = smooth01((a.marksAge - 0.15f) / 0.3f);
                arrow.age = std::max(0.0f, a.marksAge - 0.15f);
                if (arrow.strength > 0.0f) marks.push_back(arrow);
            }
        }
    }
    // The commentator's marks, as the coach's.
    std::set<int> used;
    for (const coach::ShownMark& m : a.director.marks()) {
        if (m.strength <= 0.0f) continue;
        CoachMark c;
        c.strength = m.strength;
        c.age = m.age;
        switch (m.kind) {
        case coach::Mark::Kind::Square:
            c.kind = CoachMark::Square;
            c.sq = m.square;
            coachMarks.push_back(c);
            break;
        case coach::Mark::Kind::Arrow:
            c.kind = CoachMark::Arrow;
            c.from = m.from;
            c.via = m.via;
            c.to = m.to;
            coachMarks.push_back(c);
            break;
        case coach::Mark::Kind::Piece: {
            if (m.square == NoSquare) break;
            used.insert(int(m.square));
            auto it = a.markPieces.find(int(m.square));
            int id = it != a.markPieces.end() ? it->second : board_.idAt(m.square);
            if (it == a.markPieces.end() && id >= 0) a.markPieces[int(m.square)] = id;
            const PieceObject* pc = id >= 0 ? board_.byId(id) : nullptr;
            if (pc) highlights.push_back({id, m.strength});
            c.kind = CoachMark::Square;
            c.sq = pc && pc->square != NoSquare ? pc->square : m.square;
            coachMarks.push_back(c);
            break;
        }
        }
    }
    for (auto it = a.markPieces.begin(); it != a.markPieces.end();) it = used.count(it->first) ? std::next(it) : a.markPieces.erase(it);
}

const std::vector<std::string>& GameScene::analysedMoves() const {
    static const std::vector<std::string> none;
    return analysis_ ? analysis_->review.game().sanMoves() : none;
}

ClockDisplay GameScene::analysisClockDisplay() const {
    ClockDisplay d;
    replay::ClockView v;
    if (analysis_) v = replay::clocksAt(analysis_->timeline, int(game_.moves().size()));
    int hw = world_.clockHalfForSeat(1.0f), hb = 1 - hw;
    d.dashes = !v.known;
    d.ms[hw] = v.ms[0];
    d.ms[hb] = v.ms[1];
    d.running = -1;
    d.unlimited = false;
    d.paused = false;
    d.leverSide = leverSide_;
    return d;
}

// ==============================================================================================
// The overlay
// ==============================================================================================

void GameScene::drawAnalysisOverlay() {
    if (!analysis_ || !analysis_->loaded) return;
    AnalysisRuntime& a = *analysis_;
    const Settings& s = settings();
    const int plies = int(a.moves.size());
    const int cur = int(game_.moves().size());
    // The rows and the summary, when the review changed.
    if (a.hudVersion != a.version) {
        a.hudVersion = a.version;
        a.hudMoves.assign(size_t(plies), ui::AnalysisMove());
        const std::vector<std::string>& san = a.review.game().sanMoves();
        for (int i = 0; i < plies; ++i) {
            ui::AnalysisMove& m = a.hudMoves[size_t(i)];
            m.san = i < int(san.size()) ? san[size_t(i)] : std::string();
            analysis::Verdict v = a.review.verdict(i);
            m.nag = v.known ? int(v.nag) : 0;
            analysis::EvalBar b = a.review.bar(i + 1);
            m.known = b.known;
            m.white = b.white;
        }
        a.summaryKnown = a.review.complete() && engineOk_;
        if (a.summaryKnown) {
            a.summary[0] = a.review.summary(White);
            a.summary[1] = a.review.summary(Black);
        }
    }
    ui::AnalysisHud h;
    h.visible = hudVisible_;
    analysis::EvalBar bar = a.review.bar(cur);
    h.evalKnown = bar.known;
    h.evalWhite = bar.white;
    h.evalText = bar.text;
    h.whiteBottom = analysisWhiteBottom_;
    auto label = [](const std::string& name, int elo) { return elo > 0 ? name + " (" + std::to_string(elo) + ")" : name; };
    h.white = label(seats_[0].name, seats_[0].elo);
    h.black = label(seats_[1].name, seats_[1].elo);
    h.result = resultLabel(replayRecord_.result);
    h.opening = a.opening;
    h.moves = a.hudMoves;
    const Position& start = game_.startPosition();
    h.firstMoveNumber = replay::moveNumberAfter(start, 0);
    h.blackFirst = start.sideToMove() == Black;
    h.current = cur;
    h.summaryKnown = a.summaryKnown;
    for (int c = 0; c < 2; ++c) {
        h.accuracy[c] = a.summary[c].accuracy;
        for (int k = 0; k < 7; ++k) h.counts[c][k] = a.summary[c].count[k];
    }
    h.progress = engineOk_ ? a.review.progress() : 1.0f;
    h.engineMissing = !engineOk_;
    h.playing = a.playing;
    h.atStart = a.target == 0;
    h.atEnd = a.target >= plies;
    h.commentsOn = s.analysisComments;
    h.voiceOn = s.analysisVoice;
    h.voiceAvailable = coachVoiceExpected();
    h.arrowsOn = s.analysisArrows;
    ui::AnalysisHudResult r = ui::analysisHud(h);
    switch (r.action) {
    case ui::AnalysisAction::GoTo:
        a.playing = false;
        analysisGoTo(r.position);
        break;
    case ui::AnalysisAction::Start: analysisKey("Home"); break;
    case ui::AnalysisAction::Back: analysisKey("J"); break;
    case ui::AnalysisAction::TogglePlay: analysisKey("K"); break;
    case ui::AnalysisAction::Forward: analysisKey("L"); break;
    case ui::AnalysisAction::End: analysisKey("End"); break;
    case ui::AnalysisAction::ToggleComments: analysisKey("N"); break;
    case ui::AnalysisAction::ToggleVoice: analysisKey("M"); break;
    case ui::AnalysisAction::ToggleArrows: analysisKey("B"); break;
    default: break;
    }
    // The commentator's words, between the bar and the panel.
    ui::Subtitle sub;
    sub.text = a.subText;
    sub.age = a.subAge;
    sub.duration = a.subHold;
    sub.spanLeft = r.freeLeft;
    sub.spanRight = r.freeRight;
    sub.tag = i18n::tr("analysis.hud.speaker");
    ui::subtitles(sub);
}

}  // namespace game
