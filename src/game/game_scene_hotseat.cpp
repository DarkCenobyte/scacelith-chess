// Hot-seat in the 3D scene: two people play on one PC, in turn, each from their own robot's eyes
// (docs/MULTIPLAYER_PLAN.md).
//   - The seat to move has the mouse and keyboard (inputSeat()) and the view (viewSeat()); every
//     rule of the game against Stockfish applies to it: touch-move, the arbiter's penalties when
//     the legal-move hints are off, the clock pressed by hand (none in an untimed game: the move is
//     completed as its last piece is released, clock_rules.h), the claims.
//   - Once the move is completed (completeMove) the view goes over to the other player: a camera
//     flight (CameraFlight::handoverShape) or, with Options > Gameplay > Handover on "Instant cut",
//     a cut through black. The clock is frozen meanwhile (nothing counts, the delay window waits),
//     drags are cancelled at the completion, and buttons still held from the previous turn are
//     ignored until released. During a flight the mover's head is its robot's again and the next player's head
//     turns to that player's own look (each seat keeps its yaw, pitch and lean).
//   - Scoresheets: each player writes their own sheet in their own hand. The mover records the
//     move at once; the next player records it once the view has reached them, unless they touch
//     a piece first: it is then written after their own move.
//   - Draw offers go with a move (FIDE 9.1.2): offered from the Esc menu, shown to the opponent as
//     a card when the view reaches them, declined by touching a piece. Resignation (Esc menu) is
//     the player to move's, confirmed with their name.
//   - A hot-seat game is friendly by default and never changes the rating against Stockfish; a
//     rated one updates both names' local ratings (Settings::localPlayers, elo::applyPair).
//   - The rematch swaps the colours; the clock follows its player (each keeps their hand).
#include "game_scene.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "../platform/platform.h"
#include "layout.h"
#include "settings.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace m;
using namespace chess;

