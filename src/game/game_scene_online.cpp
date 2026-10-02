// Online play in the 3D scene: the opponent is a player of the Scacelith server or of a direct
// match, and sits in the other chair as a robot (Controller::Remote).
//
// The authority (server or direct-match host) owns the game: its clocks, the legality of the
// moves and the result. The scene keeps the physical experience of a game against Stockfish:
//   - My move: the piece is touched and carried as usual, but it can only be released on a legal
//     square (no arbiter penalty online). When the authority lets the robots press the clock
//     (og_.autoPress: the server's rule, on by default, or the host's choice in a direct match),
//     the move is sent the moment its destination (and promotion) is chosen, before the hand
//     moves; the robot then places it and presses the clock by itself (animation only).
//     Otherwise the move is staged: the robot places it, the player presses the clock (Space or a
//     click, as against Stockfish) and the move goes at the lever contact, so that its thinking
//     time covers the placement and the press. The thinking time is measured on a steady local
//     clock. My clock display stands still from the send until the authority confirms, at most
//     max(1 s, 3 pings) and never while reconnecting; a move the authority never got (the
//     connection dropped) is sent again when the snapshot after the reconnection lacks only it.
//   - The opponent's move (MoveMade) is played by its robot like a Stockfish move but without
//     thinking time: touch, carry, capture, castling rook, promotion swap, clock, at once and from
//     where their gestures left it (the piece in hand, the move already put down). I may touch my
//     pieces as soon as its pieces are down.
//   - Live gestures (net/gesture.h; the rules are in online_live.h). Mine: the piece in hand, the
//     square it is aimed at (after a short dwell), the promotion picker, a staged move, and my
//     head (look, lean, the glance at my scoresheet, a look beside the board) go to the opponent
//     when they change, once a second at least, while the game is played. The opponent's drive
//     their robot: it takes the piece they touch, carries it over the square they aim at, puts a
//     staged move down before their press, lets go of the piece when they do, and its head and
//     lean follow theirs (unless Options > Gameplay ignores the opponent's head). Gestures are
//     cosmetic and come from the other client: their squares are checked, they apply only to the
//     move being prepared, and they never touch the game, the arbiter, the clocks or og_. Nor my
//     turn: the robot's hand takes one step at a time from their latest gesture however fast they
//     come, and their MoveMade drops at once whatever it has left that is not that move.
//   - The clock shows the server's times (extrapolated with serverNowMs()), never flags locally
//     (it stops at 0.0 until the server's GameEnd).
//   - MoveRejected / a snapshot that disagrees: once the robots are idle the board, the game and
//     the scoresheets are rebuilt from the server's move list (a short fade hides the snap).
//     After a reconnection the moves missed are set up the same way, without animation.
//   - GameEnd waits until the last move is on the board, then the usual end (result on the
//     scoresheets, handshake, game over card with online reasons, rating change, rematch, report).
#include "game_scene.h"
#include "game_scene_detail.h"
#include "../audio/audio.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "../platform/platform.h"
#include "../ui/ui_online.h"
#include "online_mock.h"
#include "settings.h"
#include <algorithm>
#include <cmath>

using namespace m;
using namespace chess;

