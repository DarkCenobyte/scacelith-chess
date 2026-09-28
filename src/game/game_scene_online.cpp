// Online play in the 3D scene: the opponent is a player of the Scacelith server or of a direct
// match, and sits in the other chair as a robot (Controller::Remote).
//
// The authority (server or direct-match host) owns the game: its clocks, the legality of the
// moves and the result. The scene keeps the physical experience of a game against Stockfish:
//   - My move: the piece is touched and carried as usual, but it can only be released on a legal
//     square (no arbiter penalty online). The move is sent the moment its destination (and
//     promotion) is chosen, before the hand moves; the robot then places it and presses the
//     clock by itself (animation only). My clock display freezes until the server confirms.
//   - The opponent's move (MoveMade) is played by its robot like a Stockfish move but without
//     thinking time: touch, carry, capture, castling rook, promotion swap, clock. I may touch my
//     pieces as soon as its pieces are down.
//   - The clock shows the server's times (extrapolated with serverNowMs()), never flags locally
//     (it stops at 0.0 until the server's GameEnd).
//   - MoveRejected / a snapshot that disagrees: once the robots are idle the board, the game and
//     the scoresheets are rebuilt from the server's move list (a short fade hides the snap).
//     After a reconnection the moves missed are set up the same way, without animation.
//   - GameEnd waits until the last move is on the board, then the usual end (result on the
//     scoresheets, handshake, game over card with online reasons, rating change, rematch, report).
#include "game_scene.h"
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

namespace {

using Kind = net::Event::Kind;

// Protocol values (dedicated-server/src/protocol/schema.js).
enum Status { StOngoing = 0, StWhiteWins = 1, StBlackWins = 2, StDraw = 3, StAborted = 4 };
enum GameEventKind { EvDrawOffered = 1, EvDrawDeclined = 2, EvDisconnected = 3, EvReconnected = 4, EvRematchOffered = 5, EvRematchDeclined = 6 };
constexpr int kErrDrawOfferLimit = 108;

anim::Task makeTask(anim::TaskType t, int pieceId = -1, vec3 pos = vec3(0)) {
    anim::Task k;
    k.type = t;
    k.pieceId = pieceId;
    k.position = pos;
    return k;
}

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

std::string playerName() {
    const std::string& n = settings().playerName;
    return n.empty() || n == "Human" ? std::string(i18n::tr("player.default_name")) : n;
}

}  // namespace

// =============================================================================================
// Setup
// =============================================================================================

void GameScene::initOnline() {
    bool virtualClock = ctx_->screenshotMode || ctx_->hasArg("--warp");
    onlineSession().init(ctx_->hasArg("--online-mock"), virtualClock);
    if (ctx_->hasArg("--start-online")) {
        startOnline_ = ctx_->argValue("--start-online");
        if (startOnline_.empty() || startOnline_[0] == '-') startOnline_ = "5+3";
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
    turnStartMs_ = link_ ? link_->serverNowMs() : 0.0;
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
        st.name = p.name.empty() ? (st.human() ? playerName() : std::string("?")) : p.name;
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
    s.update(dt);
    if (!online() || !link_) return;
    if (state_ != State::Intro && state_ != State::Handshake && state_ != State::Playing && state_ != State::GameOver) return;
    net::Event e;
    while (s.nextGameEvent(e)) onlineEvent(e);
    if (state_ != State::Playing) return;
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
        if (!anim_[0].busy() && !anim_[1].busy() && dest_.empty()) rebuildOnline();
        return;
    }
    if (turn_ == Turn::RemoteWaiting && !remoteQueue_.empty()) startRemoteMove();
    // The opponent's pieces are down: my turn begins while its hand goes to the clock.
    if (turn_ == Turn::RemoteMoving && dest_.empty()) beginTurn();

    if (endPending_) {
        endWait_ += dt;
        bool settled = remoteQueue_.empty() && turn_ != Turn::RemoteMoving && turn_ != Turn::HumanPromotion && dest_.empty() &&
                       !anim_[0].busy() && !anim_[1].busy();
        if (turn_ == Turn::RemoteWaiting && !remoteQueue_.empty()) settled = false;
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
        turnStartMs_ = link_->serverNowMs();
        myDrawOffer_ = myDrawOffer_ && og_.drawOfferBy == int(humanColor_);
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
        ui::notify(serverErrorText(e.code), 4.0f);
        if (e.code == kErrDrawOfferLimit) myDrawOffer_ = false;
        break;
    default: break;
    }
}