namespace game {

namespace {

std::string signedDelta(int d) {
    return (d > 0 ? "+" : d < 0 ? "\xE2\x88\x92" : "\xC2\xB1") + std::to_string(std::abs(d));
}

}  // namespace

// =============================================================================================
// Seats
// =============================================================================================

int GameScene::inputSeat() const {
    return hotseat::inputSeat(hotSeat(), humanSeat(), seatOf(game_.position().sideToMove()));
}

int GameScene::viewSeat() const {
    if (!hotSeat()) return humanSeat();
    return handover_.active() ? handover_.viewSeat() : viewSeat_;
}

int GameScene::firstPersonSeat() const {
    if (watching()) return -1;
    if (!hotSeat()) return humanSeat();
    if (handover_.active())
        return handover_.phase() == hotseat::Handover::Phase::FadeOut ? handover_.fromSeat() : handover_.toSeat();
    return viewSeat_;
}

bool GameScene::anyInputHeld() const {
    const plat::Input& in = plat::input();
    for (int b = 0; b < plat::MOUSE_BUTTON_COUNT; ++b)
        if (in.mouseDown[b]) return true;
    return in.keyDown[plat::KEY_SPACE];
}

// =============================================================================================
// Setup
// =============================================================================================

void GameScene::initHotSeatArgs() {
    // The last two-player choices (the New Game page loads them as well when it opens).
    const Settings& s = settings();
    setup_.opponent = s.opponent;
    for (int i = 0; i < 2; ++i) {
        setup_.names[i] = s.hotseatNames[i];  // "" = the default (setupNewGame)
        setup_.hands[i] = s.hotseatHands[i];  // -1 = the default
    }
    setup_.clockRightOf = s.hotseatClockRightOf;
    setup_.rated = s.hotseatRated;
    const AppContext& ctx = *ctx_;
    if (ctx.hasArg("--handover")) handoverArg_ = std::clamp(float(std::atof(ctx.argValue("--handover").c_str())), 0.0f, 2.0f);
    std::string play = ctx.argValue("--play");
    script_.clear();
    size_t p = 0;
    while (p < play.size()) {
        size_t q = play.find(',', p);
        std::string m = play.substr(p, q == std::string::npos ? std::string::npos : q - p);
        if (!m.empty()) script_.push_back(m);
        if (q == std::string::npos) break;
        p = q + 1;
    }
    // --play-then resign|leave|takeback,...: the Esc menu's choices once those moves are made.
    std::string then = ctx.argValue("--play-then");
    scriptThen_.clear();
    for (size_t a = 0; a < then.size();) {
        size_t b = then.find(',', a);
        std::string m = then.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (!m.empty()) scriptThen_.push_back(m);
        if (b == std::string::npos) break;
        a = b + 1;
    }
    if (!ctx.hasArg("--hotseat")) return;
    // --start --hotseat [--white-name N] [--black-name N] [--clock-right white|black] [--rated]
    setup_.opponent = 1;
    if (ctx.hasArg("--white-name")) setup_.names[0] = ctx.argValue("--white-name");
    if (ctx.hasArg("--black-name")) setup_.names[1] = ctx.argValue("--black-name");
    std::string clock = ctx.argValue("--clock-right");
    if (clock == "white") setup_.clockRightOf = 0;
    if (clock == "black") setup_.clockRightOf = 1;
    if (ctx.hasArg("--rated")) setup_.rated = true;
}

void GameScene::configureHotSeatSeats() {
    const Settings& s = settings();
    for (int i = 0; i < 2; ++i) {
        Seat& st = seats_[i];
        character::Side hand = st.playHand;  // set by initAnimators()
        st = Seat();
        st.color = colorOfSeat(i);
        st.playHand = hand;
        st.controller = Controller::Human;
        st.name = hsPlayers_.names[i];
        if (hsPlayers_.rated) {
            // The local two-player rating of that name (a new name starts at 1500).
            const LocalPlayer* lp = s.findLocalPlayer(st.name);
            st.elo = lp ? lp->record.rating : elo::kInitialRating;
            st.provisional = !lp || lp->record.provisional();
        }
    }
}

void GameScene::swapHotSeatColours() {
    hotseat::Players p = hsPlayers_.swapped();
    Settings& s = settings();
    for (int i = 0; i < 2; ++i) {
        setup_.names[i] = p.names[i];
        setup_.hands[i] = p.hands[i];
        s.hotseatNames[i] = p.names[i];
        s.hotseatHands[i] = p.hands[i];
    }
    setup_.clockRightOf = s.hotseatClockRightOf = p.clockRightOf;
    s.save();
}

// =============================================================================================
// The handover
// =============================================================================================

void GameScene::startHandover(int mover) {
    int next = 1 - mover;
    // Whatever the previous player was doing with the mouse ends here.
    if (dragging_) {
        dragging_ = false;
        plat::setMouseCaptured(false);
    }
    hoverId_ = -1;
    inputGate_.arm();
    // A look at the scoresheet (S) ends with the turn. The flight leaves from the view as it is
    // (the sheet's narrower field of view widens during the flight); a cut lets the look end
    // while the view darkens.
    CameraPose from = firstPersonPose(mover);
    from.fovY = camera_.fovY;
    glance_ = false;
    float seconds = handoverArg_ >= 0.0f ? handoverArg_ : settings().handoverSeconds;
    if (seconds > 0.0f) {
        // A flight: the mover's head is its robot's again (the gaze controller takes over
        // smoothly), and the next player's head turns to their own look while the camera flies.
        anim_[mover].setHeadOverride(false);
        glanceBlend_ = 0.0f;  // before posing the next player's view
        beginLook(next, false);
    }
    handover_.start(mover, next, seconds, from, firstPersonPose(next), vec3(0.0f, layout::BOARD_TOP_Y, 0.0f));
    setClockFrozen(true);
    captionAge_ = 0.0f;
    LOGI("hot-seat: handover to %s (%s, %.2f s) at t = %.2f s", seats_[next].name.c_str(), seconds > 0.0f ? "flight" : "cut",
         handover_.totalTime(), time_);
}

void GameScene::updateHandover(float dt) {
    // The caption ("Bob, your move") fades in at the completion and stays up while the view goes over
    // (ui::hotSeatHud); updatePlaying() does not run meanwhile (the clock is frozen).
    if (!paused_) captionAge_ = handover_.active() ? std::min(captionAge_ + dt, 0.3f) : captionAge_ + dt;
    if (!handover_.active()) return;
    int to = handover_.toSeat();
    hotseat::Handover::Step st = handover_.update(dt, firstPersonPose(to));
    if (st.cut) {
        // At black: the view changes seats, the next player's look is theirs at once.
        anim_[handover_.fromSeat()].setHeadOverride(false);
        glanceBlend_ = 0.0f;
        beginLook(to, true);
        cameraCut_ = true;
    }
    if (st.landed) landHandover();
}

void GameScene::landHandover() {
    viewSeat_ = handover_.toSeat();
    LOGI("hot-seat: the view is in %s's eyes at t = %.2f s", seats_[viewSeat_].name.c_str(), time_);
    setClockFrozen(false);  // the next player's clock (and delay window) starts now
    writeGrace_ = kWriteGrace;
    scriptWait_ = kScriptThink;
}

void GameScene::beginLook(int seat, bool snap) {
    seat &= 1;
    Look& L = look_[seat];
    // A pointer the previous player left at the top of the window does not lift the new view.
    L.lookUpArmed = false;
    L.lookUpLift = 0.0f;
    if (snap) {
        L.gazeYaw = L.yaw;
        L.gazePitch = kBaseGazePitch + L.pitch;
        L.leanSmooth = L.lean;
    } else {
        // From where the head looks now (relative to the body: White faces -Z, Black +Z), so it
        // turns smoothly to the player's own look.
        CameraPose e = eyePose(seat);
        L.gazeYaw = clamp(angleDelta(seat == 0 ? 0.0f : PI, e.yaw), -kHeadYawLimit, kHeadYawLimit);
        L.gazePitch = clamp(e.pitch, kHeadPitchDown, kHeadPitchUp);
        L.leanSmooth = 0.0f;
    }
    L.headYaw = clamp(L.gazeYaw, -kHeadYawLimit, kHeadYawLimit);
    L.headPitch = clamp(L.gazePitch, kHeadPitchDown, kHeadPitchUp);
    anim_[seat].setHeadOverride(true, L.headYaw, L.headPitch);
}

// =============================================================================================
// The turn: writing, draw offers, scripted moves
// =============================================================================================

void GameScene::updateHotSeatTurn(float dt) {
    if (writeGrace_ > 0.0f) {
        writeGrace_ -= dt;
        if (turn_ != Turn::HumanIdle) {
            // A piece touched first: the opponent's move is recorded after this one (the hold is
            // released when this player's move is completed, completeMove).
            writeGrace_ = 0.0f;
        } else if (writeGrace_ <= 0.0f) {
            scorekeeper_.setHold(inputSeat(), false);  // records the opponent's move now
        }
    }
}

void GameScene::updateScript(float dt) {
    if (scriptWait_ > 0.0f) scriptWait_ -= dt;
    if (turn_ != Turn::HumanIdle || paused_ || scriptWait_ > 0.0f) return;
    if (scriptPos_ >= script_.size() && scriptThenPos_ >= scriptThen_.size()) return;
    if (hotSeat() && handover_.active()) return;
    if (anim_[inputSeat()].busy()) return;
    if (coach() && !coachMayTouch()) return;  // the coach has the floor (or its hands the table)
    if (scriptPos_ >= script_.size()) {
        // --play-then: the player opens the Esc menu and picks the next choice (menuChoice).
        const std::string& a = scriptThen_[scriptThenPos_++];
        ui::MenuAction m = a == "resign"     ? ui::MenuAction::Resign
                           : a == "leave"    ? ui::MenuAction::BackToMainMenu
                           : a == "takeback" ? ui::MenuAction::TakeBack
                                             : ui::MenuAction::None;
        if (m == ui::MenuAction::None) {
            LOGW("--play-then: '%s' is not resign, leave or takeback", a.c_str());
            return;
        }
        LOGI("--play-then: %s", a.c_str());
        paused_ = true;
        scriptMenu_ = m;
        scriptWait_ = kScriptThink;
        return;
    }
    const std::string& u = script_[scriptPos_++];
    Move mv = game_.position().parseUCI(u);
    PieceObject* p = mv.valid() ? board_.at(mv.from) : nullptr;
    if (!p) {
        LOGW("--play: '%s' is not legal here, the script stops", u.c_str());
        scriptPos_ = script_.size();
        return;
    }
    // By hand, as a player would: touch, carry, press the clock (queued until the piece is down;
    // untimed, there is no press: humanPressClock ignores it).
    humanTouch(p->id);
    if (turn_ != Turn::HumanTouched) return;
    scriptPromo_ = mv.promotion;
    humanPlace(mv.to);
    humanPressClock();
}

ui::MenuAction GameScene::menuChoice(ui::MenuAction shown) {
    if (scriptMenu_ == ui::MenuAction::None) return shown;
    ui::MenuAction m = scriptMenu_;
    scriptMenu_ = ui::MenuAction::None;
    return m;
}

void GameScene::offerDrawHotSeat() {
    int ply = int(game_.moves().size());
    if (drawOfferPly_ == ply || drawOfferBy_ >= 0) {
        ui::notify(i18n::tr("notify.draw_already_offered"), 2.5f);
        return;
    }
    int seat = inputSeat();
    drawOfferPly_ = ply;
    drawOfferBy_ = seat;
    // FIDE 9.1.2: make the move, offer, press the clock (untimed: the offer goes with the move made);
    // the opponent sees it when the view reaches them.
    ui::notify(i18n::trf("hotseat.draw.offer_noted", {seats_[1 - seat].name}), 4.0f);
}

void GameScene::answerHotSeatDraw(bool accept) {
    if (drawCardFor_ < 0) return;
    int answering = drawCardFor_, offering = 1 - answering;
    drawCardFor_ = -1;
    const std::string& a = seats_[answering].name;
    const std::string& o = seats_[offering].name;
    if (accept) {
        LOGI("hot-seat: %s accepts the draw offered by %s", a.c_str(), o.c_str());
        ui::notify(i18n::trf("hotseat.draw.accepted", {a, o}), 3.5f);
        game_.agreeDraw();
        endGame();
    } else {
        ui::notify(i18n::trf("hotseat.draw.declined", {a, o}), 3.0f);
    }
}

void GameScene::drawHotSeatHud() {
    ui::HotSeatHud h;
    for (int i = 0; i < 2; ++i) {
        h.names[i] = seats_[i].name;
        if (hsPlayers_.rated) h.ratings[i] = std::to_string(seats_[i].elo);
    }
    bool playing = state_ == State::Playing;
    h.toMove = playing ? seatOf(game_.position().sideToMove()) : -1;
    if (playing) {
        int next = handover_.active() ? handover_.toSeat() : inputSeat();
        h.caption = i18n::trf("hotseat.your_move", {seats_[next].name});
        h.captionAge = captionAge_;
    }
    h.drawOffer = playing && !paused_ && !handover_.active() && drawCardFor_ >= 0 && drawCardFor_ == inputSeat();
    if (drawCardFor_ >= 0) h.drawOfferText = i18n::trf("hotseat.draw.offered", {seats_[1 - drawCardFor_].name});
    switch (ui::hotSeatHud(h)) {
    case ui::HotSeatAction::AcceptDraw: answerHotSeatDraw(true); break;
    case ui::HotSeatAction::DeclineDraw: answerHotSeatDraw(false); break;
    default: break;
    }
}

// =============================================================================================
// Result and ratings
// =============================================================================================

void GameScene::rateHotSeat() {
    for (int i = 0; i < 2; ++i) hsEloBefore_[i] = hsEloAfter_[i] = seats_[i].elo;
    if (!hsPlayers_.rated) return;  // a friendly game
    const std::string& w = seats_[0].name;
    const std::string& b = seats_[1].name;
    // As in FIDE rating, a game counts once both players have made a move.
    if (game_.moves().size() < 2 || w == b) {
        LOGI("hot-seat: game not rated (%d plies%s)", int(game_.moves().size()), w == b ? ", same name twice" : "");
        return;
    }
    Settings& s = settings();
    s.localPlayer(w);
    s.localPlayer(b);  // both exist before taking pointers (the vector may grow)
    LocalPlayer* pw = s.findLocalPlayer(w);
    LocalPlayer* pb = s.findLocalPlayer(b);
    GameStatus st = game_.status();
    double score = st == GameStatus::Draw ? 0.5 : st == GameStatus::WhiteWins ? 1.0 : 0.0;
    elo::PairChange c = elo::applyPair(pw->record, pb->record, score);
    s.save();
    eloCounted_ = true;
    hsEloBefore_[0] = c.white.before;
    hsEloAfter_[0] = c.white.after;
    hsEloBefore_[1] = c.black.before;
    hsEloAfter_[1] = c.black.after;
    LOGI("hot-seat Elo: %s %d -> %d (%+d), %s %d -> %d (%+d)", w.c_str(), c.white.before, c.white.after, c.white.delta(), b.c_str(),
         c.black.before, c.black.after, c.black.delta());
}

ui::GameOverExtras GameScene::hotSeatGameOverExtras() const {
    ui::GameOverExtras x;
    int moves = int(game_.moves().size() + 1) / 2;
    GameStatus st = game_.status();
    if (st == GameStatus::WhiteWins || st == GameStatus::BlackWins) {
        const std::string& winner = seats_[st == GameStatus::WhiteWins ? 0 : 1].name;
        x.line = moves > 0 ? i18n::trn("hotseat.gameover.wins", moves, {winner, std::to_string(moves)})
                           : i18n::trf("hotseat.gameover.wins_now", {winner});
    }  // a draw keeps the card's own sentence
    if (eloCounted_) {
        auto change = [&](int i) {
            return i18n::trf("hotseat.elo.change", {seats_[i].name, std::to_string(hsEloBefore_[i]), std::to_string(hsEloAfter_[i]),
                                                    i18n::ltr(signedDelta(hsEloAfter_[i] - hsEloBefore_[i]))});
        };
        x.detail = change(0) + "  \xC2\xB7  " + change(1);
    } else if (hsPlayers_.rated) {
        x.detail = i18n::tr("hotseat.elo.unrated");
    } else {
        x.detail = i18n::trf("hotseat.gameover.friendly", {seats_[0].name, seats_[1].name});
    }
    return x;
}

}  // namespace game