namespace game {

using namespace scene_detail;

namespace {

using Kind = net::Event::Kind;

// Protocol values (dedicated-server/src/protocol/schema.js).
enum Status { StOngoing = 0, StWhiteWins = 1, StBlackWins = 2, StDraw = 3, StAborted = 4 };
enum GameEventKind { EvDrawOffered = 1, EvDrawDeclined = 2, EvDisconnected = 3, EvReconnected = 4, EvRematchOffered = 5, EvRematchDeclined = 6 };
constexpr int kErrDrawOfferLimit = 108;

uint16_t packed(const Move& m) { return net::packMove(m.from, m.to, m.promotion); }

Move unpack(const Position& pos, uint16_t mv) {
    return pos.findLegal(Square(net::moveFrom(mv)), Square(net::moveTo(mv)), PieceType(net::movePromo(mv)));
}

// "1512", "1500?" (provisional), "" (no rating: direct match).
std::string ratingText(const net::PlayerInfo& p) {
    if (p.rating <= 0) return "";
    return std::to_string(p.rating) + (p.provisional ? "?" : "");
}

// Reason line of the game over card: chess reasons 0..13, online ones from 20.
std::string reasonText(int reason) {
    switch (reason) {
    case 20: return i18n::tr("reason.online.abandonment");
    case 21: return i18n::tr("reason.online.abandonment_vs_insufficient");
    case 22: return i18n::tr("reason.online.aborted");
    case 23: return i18n::tr("reason.online.no_show");
    case 24: return i18n::tr("reason.online.forfeit");
    case 25: return i18n::tr("reason.online.server_aborted");
    case 26: return i18n::tr("reason.online.both_disconnected");
    default: return reason > 0 && reason <= 13 ? endReasonText(GameEndReason(reason)) : "";
    }
}

}  // namespace

// =============================================================================================
// Setup
// =============================================================================================

void GameScene::initOnline() {
    bool virtualClock = ctx_->screenshotMode || ctx_->hasArg("--warp");
    virtualTime_ = virtualClock;
    onlineSession().init(ctx_->hasArg("--online-mock"), virtualClock);
    if (ctx_->hasArg("--start-online")) {
        startOnline_ = ctx_->argValue("--start-online");
        if (startOnline_.empty() || startOnline_[0] == '-') startOnline_ = "5+3";
        startTouch_ = ctx_->argValue("--touch");
    }
}

bool GameScene::takeOnlineGame() {
    if (!onlineSession().gameReady()) return false;
    mode_ = GameMode::Online;
    setupNewGame();
    return link_ != nullptr;
}

void GameScene::setupOnlineGame() {
    OnlineSession& s = onlineSession();
    link_ = s.takeGame(og_);
    // Kept for the saved games: link_ is gone by the time a game left is saved.
    directMatch_ = link_ && link_->kind() == LinkKind::Direct;
    humanColor_ = og_.you == 1 ? Black : White;
    remoteQueue_.clear();
    pendingPly_ = -1;
    pendingMove_ = 0;
    recordedPly_ = 0;
    pressedPly_ = remotePly_ = -1;
    promoTo_ = NoSquare;
    resync_ = rebuildFade_ = endPending_ = false;
    endWait_ = 0.0f;
    drawOffered_ = myDrawOffer_ = opponentAway_ = false;
    ratingKnown_ = false;
    ratingBefore_ = ratingAfter_ = 0;
    rematchAsked_ = rematchOffered_ = rematchGone_ = false;
    reportOpen_ = reported_ = false;
    reportCategory_ = 0;
    reportComment_.clear();
    fadeDip_ = 0.0f;
    turnStartMs_ = localMs();
    pendingFen_.clear();
    pendingThinkMs_ = 0;
    moveStaged_ = false;
    gestureBuiltAny_ = gestureSentAny_ = gestureFinal_ = false;
    gestureSinceMs_ = 0.0;
    aimDwell_.reset();
    aimDwellFor_ = NoSquare;
    remoteLive_ = RemoteLive();
    remoteLiveEnd_ = 0.0f;
    remoteGesture_ = net::Gesture();
    remoteAge_ = 1e9f;
    remoteFresh_ = false;
    remoteAim_.reset();
    remoteHeadOn_ = remoteGlancing_ = false;
    remoteGlanceBlend_ = 0.0f;
    LOGI("New online game %llu: you play %s against %s, %s%s", (unsigned long long)og_.id, humanColor_ == White ? "White" : "Black",
         (humanColor_ == White ? og_.black : og_.white).name.c_str(), og_.category.c_str(), og_.rated ? " rated" : "");
}

void GameScene::configureOnlineSeats() {
    bool direct = link_ && link_->kind() == LinkKind::Direct;
    for (int i = 0; i < 2; ++i) {
        Seat& st = seats_[i];
        character::Side hand = st.playHand;  // set by initAnimators()
        st = Seat();
        st.color = colorOfSeat(i);
        st.playHand = hand;
        const net::PlayerInfo& p = i == 0 ? og_.white : og_.black;
        st.controller = st.color == humanColor_ ? Controller::Human : Controller::Remote;
        st.name = p.name.empty() ? (st.human() ? localPlayerName() : std::string("?")) : p.name;
        if (!direct) {
            st.elo = p.rating;
            st.provisional = p.provisional;
            st.ratingText = ratingText(p);
        }
    }
}

Scorekeeper::Details GameScene::onlineSheetDetails() const {
    Scorekeeper::Details d;
    d.round = "-";
    d.noBoard = true;
    std::string tc = og_.category != "custom" && !og_.category.empty()
                         ? og_.category
                         : std::to_string(og_.baseMs / 60000) + "+" + std::to_string(og_.incMs / 1000);
    if (og_.baseMs % 60000 != 0 && og_.category == "custom") {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%d:%02d+%d", int(og_.baseMs / 60000), int(og_.baseMs / 1000 % 60), int(og_.incMs / 1000));
        tc = buf;
    }
    if (link_ && link_->kind() == LinkKind::Direct) {
        d.event = link_->eventName();
        d.note = i18n::trf("direct.sheet.note", {tc});
    } else {
        d.event = link_ ? link_->eventName() : std::string("Scacelith");
        d.note = i18n::trf("online.sheet.note", {tc, i18n::tr(og_.rated ? "online.sheet.rated" : "online.sheet.casual")});
        d.reference = i18n::trf("online.sheet.game", {std::to_string(og_.id)});
    }
    return d;
}

// =============================================================================================
// Frame update
// =============================================================================================

void GameScene::updateOnline(float dt) {
    OnlineSession& s = onlineSession();
    // Developer switches of the fakes: F9 the opponent leaves for 20 s, F10 our connection drops.
    if (s.mock() && online() && state_ == State::Playing && !ui::wantsKeyboard()) {
        const plat::Input& in = plat::input();
        if (in.keyPressed[plat::KEY_F9]) net::mock::opponentDrop(20);
        if (in.keyPressed[plat::KEY_F10]) net::mock::connectionDrop(8);
    }
    // Notices that must not interrupt a game wait until none is being played (any mode but the
    // viewer's; the game over card is fine).
    bool inGame = !watching() && (state_ == State::FadeToGame || state_ == State::Intro || state_ == State::Handshake ||
                                  state_ == State::Playing);
    s.setInGame(inGame);
    s.update(dt);
    if (!online() || !link_) return;
    if (state_ != State::Intro && state_ != State::Handshake && state_ != State::Playing && state_ != State::GameOver) return;
    net::Event e;
    while (s.nextGameEvent(e)) onlineEvent(e);
    remoteAge_ += dt;
    if (link_->reconnecting()) remoteFresh_ = false;  // a gesture from before our reconnection is stale
    sendOnlineGesture(dt);
    settleRemoteTakeBack();
    if (state_ != State::Playing) return;
    // --touch with --start-online: the hand goes to that piece once the handshake is over.
    if (!startTouch_.empty() && turn_ == Turn::HumanIdle && !anim_[humanSeat()].busy()) {
        int id = board_.idAt(parseSquare(startTouch_));
        startTouch_.clear();
        if (id >= 0) humanTouch(id);
    }
    if (fadeDip_ > 0.0f) {
        fade_ = std::max(fade_, fadeDip_);
        fadeDip_ = 0.0f;
    }
    fade_ = std::max(0.0f, fade_ - dt / 1.2f);  // the fade-in of the game, or a rebuild's dip

    if (resync_) {
        // The server's state wins: once nothing is in a hand, the board is set up from it.
        if (turn_ == Turn::HumanTouched) humanRelease();
        if (turn_ == Turn::HumanPromotion) {
            promoTo_ = NoSquare;
            humanRelease();
        }
        cancelRemoteLive();
        if (!anim_[0].busy() && !anim_[1].busy() && dest_.empty()) rebuildOnline();
        return;
    }
    // The opponent's move: their robot plays it at once, from where their gestures left it.
    if (turn_ == Turn::RemoteWaiting && !remoteQueue_.empty()) startRemoteMove();
    // The opponent's pieces are down: my turn begins while its hand goes to the clock.
    if (turn_ == Turn::RemoteMoving && dest_.empty()) beginTurn();
    updateRemoteLive(dt);

    if (endPending_) {
        // The game is over: an open promotion picker closes, its pawn going back.
        if (turn_ == Turn::HumanPromotion) {
            promoTo_ = NoSquare;
            humanRelease();
        }
        endWait_ += dt;
        bool settled = remoteQueue_.empty() && turn_ != Turn::RemoteMoving && turn_ != Turn::HumanPromotion && dest_.empty() &&
                       !anim_[0].busy() && !anim_[1].busy() && !remoteLive_.takeBack;
        if (settled || endWait_ > 6.0f) endGame();
    }
}

void GameScene::onlineEvent(const net::Event& e) {
    switch (e.kind) {
    case Kind::GameSnapshot: onlineSnapshot(e.game); break;
    case Kind::MoveMade:
        og_ = e.game;
        if (e.mine) {
            if (e.ply == pendingPly_) {
                pendingPly_ = -1;
                if (pressedPly_ >= e.ply) recordOnline(e.ply);
            }
            break;
        }
        // The opponent moved: my thinking time starts now, whatever the robot still has to do.
        remoteQueue_.push_back({e.ply, e.move});
        turnStartMs_ = localMs();
        myDrawOffer_ = myDrawOffer_ && og_.drawOfferBy == int(humanColor_);
        break;
    case Kind::OpponentGesture:
        // Cosmetic: their hand and head for their robot (updateRemoteLive, driveRemoteHead). It
        // never changes og_, the game or the clocks.
        remoteGesture_ = e.gesture;
        remoteAge_ = 0.0f;
        remoteFresh_ = true;
        break;
    case Kind::MoveRejected:
        LOGW("online: move %d refused (code %d)", e.ply, e.code);
        ui::notify(serverErrorText(e.code), 4.0f);
        pendingPly_ = -1;
        pressQueued_ = false;
        resync_ = rebuildFade_ = true;
        break;
    case Kind::GameEvent: onlineGameEvent(e); break;
    case Kind::GameEnd:
        og_ = e.game;
        drawOffered_ = false;
        if (state_ == State::Playing || state_ == State::Intro || state_ == State::Handshake) {
            endPending_ = true;
            endWait_ = 0.0f;
            dropStagedMove();
            // The opponent's hand lets go, unless their last move is on its way to the robot.
            if (!remoteMoveQueued(remoteLive_.ply)) cancelRemoteLive();
        }
        break;
    case Kind::RatingUpdate: {
        const net::Event::Rating& mine = humanColor_ == White ? e.ratingWhite : e.ratingBlack;
        ratingKnown_ = true;
        ratingBefore_ = mine.before;
        ratingAfter_ = mine.after;
        LOGI("online: rating %d -> %d", mine.before, mine.after);
        break;
    }
    case Kind::ServerError:
        ui::notify(eventErrorText(e), 4.0f);
        if (e.code == kErrDrawOfferLimit) myDrawOffer_ = false;
        break;
    default: break;
    }
}

void GameScene::onlineSnapshot(const net::OnlineGame& g) {
    og_ = g;
    remoteFresh_ = false;  // only a gesture sent after it moves pieces again
    if (g.status != StOngoing && state_ != State::GameOver) {
        endPending_ = true;
        endWait_ = 0.0f;
        dropStagedMove();
        if (!remoteMoveQueued(remoteLive_.ply)) cancelRemoteLive();
    }
    const std::vector<Move>& local = game_.moves();
    size_t n = std::min(local.size(), g.moves.size());
    bool prefix = true;
    for (size_t i = 0; i < n && prefix; ++i) prefix = packed(local[i]) == g.moves[i].move;
    if (prefix && local.size() == g.moves.size()) {
        if (pendingPly_ >= 0 && size_t(pendingPly_) < g.moves.size()) {
            pendingPly_ = -1;  // the snapshot confirms my move
            if (pressedPly_ >= 0) recordOnline(pressedPly_);
        }
        return;
    }
    if (prefix && local.size() < g.moves.size()) {
        // Moves I have not seen yet: fine when they are all waiting for the robot.
        bool queued = true;
        for (size_t i = local.size(); i < g.moves.size() && queued; ++i) {
            size_t k = i - local.size();
            queued = k < remoteQueue_.size() && remoteQueue_[k].ply == int(i) && remoteQueue_[k].move == g.moves[i].move;
        }
        if (queued) return;
        resync_ = true;  // missed while reconnecting: set up without animation
        return;
    }
    std::vector<uint16_t> mine, theirs;
    for (const Move& m : local) mine.push_back(packed(m));
    for (const net::OnlineGame::MoveRec& rec : g.moves) theirs.push_back(rec.move);
    if (live::resendPendingMove(mine, theirs, pendingPly_, int(humanColor_), g.status == StOngoing)) {
        // My move never reached the authority (the connection dropped): the same move again,
        // instead of making me play it once more.
        link_->sendMove(pendingPly_, pendingMove_, pendingFen_, pendingThinkMs_, false);
        clockFreeze_.start(localMs(), onlineClockMs(int(humanColor_)));
        LOGI("online: move %d sent again after the reconnection", pendingPly_ + 1);
        return;
    }
    resync_ = rebuildFade_ = true;  // my move was refused
}

void GameScene::onlineGameEvent(const net::Event& e) {
    int me = int(humanColor_), opp = 1 - me;
    og_ = e.game;
    switch (e.gameEventKind) {
    case EvDrawOffered:
        if (e.color == opp) {
            drawOffered_ = true;
            audio::playUI(audio::Sfx::UIClick, 0.5f);
        } else {
            myDrawOffer_ = true;
            ui::notify(i18n::tr("online.draw.offered"), 3.0f);
        }
        break;
    case EvDrawDeclined:
        if (e.color == opp) {
            if (myDrawOffer_) ui::notify(i18n::tr("notify.draw_declined"), 3.0f);
            myDrawOffer_ = false;
        } else {
            drawOffered_ = false;
        }
        break;
    case EvDisconnected:
        if (e.color == opp) {
            opponentAway_ = true;
            opponentBackBy_ = link_->serverNowMs() + double(e.arg);
            // Their robot lets go of what it held for them; their next gestures take it again.
            remoteFresh_ = false;
            if (!remoteMoveQueued(remoteLive_.ply)) cancelRemoteLive();
        }
        break;
    case EvReconnected:
        if (e.color == opp) remoteFresh_ = false;  // wait for a gesture sent after their return
        if (e.color == opp && opponentAway_) {
            opponentAway_ = false;
            ui::notify(i18n::tr("online.opponent_back"), 3.0f);
        }
        break;
    case EvRematchOffered:
        if (e.color == opp) rematchOffered_ = true;
        break;
    case EvRematchDeclined:
        if (e.color != me) {
            if (rematchAsked_ || rematchOffered_) ui::notify(i18n::tr("online.rematch.declined"), 3.0f);
            rematchGone_ = true;
            rematchOffered_ = false;
        }
        break;
    default: break;
    }
}

// =============================================================================================
// Board and moves
// =============================================================================================

void GameScene::rebuildOnline() {
    resync_ = false;
    game_.reset();
    for (const net::OnlineGame::MoveRec& rec : og_.moves) {
        Move mv = unpack(game_.position(), rec.move);
        if (!mv.valid()) {
            LOGE("online: move %d of the server's game is not legal here", int(game_.moves().size()) + 1);
            break;
        }
        game_.play(mv);
    }
    int n = int(game_.moves().size());
    arbiter_.reset(game_);
    dest_.clear();
    board_.syncTo(game_.position());
    remoteQueue_.erase(std::remove_if(remoteQueue_.begin(), remoteQueue_.end(), [n](const RemoteMove& r) { return r.ply < n; }),
                       remoteQueue_.end());
    pendingPly_ = -1;
    promoTo_ = NoSquare;
    moveStaged_ = false;
    // The opponent's robot holds nothing any more (the hands were idle): their latest gesture is
    // applied again from this position.
    remoteLive_ = RemoteLive();
    remoteAim_.reset();
    if (recordedPly_ < n) {
        scorekeeper_.writeMovesInstantly(game_.sanMoves());
        recordedPly_ = n;
    }
    pressedPly_ = n - 1;
    // The lever is down on the side of the player who moved last.
    Color moved = opposite(game_.position().sideToMove());
    int half = world_.clockHalfForSeat(seatOf(moved) == 0 ? 1.0f : -1.0f);
    leverTarget_ = half == 1 ? 1.0f : -1.0f;
    if (rebuildFade_) fadeDip_ = 0.6f;
    rebuildFade_ = false;
    turnStartMs_ = localMs();
    LOGI("online: board rebuilt from the server (%d moves)", n);
    if (state_ == State::Playing) beginTurn();
}

double GameScene::localMs() const { return virtualTime_ ? double(time_) * 1000.0 : plat::time() * 1000.0; }

void GameScene::placeOnlineMove(const Move& mv) {
    if (autoPressClock()) {
        sendOnlineMove(mv);
        return;
    }
    stagedMove_ = mv;
    moveStaged_ = true;
}

void GameScene::pressOnlineClock() {
    if (!moveStaged_) return;
    // My clock press: the staged move goes, unless the game ended or is being rebuilt meanwhile.
    if (endPending_ || resync_ || og_.status != StOngoing) {
        dropStagedMove();
        return;
    }
    moveStaged_ = false;
    sendOnlineMove(stagedMove_);
}

void GameScene::dropStagedMove() {
    if (!moveStaged_) return;
    // The move stands on the board but never went: the board is set up again from the
    // authority's moves once the hands are idle, behind a short fade.
    moveStaged_ = false;
    pressQueued_ = false;
    resync_ = rebuildFade_ = true;
}

void GameScene::sendOnlineMove(const Move& mv) {
    int ply = int(game_.moves().size());
    uint16_t pm = packed(mv);
    // The thinking time on a steady clock: the estimate of the authority's clock may be corrected
    // between two readings, which would make an honest time look implausible.
    double now = localMs();
    pendingThinkMs_ = uint32_t(std::max(0.0, now - turnStartMs_));
    pendingFen_ = game_.position().fen();
    clockFreeze_.start(now, onlineClockMs(int(humanColor_)));
    // While the connection is being restored the move waits: the snapshot that follows the
    // reconnection sends it (onlineSnapshot).
    if (!link_->reconnecting()) link_->sendMove(ply, pm, pendingFen_, pendingThinkMs_, false);
    pendingPly_ = ply;
    pendingMove_ = pm;
    game_.play(mv);
    arbiter_.reset(game_);
    drawOffered_ = false;  // moving declines the opponent's offer
    LOGI("online: move %d %s sent (%u ms)", ply + 1, game_.sanMoves().back().c_str(), pendingThinkMs_);
}

void GameScene::startRemoteMove() {
    RemoteMove r = remoteQueue_.front();
    remoteQueue_.erase(remoteQueue_.begin());
    int ply = int(game_.moves().size());
    if (r.ply < ply) return;  // already on the board
    Move mv = r.ply == ply ? unpack(game_.position(), r.move) : Move();
    if (!mv.valid()) {
        LOGW("online: the opponent's move %d does not follow the local game: resynchronising", r.ply + 1);
        resync_ = true;
        link_->requestResync();
        return;
    }
    Color side = game_.position().sideToMove();
    int seat = seatOf(side);
    // What their gestures already did: the move put down (only the clock press is left), or the
    // piece in hand (the hand goes on from where it is).
    const RemoteLive& L = remoteLive_;
    live::LiveWork work;
    work.held = L.pieceId >= 0 ? int(L.from) : live::kNoSquare;
    work.ply = L.ply;
    work.placed = L.placed;
    work.takeBack = L.takeBack;
    work.before = L.pieceId >= 0 && L.reachAt > anim_[seat].time() + live::kHandSlack;
    work.busy = remoteLiveEnd_ > anim_[seat].time();
    live::LiveStart start = live::liveStart(work, r.ply, r.move);
    if (start == live::LiveStart::Cut) {
        // Anything else their gestures left to the robot (another piece in hand or going back, a
        // move put down that is not this one) is dropped at once and the board set back from the
        // game, behind a short dip when a piece had moved: the move takes its usual time whatever
        // came before it, so that gestures never hold my turn back while my clock runs.
        bool moved = L.placed != 0 || L.takeBack ||
                     std::any_of(board_.pieces().begin(), board_.pieces().end(), [](const PieceObject& p) { return p.held; });
        anim_[seat].cancelTasks();
        remoteLiveEnd_ = anim_[seat].time();
        dest_.clear();
        board_.syncTo(game_.position());
        if (moved) fadeDip_ = std::max(fadeDip_, 0.4f);
        LOGI("online: what the opponent's gestures left to their robot is dropped for their move %d", r.ply + 1);
    }
    std::vector<anim::Task> tasks;
    if (start != live::LiveStart::Placed) {
        if (start != live::LiveStart::Held) tasks.push_back(task(anim::TaskType::Reach, board_.idAt(mv.from)));
        planMove(tasks, mv, game_.position().sideToMove(), start == live::LiveStart::Held);
    }
    remoteLive_ = RemoteLive();
    remoteAim_.reset();
    int half = world_.clockHalfForSeat(seat == 0 ? 1.0f : -1.0f);
    tasks.push_back(task(anim::TaskType::PressClock, -1, world_.clockPressPoint(half)));
    tasks.push_back(task(anim::TaskType::Retract));
    anim_[seat].setThinking(false);
    anim_[seat].enqueue(tasks);
    game_.play(mv);
    arbiter_.reset(game_);
    aiMoveTo_ = mv.to;
    remotePly_ = r.ply;
    turn_ = Turn::RemoteMoving;
}

void GameScene::recordOnline(int ply) {
    const std::vector<std::string>& san = game_.sanMoves();
    for (int p = recordedPly_; p <= ply && p < int(san.size()); ++p) scorekeeper_.recordMove(p, san[size_t(p)]);
    recordedPly_ = std::max(recordedPly_, std::min(ply + 1, int(san.size())));
}

// A move sent and not confirmed yet counts: the server already has it and would refuse an abort.
bool GameScene::myFirstMoveMade() const { return int(og_.moves.size()) > int(humanColor_) || pendingPly_ >= 0; }

// =============================================================================================
// Live gestures: mine to the opponent
// =============================================================================================

live::Hand GameScene::onlineHand(float dt) {
    live::Hand h;
    // The aimed square counts once the pointer rests on it (kAimDwell), for the piece in hand.
    int aim = live::kNoSquare;
    if (turn_ == Turn::HumanTouched && touchedSq_ != NoSquare) {
        if (aimDwellFor_ != touchedSq_) {
            aimDwell_.reset();
            aimDwellFor_ = touchedSq_;
        }
        aim = aimDwell_.update(aimLegal_ && aimSq_ != touchedSq_ ? int(aimSq_) : live::kNoSquare, dt);
    } else {
        aimDwellFor_ = NoSquare;
    }
    auto wire = [](Square s) { return s == NoSquare ? live::kNoSquare : int(s); };
    switch (turn_) {
    case Turn::HumanTouched:
        h.touch = wire(touchedSq_);
        h.aim = aim;
        break;
    case Turn::HumanPromotion:
        h.touch = wire(touchedSq_);
        h.aim = wire(promoTo_);
        h.promoting = true;
        break;
    case Turn::HumanPlacing:
    case Turn::HumanPlaced:
    case Turn::HumanPressing:
        // A staged move stands on the board until my press; a move already sent is the
        // opponent's MoveMade to play, my hand is idle again.
        if (moveStaged_) {
            h.touch = stagedMove_.from;
            h.aim = stagedMove_.to;
            h.placed = packed(stagedMove_);
        }
        break;
    default: break;
    }
    if (h.touch == live::kNoSquare) h = live::Hand();
    return h;
}

void GameScene::sendOnlineGesture(float dt) {
    live::Hand hand = onlineHand(dt);
    // While the game is played; once it is over, one last idle state, then nothing.
    bool over = !(state_ == State::Intro || state_ == State::Handshake || state_ == State::Playing) || endPending_ ||
                og_.status != StOngoing;
    if (over) {
        if (gestureFinal_) return;
        hand = live::Hand();
    }
    if (link_->reconnecting()) {
        // Nothing is kept for the reconnection; the first gesture after it goes at once (the
        // opponent's client waits for one sent after our return).
        gestureSentAny_ = false;
        return;
    }
    int seat = humanSeat();
    const Look& L = look_[seat];
    vec3 eye;
    quat q;
    firstPersonView(seat, eye, q);
    bool side = live::lookBesideBoard(eye, rotate(q, vec3(0, 0, -1)));
    net::Gesture g = live::buildGesture(hand, int(game_.moves().size()), L.gazeYaw, L.gazePitch, L.leanSmooth, glance_, side,
                                        gestureBuiltAny_ ? &gestureBuilt_ : nullptr);
    gestureBuilt_ = g;
    gestureBuiltAny_ = true;
    gestureSinceMs_ += double(dt) * 1000.0;
    if (over) {
        gestureFinal_ = true;
        if (gestureSentAny_ && live::sameHand(gestureSent_, g)) return;  // idle already
    } else if (gestureSentAny_ && !live::gestureDue(gestureSent_, g, gestureSinceMs_)) {
        return;
    }
    link_->sendGesture(g);
    gestureSent_ = g;
    gestureSentAny_ = true;
    gestureSinceMs_ = 0.0;
}

// =============================================================================================
// Live gestures: the opponent's robot
// =============================================================================================

bool GameScene::remoteMoveQueued(int ply) const {
    return std::any_of(remoteQueue_.begin(), remoteQueue_.end(), [ply](const RemoteMove& r) { return r.ply == ply; });
}

void GameScene::updateRemoteLive(float dt) {
    RemoteLive& L = remoteLive_;
    if (L.takeBack) return;
    // Their gestures stopped coming (the keepalive is once a second): the piece goes back (a move
    // put down is taken back), and their next gesture takes it again.
    if (L.pieceId >= 0 && live::holdExpired(remoteAge_)) {
        cancelRemoteLive();
        remoteFresh_ = false;
        return;
    }
    int r = aiSeat();
    Color remote = colorOfSeat(r);
    live::PieceGate gate;
    gate.plies = int(game_.moves().size());
    gate.remoteToMove = game_.position().sideToMove() == remote;
    gate.waiting = turn_ == Turn::RemoteWaiting;
    gate.moveQueued = remoteMoveQueued(gate.plies);
    gate.playing = state_ == State::Playing && og_.status == StOngoing && !endPending_;
    gate.resync = resync_;
    gate.fresh = remoteFresh_;
    if (!live::piecesApply(remoteGesture_, gate)) return;
    live::PieceIntent in = live::pieceIntent(remoteGesture_, game_.position(), remote);
    if (L.placed != 0) {
        // The move put down stays until its MoveMade, or until the gestures have shown something
        // else for a while (the move then never came: the board is set back).
        L.placedAway = in.placed == L.placed ? 0.0f : L.placedAway + dt;
        if (L.placedAway >= live::kPlacedTimeout) L.takeBack = true;
        return;
    }
    // One step at a time, from their latest gesture (live::handStep).
    live::HandStep step = live::handStep(L.pieceId >= 0 ? int(L.from) : live::kNoSquare, in, anim_[r].remainingTime());
    if (step.letGo) {
        // For another piece the hand goes straight on to it, or retracts while it is still in a
        // hand; with nothing to take it retracts.
        const PieceObject* next = step.take ? board_.byId(board_.idAt(Square(in.touch))) : nullptr;
        cancelRemoteLive(!next || pieceInHand(*next));
    }
    if (step.take) gripRemoteLive(Square(in.touch), gate.plies);
    if (L.pieceId < 0) return;
    if (step.place) placeRemoteLive(in.placed);
    else if (step.follow) followRemoteAim(in.aim, dt);
}

void GameScene::enqueueRemoteLive(const std::vector<anim::Task>& tasks) {
    anim::Animator& a = anim_[aiSeat()];
    a.enqueue(tasks);
    remoteLiveEnd_ = a.time() + a.remainingTime();
}

void GameScene::gripRemoteLive(Square from, int ply) {
    int id = board_.idAt(from);
    const PieceObject* p = board_.byId(id);
    // A piece still in a hand (being put back) is taken again once it is down.
    if (!p || pieceInHand(*p)) return;
    int r = aiSeat();
    anim_[r].setThinking(false);
    float reachAt = anim_[r].time() + anim_[r].remainingTime();  // after a piece going back, if any
    enqueueRemoteLive({task(anim::TaskType::Reach, id), task(anim::TaskType::Lift, id)});
    remoteLive_ = RemoteLive();
    remoteLive_.pieceId = id;
    remoteLive_.from = remoteLive_.hover = from;
    remoteLive_.ply = ply;
    remoteLive_.reachAt = reachAt;
    remoteAim_.reset();
}

void GameScene::followRemoteAim(int aim, float dt) {
    RemoteLive& L = remoteLive_;
    int r = aiSeat();
    // The piece goes over the square they aim at once the aim holds, back over its own square when
    // they aim nowhere for a while; one carry at a time.
    int target = remoteAim_.update(aim, dt);
    L.noAim = target == live::kNoSquare ? L.noAim + dt : 0.0f;
    Square want = target != live::kNoSquare ? Square(target) : L.noAim >= live::kAimLost ? L.from : L.hover;
    if (want == L.hover || !live::handReady(anim_[r].remainingTime())) return;
    const PieceObject* p = board_.byId(L.pieceId);
    if (!p) return;
    // Over a piece it would capture, the held piece stays clear of its top.
    const PieceObject* victim = want != L.from ? board_.at(want) : nullptr;
    float height = victim ? layout::PIECE_HEIGHT[victim->type] + 0.012f : 0.0f;
    vec3 pos = want == L.from ? p->basePos : board_.squareBase(want);
    enqueueRemoteLive({task(anim::TaskType::Carry, L.pieceId, pos, height)});
    L.hover = want;
}

void GameScene::placeRemoteLive(uint16_t move) {
    RemoteLive& L = remoteLive_;
    Move mv = unpack(game_.position(), move);
    if (!mv.valid() || board_.idAt(mv.from) != L.pieceId) return;
    // Their move stands on the board before their clock press: the placement, not the press, and
    // not in game_ (their MoveMade confirms it, startRemoteMove).
    std::vector<anim::Task> tasks;
    planMove(tasks, mv, game_.position().sideToMove(), true);
    tasks.push_back(task(anim::TaskType::Retract));
    enqueueRemoteLive(tasks);
    L.placed = move;
    L.hover = mv.to;
    L.placedAway = 0.0f;
    aiMoveTo_ = mv.to;
}

void GameScene::cancelRemoteLive(bool retract) {
    RemoteLive& L = remoteLive_;
    if (L.placed != 0) {
        L.takeBack = true;
        return;
    }
    if (L.pieceId < 0) return;
    int r = aiSeat();
    if (PieceObject* p = board_.byId(L.pieceId)) {
        // Back over its square first (Place comes straight down), then down on it.
        std::vector<anim::Task> tasks;
        if (L.hover != L.from) tasks.push_back(task(anim::TaskType::Carry, p->id, p->basePos));
        tasks.push_back(task(anim::TaskType::Place, p->id, p->basePos));
        if (retract) tasks.push_back(task(anim::TaskType::Retract));
        dest_[p->id].push_back({L.from, p->basePos, false});
        enqueueRemoteLive(tasks);
    }
    remoteLive_ = RemoteLive();
    remoteAim_.reset();
    if (retract && turn_ == Turn::RemoteWaiting) anim_[r].setThinking(true);
}

void GameScene::settleRemoteTakeBack() {
    // A move the opponent's robot put down and that never came: once the hands have let go, the
    // board is set back from the game (as a rebuild does, behind a short dip).
    if (!remoteLive_.takeBack || anim_[0].busy() || anim_[1].busy() || !dest_.empty()) return;
    board_.syncTo(game_.position());
    remoteLive_ = RemoteLive();
    remoteAim_.reset();
    fadeDip_ = std::max(fadeDip_, 0.4f);
    if (turn_ == Turn::RemoteWaiting) anim_[aiSeat()].setThinking(true);
    LOGI("online: the opponent's unconfirmed move is taken back");
}

bool GameScene::driveRemoteHead(float dt) {
    int r = aiSeat();
    const net::Gesture& g = remoteGesture_;
    bool following = (state_ == State::Intro || state_ == State::Handshake || state_ == State::Playing || state_ == State::GameOver) &&
                     link_ && live::headActive(remoteAge_, remoteFresh_, settings().ignoreOpponentHead, opponentAway_, link_->reconnecting());
    // Their clock stands at their right on their screen, not here: the robot's own look follows its
    // hand to this clock.
    bool active = following && !anim_[r].runningTask(anim::TaskType::PressClock);
    remoteGlancing_ = following && (g.flags & net::proto::GestureFlag::Glance) != 0;
    remoteGlanceBlend_ = clamp(remoteGlanceBlend_ + (remoteGlancing_ ? dt : -dt) / kGlanceTime, 0.0f, 1.0f);
    anim_[r].setLean(following ? g.lean : 0.0f);
    if (!active) {
        if (remoteHeadOn_) anim_[r].setHeadOverride(false);  // the gaze controller takes over smoothly
        remoteHeadOn_ = false;
        return false;
    }
    if (!remoteHeadOn_) {
        float yaw, pitch;
        anim_[r].headAngles(yaw, pitch);
        remoteHead_.snap(yaw, pitch);
        remoteHeadOn_ = true;
    }
    // What lies beside the board is mirrored between the two clients: a look there turns the
    // other way here.
    float yaw = (g.flags & net::proto::GestureFlag::Side) ? -g.yaw : g.yaw;
    float pitch = g.pitch;
    if (remoteGlanceBlend_ > 0.0f) {
        // Their scoresheet: this robot looks at its own pad here, as the local glance does.
        vec3 d = glanceTarget(r) - anim_[r].eyeCameraTransform().c[3].xyz();
        float zs = r == 0 ? 1.0f : -1.0f;  // the seat faces -Z * zs
        float b = smootherstep(remoteGlanceBlend_);
        yaw = lerp(yaw, std::atan2(-zs * d.x, -zs * d.z), b);
        pitch = lerp(pitch, std::atan2(d.y, length(vec2(d.x, d.z))), b);
    }
    remoteHead_.update(clamp(yaw, -kHeadYawLimit, kHeadYawLimit), clamp(pitch, kHeadPitchDown, kHeadPitchUp), dt);
    anim_[r].setHeadOverride(true, remoteHead_.yaw(), remoteHead_.pitch());
    return true;
}

// =============================================================================================
// Clock, end, overlay
// =============================================================================================

int64_t GameScene::onlineClockMs(int color) const {
    int64_t ms = color == 0 ? og_.whiteMs : og_.blackMs;
    if (og_.status == StOngoing && og_.running == color && link_) ms -= int64_t(link_->serverNowMs() - og_.serverTimeMs);
    return std::max<int64_t>(0, ms);
}

ClockDisplay GameScene::onlineClockDisplay() const {
    ClockDisplay d;
    int hw = world_.clockHalfForSeat(1.0f), hb = 1 - hw;
    int64_t ms[2] = {onlineClockMs(0), onlineClockMs(1)};
    int running = og_.status == StOngoing && state_ == State::Playing ? og_.running : 2;
    if (pendingPly_ >= 0 && clockFreeze_.holds(localMs(), link_->pingMs(), link_->reconnecting())) {
        // My move is on its way: my time stands still until the server has it (a confirmation
        // that takes too long, or a lost connection, shows the authority's running clock again).
        ms[int(humanColor_)] = clockFreeze_.shownMs;
        running = 2;
    }
    d.ms[hw] = ms[0];
    d.ms[hb] = ms[1];
    d.running = running == 0 ? hw : running == 1 ? hb : -1;
    d.unlimited = false;
    d.paused = false;
    d.leverSide = leverSide_;
    return d;
}

void GameScene::onlineResult() {
    int st = og_.status;
    resultText_ = st == StWhiteWins ? "1-0" : st == StBlackWins ? "0-1" : st == StDraw ? "\xC2\xBD-\xC2\xBD" : "\xE2\x80\x94";
    reasonText_ = reasonText(og_.reason);
    isDraw_ = st == StDraw;
    playerWon_ = (st == StWhiteWins && humanColor_ == White) || (st == StBlackWins && humanColor_ == Black);
    myDrawOffer_ = drawOffered_ = opponentAway_ = false;
}

ui::GameOverExtras GameScene::onlineGameOverExtras() const {
    ui::GameOverExtras x;
    bool direct = link_ && link_->kind() == LinkKind::Direct;
    if (og_.status == StAborted) x.line = i18n::tr("online.gameover.aborted");
    if (!direct && og_.status != StAborted) {
        if (!og_.rated) {
            x.detail = i18n::tr("online.rating.casual");
        } else if (ratingKnown_) {
            std::string delta = signedDelta(ratingAfter_ - ratingBefore_);
            x.detail = i18n::trf("online.rating.change", {std::to_string(ratingBefore_), std::to_string(ratingAfter_), i18n::ltr(delta)});
        } else {
            x.detail = i18n::tr("online.rating.pending");
        }
    }
    if (rematchOffered_ && !rematchGone_) {
        x.primaryLabel = i18n::tr("online.rematch.accept");
        x.detail = i18n::tr("online.rematch.offered");
    } else if (rematchAsked_) {
        x.primaryLabel = i18n::tr(rematchGone_ ? "online.rematch.gone" : "online.rematch.waiting");
        x.primaryDisabled = true;
    } else if (rematchGone_) {
        x.primaryLabel = i18n::tr("online.rematch.gone");
        x.primaryDisabled = true;
    }
    if (link_ && link_->canReport() && !reported_) x.reportLabel = i18n::tr("online.report.button");
    return x;
}

void GameScene::drawOnlineHud() {
    if (!link_) return;
    ui::OnlineHud hud;
    hud.pingMs = link_->pingMs();
    hud.reconnecting = link_->reconnecting();
    double now = link_->serverNowMs();
    bool playing = state_ == State::Playing && og_.status == StOngoing && !endPending_;
    if (playing && og_.moves.size() < 2 && game_.position().sideToMove() == humanColor_ && og_.firstMoveMs > 0 && pendingPly_ < 0) {
        double left = double(og_.firstMoveMs) - (now - og_.serverTimeMs);
        if (left > 0.0) hud.countdown = i18n::trf("online.first_move", {durationText(left)});
    }
    if (playing && opponentAway_) {
        hud.banner = i18n::trf("online.opponent_away", {durationText(std::max(0.0, opponentBackBy_ - now))});
    }
    hud.drawOffer = playing && drawOffered_ && !paused_;
    switch (ui::onlineHud(hud)) {
    case ui::OnlineHudAction::AcceptDraw:
        link_->answerDraw(true);
        drawOffered_ = false;
        break;
    case ui::OnlineHudAction::DeclineDraw:
        link_->answerDraw(false);
        drawOffered_ = false;
        break;
    default: break;
    }
}

// =============================================================================================
// Input and menus
// =============================================================================================

bool GameScene::updateReportDialog() {
    if (!reportOpen_) return false;
    int r = ui::reportDialog(reportCategory_, reportComment_);
    if (r == 1 && link_) {
        static const char* cats[] = {"cheating", "abuse", "other"};
        link_->report(seats_[aiSeat()].name, cats[std::clamp(reportCategory_, 0, 2)], reportComment_);
        reported_ = true;
        ui::notify(i18n::tr("online.report.sent"), 3.5f);
    }
    if (r >= 0) reportOpen_ = false;
    return true;
}

void GameScene::updateOnlineInput() {
    const plat::Input& in = plat::input();
    if (updateReportDialog()) return;
    if (!paused_ && in.keyPressed[plat::KEY_ESCAPE] && turn_ != Turn::HumanPromotion) {
        paused_ = true;
        if (dragging_) {
            dragging_ = false;
            plat::setMouseCaptured(false);
        }
    }
    if (paused_) {
        // The game goes on behind the menu: the server's clock does not stop.
        ui::OnlinePause p;
        p.canOfferDraw = !myDrawOffer_ && !drawOffered_ && og_.status == StOngoing;
        p.canClaimDraw = game_.canClaimThreefold() || game_.canClaimFiftyMove();
        p.canAbort = !myFirstMoveMade() && og_.status == StOngoing;
        p.canReport = link_ && link_->canReport() && !reported_;
        switch (menuChoice(ui::onlinePauseMenu(p))) {
        case ui::MenuAction::Resume: paused_ = false; break;
        case ui::MenuAction::OfferDraw:
            paused_ = false;
            link_->offerDraw();
            myDrawOffer_ = true;
            break;
        case ui::MenuAction::ClaimDraw:
            paused_ = false;
            link_->claimDraw();
            break;
        case ui::MenuAction::Abort:
            paused_ = false;
            link_->abortGame();
            break;
        case ui::MenuAction::Resign:
            paused_ = false;
            link_->resign();
            break;
        case ui::MenuAction::Report:
            paused_ = false;
            reportOpen_ = true;
            break;
        case ui::MenuAction::BackToMainMenu:
            paused_ = false;
            if (og_.status == StOngoing) leaveOngoingOnlineGame();
            leaveOnlineGame();
            break;
        case ui::MenuAction::OptionsChanged: applySettings(true); break;
        default: break;
        }
        return;
    }
    if (in.keyPressed[plat::KEY_TAB] && !ui::wantsKeyboard()) showMoveList_ = !showMoveList_;
    if (isHumanTurn() && !resync_ && !endPending_) updateHumanInput();
}

void GameScene::updateOnlineGameOver() {
    const plat::Input& in = plat::input();
    // A rematch accepted, or a challenge accepted at the table: the next game.
    if (onlineSession().gameReady()) {
        state_ = State::FadeToGame;
        stateTime_ = 0.0f;
        return;
    }
    if (updateReportDialog()) return;
    if (stateTime_ > 1.2f && (endHandshakeDone_ || stateTime_ > 5.0f)) {
        gameOverShown_ = true;
        ui::MenuAction a = ui::gameOver(resultText_, reasonText_, playerWon_, isDraw_, int(game_.moves().size() + 1) / 2,
                                        onlineGameOverExtras());
        if (a == ui::MenuAction::Rematch && link_ && !rematchAsked_ && !rematchGone_) {
            link_->rematch(true);
            rematchAsked_ = true;
        } else if (a == ui::MenuAction::BackToMainMenu) {
            if (rematchOffered_ && link_) link_->rematch(false);
            leaveOnlineGame();
            return;
        } else if (a == ui::MenuAction::Report) {
            reportOpen_ = true;
        }
    }
    if (in.keyPressed[plat::KEY_TAB] && !ui::wantsKeyboard()) showMoveList_ = !showMoveList_;
    // Challenges received between two games.
    if (link_ && link_->kind() == LinkKind::Server) ui::onlineChallenges();
}

void GameScene::leaveOngoingOnlineGame() {
    // Leaving resigns, or aborts before my first move (unrated), the same way from the menu and
    // when the window is closed.
    if (!myFirstMoveMade()) link_->abortGame();
    else link_->resign();
}

void GameScene::leaveOnlineGame() {
    // A direct match goes to the saved games first (nothing happens when its end saved it): its
    // authority has not answered a resignation or abort just sent, see saving::directMatchRecord.
    archiveGame(true);
    // The fade to the menu shows the clock as it stood (without link_, clock_ would show the
    // starting times).
    leaveClock_ = onlineClockDisplay();
    leaveClock_.running = -1;
    onlineSession().leaveGame();
    link_ = nullptr;
    clock_.stop();
    state_ = State::FadeToMenu;
    stateTime_ = 0.0f;
}

}  // namespace game
