// Replay of a saved game in the 3D scene (GameMode::Replay): the two robots play the record's moves
// again at the pace they were played, watched from the viewer mode's free observer camera.
//
//   - The game comes from the "Saved games" page (MenuAction::StartReplay) or --replay <file>
//     (--game N), through archive::loadFile: moves, start position (a FEN game), per-ply clocks and
//     times, tags.
//   - replay::ReplayClock (replay.h) says when each move begins: the time it took in the game
//     ([%emt], else the difference of the [%clk] values, else a natural default pace), divided by
//     the speed (x1, x2, x4, x8; Instant waits not at all), the robot's own move included. The scene
//     plays the move with the robot of the side to move (playRobotMove, as Stockfish's moves) and
//     tells the clock when it is completed (the clock press; untimed: the last piece released).
//   - The chess clock shows the record's clocks (replayClockDisplay), dashes without them; the
//     scene's own chess::Clock never runs.
//   - The scoresheets carry the record's names, ratings, date, event and round, and the players
//     write each move as it is played. A step back or a jump sets the board, the game and fresh
//     sheets at once, behind a short dip of the lights (setReplayPosition).
//   - At the last move: the record's result card (the result, how it ended, the players), with
//     "Replay again"; a step back or a jump from there plays on. Nothing is rated or saved.
//   - Keys (updateReplayInput, besides the viewer's camera keys): K pause / resume, J / L one move
//     back / forward, Shift+J / Shift+L slower / faster, Home / End the start / the end; the same
//     as mouse buttons on the bar at the bottom (drawReplayBar).
#include "game_scene.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "../platform/platform.h"
#include "game_archive.h"
#include <algorithm>
#include <cstdlib>

using namespace m;
using namespace chess;