void GameScene::onlineSnapshot(const net::OnlineGame& g) {
    og_ = g;
    if (g.status != StOngoing && state_ != State::GameOver) {
        endPending_ = true;
        endWait_ = 0.0f;
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
        }
        break;
    case EvReconnected:
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
    turnStartMs_ = link_ ? link_->serverNowMs() : 0.0;
    LOGI("online: board rebuilt from the server (%d moves)", n);
    if (state_ == State::Playing) beginTurn();
}

void GameScene::sendOnlineMove(const Move& mv) {
    int ply = int(game_.moves().size());
    uint16_t pm = packed(mv);
    double now = link_->serverNowMs();
    uint32_t thinkMs = uint32_t(std::max(0.0, now - turnStartMs_));
    frozenMs_ = onlineClockMs(int(humanColor_));
    link_->sendMove(ply, pm, game_.position().fen(), thinkMs, false);
    pendingPly_ = ply;
    pendingMove_ = pm;
    game_.play(mv);
    arbiter_.reset(game_);
    drawOffered_ = false;  // moving declines the opponent's offer
    LOGI("online: move %d %s sent (%u ms)", ply + 1, game_.sanMoves().back().c_str(), thinkMs);
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
    std::vector<anim::Task> tasks;
    tasks.push_back(makeTask(anim::TaskType::Reach, moverId));
    planPlacement(tasks, moverId, mv.to, victimId, rookFrom, rookTo);
    if (mv.promotion != NoPiece) planPromotionSwap(tasks, moverId, mv.to, mv.promotion);
    int half = world_.clockHalfForSeat(seat == 0 ? 1.0f : -1.0f);
    tasks.push_back(makeTask(anim::TaskType::PressClock, -1, world_.clockPressPoint(half)));
    tasks.push_back(makeTask(anim::TaskType::Retract));
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

bool GameScene::myFirstMoveMade() const { return int(og_.moves.size()) > int(humanColor_); }

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
    if (pendingPly_ >= 0) {
        // My move is on its way: my time stands still until the server has it.
        ms[int(humanColor_)] = frozenMs_;
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
            int d = ratingAfter_ - ratingBefore_;
            std::string delta = (d > 0 ? "+" : d < 0 ? "\xE2\x88\x92" : "\xC2\xB1") + std::to_string(std::abs(d));
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

void GameScene::updateOnlineInput() {
    const plat::Input& in = plat::input();
    if (reportOpen_) {
        int r = ui::reportDialog(reportCategory_, reportComment_);
        if (r == 1 && link_) {
            static const char* cats[] = {"cheating", "abuse", "other"};
            const Seat& opp = seats_[aiSeat()];
            link_->report(opp.name, cats[std::clamp(reportCategory_, 0, 2)], reportComment_);
            reported_ = true;
            ui::notify(i18n::tr("online.report.sent"), 3.5f);
        }
        if (r >= 0) reportOpen_ = false;
        return;
    }
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
        switch (ui::onlinePauseMenu(p)) {
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
            // Leaving resigns (or aborts, before my first move).
            if (og_.status == StOngoing) {
                if (p.canAbort) link_->abortGame();
                else link_->resign();
            }
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
    if (reportOpen_) {
        int r = ui::reportDialog(reportCategory_, reportComment_);
        if (r == 1 && link_) {
            static const char* cats[] = {"cheating", "abuse", "other"};
            link_->report(seats_[aiSeat()].name, cats[std::clamp(reportCategory_, 0, 2)], reportComment_);
            reported_ = true;
            ui::notify(i18n::tr("online.report.sent"), 3.5f);
        }
        if (r >= 0) reportOpen_ = false;
        return;
    }
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

void GameScene::leaveOnlineGame() {
    onlineSession().leaveGame();
    link_ = nullptr;
    clock_.stop();
    state_ = State::FadeToMenu;
    stateTime_ = 0.0f;
}

}  // namespace game