namespace game {

namespace {

// A robot's move, from the reach to the clock press: part of the time each move takes at x1.
// Measured on replays (the "replay: ply" log lines): 1.17 s for a quiet move, 1.67 s for a
// capture, 2.05 s for castling, 3.4 s for a promotion; 0.3 s less without a clock to press.
constexpr int64_t kRobotMoveMs = 1300;
constexpr int64_t kRobotMoveUntimedMs = 1000;

// A rating tag ("2350"), 0 when there is none or it is not a plausible rating.
int tagElo(const pgn::Record& r, const char* tag) {
    int v = std::atoi(r.tag(tag).c_str());
    return v > 0 && v < 4000 ? v : 0;
}

}  // namespace

// =============================================================================================
// Setup
// =============================================================================================

bool GameScene::loadReplay(const std::string& path, int game) {
    archive::LoadResult r = archive::loadFile(path, game);
    if (!r.ok) {
        LOGW("replay: game %d of %s cannot be read: %s", game + 1, path.c_str(), r.error.c_str());
        ui::notify(i18n::trf("library.error.game", {r.error}), 5.0f);
        return false;
    }
    replayRecord_ = std::move(r.record);
    return true;
}

TimeControl GameScene::replayTimeControl() const {
    // The record's time control, for the robots' clock presses; an untimed game without one (no
    // TimeControl tag, no clocks), whose moves are completed as their last piece is released.
    TimeControl tc;
    int64_t base = -1, inc = 0;
    if (pgn::parseTimeControl(replayRecord_.tag("TimeControl"), base, inc) && base > 0) {
        tc.unlimited = false;
        tc.baseMs = base;
        tc.incrementMs = inc;
    } else if (replayRecord_.hasClocks()) {
        tc.unlimited = false;   // clocks without a time control (the display reads them anyway)
        for (const pgn::Ply& p : replayRecord_.plies) tc.baseMs = std::max(tc.baseMs, p.clockMs);
    } else {
        tc.unlimited = true;
    }
    return tc;
}

void GameScene::configureReplaySeats() {
    for (int i = 0; i < 2; ++i) {
        Seat& st = seats_[i];
        character::Side hand = st.playHand;  // set by initAnimators()
        st = Seat();
        st.color = colorOfSeat(i);
        st.playHand = hand;
        st.controller = Controller::Stockfish;  // a robot plays the record's moves (no engine)
        std::string name = replayRecord_.tag(i == 0 ? "White" : "Black");
        st.name = name.empty() || name == "?" ? std::string(i18n::tr("library.unknown_player")) : name;
        st.elo = tagElo(replayRecord_, i == 0 ? "WhiteElo" : "BlackElo");
    }
}

Scorekeeper::Details GameScene::replaySheetDetails() const {
    Scorekeeper::Details d;
    // The game's own saves keep the sheets as they were ("Scacelith"); an imported game its event.
    const std::string event = replayRecord_.tag("Event");
    if (!replayRecord_.findTag("ScacelithMode") && !event.empty() && event != "?") d.event = event;
    const std::string round = replayRecord_.tag("Round");
    d.round = round.empty() || round == "?" ? std::string("-") : round;
    d.noBoard = true;
    return d;
}

std::string GameScene::replaySheetDate() const {
    // "2026.10.01" as the language writes dates on a scoresheet; unknown parts ("????.??.??") stay
    // question marks, no date at all leaves the field blank.
    const std::string date = replayRecord_.tag("Date", replayRecord_.tag("UTCDate"));
    if (date.size() != 10 || date[4] != '.' || date[7] != '.') return std::string();
    return i18n::trf("scoresheet.date_format", {date.substr(8, 2), date.substr(5, 2), date.substr(0, 4)});
}

void GameScene::setupReplay() {
    replay::Options opts;
    opts.moveAnimationMs = untimed() ? kRobotMoveUntimedMs : kRobotMoveMs;
    replayClock_.load(replayRecord_, opts);   // its first event sets the start position (done already)
    // Black to move first: White's cell of the first row stays "..." (the header is written).
    if (replaySheetOffset()) scorekeeper_.writeMovesInstantly({"..."});
    replayClock_.setSpeed(replaySpeedArg_);
    if (replayPausedArg_) replayClock_.pause();
    // The lever is down on the side of the player who would have moved last (a FEN game may start
    // with Black to move).
    Color last = opposite(game_.position().sideToMove());
    leverSide_ = leverTarget_ = world_.clockHalfForSeat(seatOf(last) == 0 ? 1.0f : -1.0f) == 1 ? 1.0f : -1.0f;
}

// =============================================================================================
// Frame update
// =============================================================================================

void GameScene::updateReplay(float dt) {
    // The short dip of a board set at once (a step back, a jump).
    if (fadeDip_ > 0.0f) {
        fade_ = std::max(fade_, fadeDip_);
        fadeDip_ = 0.0f;
    }
    if (state_ == State::Playing || state_ == State::GameOver) fade_ = std::max(0.0f, fade_ - dt / 1.2f);
    if (!paused_ && state_ == State::Playing) replayClock_.update(dt);
    replay::Event e;
    while (replayClock_.poll(e)) {
        if (e.kind == replay::Event::Kind::SetPosition) {
            setReplayPosition(e.ply);
        } else if (e.kind == replay::Event::Kind::Finished) {
            endReplay();
        } else if (state_ == State::Playing) {
            // The record's move, by the robot of the side to move (pgn::read played it already: it
            // is legal; its flags are taken from the position on the board).
            const pgn::Ply& p = replayRecord_.plies[size_t(e.ply)];
            Move mv = game_.position().findLegal(p.move.from, p.move.to, p.move.promotion);
            if (e.ply != int(game_.moves().size()) || !mv.valid()) {
                LOGW("replay: move %d (%s) does not follow the board: set up at once", e.ply + 1, p.san.c_str());
                replayClock_.jumpTo(e.ply + 1);
                continue;
            }
            playRobotMove(seatOf(game_.position().sideToMove()), mv);
            replayMoveAt_ = time_;
        }
    }
    // --replay-keys: the next key once the board is still (between two moves, or paused).
    if (replayKeysPos_ < replayKeys_.size() && (state_ == State::Playing || (state_ == State::GameOver && stateTime_ > 1.0f))) {
        replayKeyWait_ -= dt;
        const bool still = turn_ != Turn::AiMoving && !replayClock_.moving() && dest_.empty() && !anim_[0].busy() &&
                           !anim_[1].busy();
        if (still && replayKeyWait_ <= 0.0f) {
            const std::string& k = replayKeys_[replayKeysPos_++];
            LOGI("--replay-keys: %s (at ply %d)", k.c_str(), replayClock_.ply());
            if (k == "Leave") {
                // Esc, then "Main menu" in the viewer's pause menu (menuChoice).
                if (state_ == State::Playing) {
                    paused_ = true;
                    scriptMenu_ = ui::MenuAction::BackToMainMenu;
                }
            } else if (!replayKey(k)) {
                LOGW("--replay-keys: '%s' is not K, J, L, Shift+J, Shift+L, Home, End or Leave", k.c_str());
            }
            replayKeyWait_ = 0.6f;
        }
    }
    if (state_ != State::Playing) return;
    // The player to move thinks (idle variations) until the robot reaches for the piece.
    int s = seatOf(replayClock_.toMove());
    anim_[s].setThinking(!replayClock_.moving() && !replayClock_.finished() && turn_ != Turn::AiMoving);
    anim_[1 - s].setThinking(false);
    // Untimed record: no clock press, the move is completed as its last piece is released.
    if (turn_ == Turn::AiMoving && untimed() && dest_.empty()) completeMove(seatOf(game_.position().sideToMove()));
}

void GameScene::completeReplayMove(int seat, const Arbiter::Verdict& v) {
    if (!v.legal) {
        // Never expected (the record's moves are legal): the board is set after the move.
        LOGW("replay: move %d refused by the arbiter: set up at once", int(game_.moves().size()) + 1);
        replayClock_.jumpTo(int(game_.moves().size()) + 1);
        return;
    }
    game_.play(v.move);
    int ply = int(game_.moves().size()) - 1;
    scorekeeper_.recordMove(ply + replaySheetOffset(), game_.sanMoves().back());
    replayClock_.moveDone();
    const replay::ClockView c = replayClock_.clocks();
    LOGI("replay: ply %d %s at %.2f s (robot %.2f s), clocks %lld / %lld ms", ply + 1, game_.sanMoves().back().c_str(),
         double(time_), double(time_ - replayMoveAt_), (long long)c.ms[0], (long long)c.ms[1]);
    beginTurn();
    followEyesAfterMove(seat);
}

void GameScene::setReplayPosition(int ply) {
    ply = std::clamp(ply, 0, int(replayRecord_.plies.size()));
    // Nothing to set: the board shows that position and nothing moves (the replay's first event).
    if (state_ == State::Playing && ply == int(game_.moves().size()) && turn_ != Turn::AiMoving && dest_.empty() &&
        !anim_[0].busy() && !anim_[1].busy()) {
        beginTurn();
        return;
    }
    const bool fromEnd = state_ == State::GameOver;
    // The game at that ply.
    game_.reset();
    if (!replayRecord_.fen.empty()) game_.resetFromFEN(replayRecord_.fen);
    for (int i = 0; i < ply; ++i) {
        const pgn::Ply& p = replayRecord_.plies[size_t(i)];
        Move mv = game_.position().findLegal(p.move.from, p.move.to, p.move.promotion);
        if (!mv.valid() || !game_.play(mv)) {
            LOGE("replay: move %d (%s) cannot be played", i + 1, p.san.c_str());
            break;
        }
    }
    arbiter_.reset(game_);
    // The hands let go and rest, the pens go back on the table; the board and fresh sheets are set
    // up at once, the moves already written, then the pens are taken up again.
    dest_.clear();
    initAnimators();
    for (auto& a : anim_) a.setHeadOverride(false);
    board_.syncTo(game_.position());
    LOGI("replay: board set at ply %d: %s", int(game_.moves().size()), game_.position().fen().c_str());
    newScoresheets();
    scorekeeper_.setDetails(replaySheetDetails());
    scorekeeper_.writeHeaderInstantly();
    std::vector<std::string> sheetMoves(size_t(replaySheetOffset()), std::string("..."));
    sheetMoves.insert(sheetMoves.end(), game_.sanMoves().begin(), game_.sanMoves().end());
    if (!sheetMoves.empty()) scorekeeper_.writeMovesInstantly(sheetMoves);
    // The lever is down on the side of the player who moved last.
    Color moved = opposite(game_.position().sideToMove());
    leverTarget_ = leverSide_ = world_.clockHalfForSeat(seatOf(moved) == 0 ? 1.0f : -1.0f) == 1 ? 1.0f : -1.0f;
    fadeDip_ = std::max(fadeDip_, 0.4f);
    if (fromEnd) {
        // Back from the result card: the game goes on.
        state_ = State::Playing;
        stateTime_ = 0.0f;
        gameOverShown_ = endHandshakeDone_ = false;
    }
    scorekeeper_.startRecording();
    turn_ = Turn::None;
    beginTurn();
    if (followEyes_) {
        // Through the eyes of the player to move, at once.
        eyesSeat_ = seatOf(game_.position().sideToMove());
        eyesSmooth_ = eyePose(eyesSeat_);
        observer_.setPose(eyesSmooth_);
        if (clockFrozen_) setClockFrozen(false);
    }
}

int GameScene::replaySheetOffset() const {
    // game_ starts from the record's start position (setupNewGame, setReplayPosition).
    return game_.startPosition().sideToMove() == Black ? 1 : 0;
}

int GameScene::replayMoveNumber(int plies) const {
    return std::max(1, game_.startPosition().fullmoveNumber()) + std::max(0, plies + replaySheetOffset() - 1) / 2;
}

void GameScene::endReplay() {
    if (state_ != State::Playing) return;
    endGame();  // the record's result (endGame's replay branch); nothing rated, nothing saved
}

ui::GameOverExtras GameScene::replayGameOverExtras() const {
    ui::GameOverExtras x;
    const std::string& r = replayRecord_.result;
    // The move as the record numbers it (a FEN game may start at move 40).
    std::string moveNo = std::to_string(replayMoveNumber(int(game_.moves().size())));
    if (r == "1-0") x.line = i18n::trf("viewer.gameover.white_wins", {moveNo});
    else if (r == "0-1") x.line = i18n::trf("viewer.gameover.black_wins", {moveNo});
    else if (r == "1/2-1/2") x.line = i18n::trf("viewer.gameover.draw", {moveNo});
    else x.line = i18n::trf("replay.gameover.unfinished", {moveNo});   // never the player's win or loss
    // The players as the record names them ("?" for an unknown name), their rating when it has one.
    auto label = [this](int i) {
        const Seat& st = seats_[i];
        return st.elo > 0 ? st.name + " (" + std::to_string(st.elo) + ")" : st.name;
    };
    x.detail = i18n::trf("viewer.gameover.players", {label(0), label(1)});
    x.primaryLabel = i18n::tr("replay.again");
    return x;
}

// =============================================================================================
// Controls
// =============================================================================================

void GameScene::updateReplayInput() {
    // Free keys of the viewer (its camera takes the arrows, WASD / ZQSD, E, C, Space, Ctrl, Shift,
    // PageUp / PageDown, 0-9, H, Tab): J K L as in video players, Home and End.
    const plat::Input& in = plat::input();
    const bool shift = in.keyDown[plat::KEY_LSHIFT] || in.keyDown[plat::KEY_RSHIFT];
    if (in.keyPressed['K']) replayKey("K");
    if (in.keyPressed['J']) replayKey(shift ? "Shift+J" : "J");
    if (in.keyPressed['L']) replayKey(shift ? "Shift+L" : "L");
    if (in.keyPressed[plat::KEY_HOME]) replayKey("Home");
    if (in.keyPressed[plat::KEY_END]) replayKey("End");
}

bool GameScene::replayKey(const std::string& key) {
    if (key == "K") replayClock_.togglePause();
    else if (key == "J") replayClock_.stepBack();
    else if (key == "L") replayClock_.stepForward();
    else if (key == "Shift+J") replayClock_.setSpeed(replay::slower(replayClock_.speed()));
    else if (key == "Shift+L") replayClock_.setSpeed(replay::faster(replayClock_.speed()));
    else if (key == "Home") replayClock_.jumpTo(0);
    else if (key == "End") replayClock_.jumpTo(replayClock_.plies());
    else return false;
    return true;
}

void GameScene::drawReplayBar() {
    ui::ReplayBar b;
    const int ply = replayClock_.ply(), plies = replayClock_.plies();
    // Full moves: White's 12th and Black's 12th are both move 12. Counted from the record's first
    // move, as the rows of the sheets (a FEN game from "40... Kd7" to "43. Ke3": 4 moves).
    const int off = replaySheetOffset();
    b.move = ply > 0 ? (ply + off + 1) / 2 : 0;
    b.moves = plies > 0 ? (plies + off + 1) / 2 : 0;
    replay::Speed sp = replayClock_.speed();
    b.speed = sp == replay::Speed::Instant ? std::string(i18n::tr("replay.speed.instant"))
                                           : i18n::trf("replay.speed.factor", {std::to_string(int(replay::speedFactor(sp)))});
    b.paused = replayClock_.paused();
    b.atStart = ply == 0 && !replayClock_.moving();
    b.atEnd = replayClock_.finished();
    b.aboveCard = state_ == State::GameOver && gameOverShown_;
    switch (ui::replayBar(b)) {
    case ui::ReplayAction::Start: replayClock_.jumpTo(0); break;
    case ui::ReplayAction::Back: replayClock_.stepBack(); break;
    case ui::ReplayAction::TogglePause: replayClock_.togglePause(); break;
    case ui::ReplayAction::Forward: replayClock_.stepForward(); break;
    case ui::ReplayAction::End: replayClock_.jumpTo(plies); break;
    default: break;
    }
}

ClockDisplay GameScene::replayClockDisplay() const {
    ClockDisplay d;
    replay::ClockView v = replayClock_.clocks();
    int hw = world_.clockHalfForSeat(1.0f), hb = 1 - hw;
    d.dashes = !v.known;
    d.ms[hw] = v.ms[0];
    d.ms[hb] = v.ms[1];
    // Before the first move (the handshake) and after the last, nothing runs.
    d.running = state_ != State::Playing || v.running < 0 ? -1 : (v.running == 0 ? hw : hb);
    d.unlimited = false;
    d.paused = state_ == State::Playing && (replayClock_.paused() || paused_);
    d.leverSide = leverSide_;
    return d;
}

}  // namespace game
