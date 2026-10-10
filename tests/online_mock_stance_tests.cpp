// The fake opponent's stances (src/game/online_mock.h, protocol minor 2), played against on the
// fakes' virtual clock (deterministic), the test being the local player's client. On its own it
// gets up now and then, while we think and during long thinks of its own, comes back at once when
// we move meanwhile, sends a stance other than Seated again every second, touches a piece only
// once its robot is seated again (2 s after its Seated, and a quarter of a second more), never
// glances at its clock or its scoresheet while it stands, and looks at the board from its standing
// eyes. Forced (forceOpponentStance, the developer key): held, its clock running and no move, back
// to its chair and playing once released; sent again after our reconnection; nothing once the
// game is over. The fake friend of a direct match (FakeDirect) does the same.
#include "test.h"
#include "anim/stance.h"
#include "chess/chess.h"
#include "game/layout.h"
#include "game/online_mock.h"
#include "net/gesture.h"
#include "net/protocol_gen.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <random>
#include <utility>
#include <vector>

using net::Event;
using Kind = net::Event::Kind;
namespace mock = net::mock;

namespace {

constexpr double kTick = 50.0;
// Its robot sits down 2 s after its Seated; its hand waits a quarter of a second more. Seen from
// here, both events arrive on the 50 ms ticks of the test.
constexpr double kSeatedBeforeHandMs = 2000.0 + 250.0 - 2.0 * kTick;
// A stance other than Seated comes again every second (the keepalive of the fakes' Welcome),
// on the fakes' ticks.
constexpr double kRefreshGapMaxMs = 1000.0 + 2.0 * kTick;

struct VirtualClock {
    bool was;
    VirtualClock() : was(mock::virtualClock()) { mock::useVirtualClock(true); }
    ~VirtualClock() { mock::useVirtualClock(was); }
};

// forceOpponentStance is a switch of all the fakes: given back to them whatever happens.
struct ForcedStance {
    explicit ForcedStance(int s) { mock::forceOpponentStance(s); }
    ~ForcedStance() { mock::forceOpponentStance(-1); }
};

// Where a look of the fake standing at 'stance' falls on the board's plane: its seat seen as
// White's (the frame of anim::stanceSpot with seatSign +1), the eyes 0.665 m above the pelvis and
// a little ahead, yaw and pitch relative to the body (net/gesture.h). False when it looks up.
bool standingLookOnBoard(int stance, const net::Gesture& g) {
    const anim::StanceSpot spot = anim::stanceSpot(anim::stanceFromCode(stance), 1.0f);
    const m::vec3 eye = spot.pelvis + m::vec3(0.0f, 0.665f, 0.0f) + spot.forward * 0.08f;
    const m::vec3 f = spot.forward, left(f.z, 0.0f, -f.x);
    const float c = std::cos(g.pitch);
    const m::vec3 d = (f * std::cos(g.yaw) + left * std::sin(g.yaw)) * c + m::vec3(0.0f, std::sin(g.pitch), 0.0f);
    if (d.y >= -0.05f) return false;
    const float t = (layout::BOARD_TOP_Y - eye.y) / d.y;
    const m::vec3 p = eye + d * t;
    const float half = layout::BOARD_SIZE * 0.5f + 0.03f;
    return std::fabs(p.x) <= half && std::fabs(p.z) <= half;
}

// What the local client sees of the games against the fake, the opponent's stance as the scene
// shows it (seated at the start of each game and after our link drops), and the counts of what
// the fake may and may not do.
struct Watch {
    bool loggedIn = false;
    uint64_t game = 0;
    int you = 0;
    chess::Game chess;
    bool over = false;
    net::OnlineGame snap;            // the last GameSnapshot
    int stance = 0;
    double stanceAt = -1e300;        // when the current stance arrived
    double seatedAt = -1e300;        // when its last Seated arrived (the start of a game counts as long ago)
    double lastStanceMsg = -1e300;   // when the last OpponentStance arrived (refreshes included)
    double comeBackFrom = -1;        // when our move arrived while it stood
    double welcomeAt = -1, snapshotAt = -1;
    std::vector<std::pair<double, int>> changes;   // (time, stance) of each change
    std::vector<double> theirMoveTimes;

    int stanceMsgs = 0, outings = 0, ourTurnOutings = 0, theirTurnOutings = 0, sideVisits = 0;
    int ourMoves = 0, theirMoves = 0, cameBack = 0, standingLooks = 0, touches = 0;
    double maxRefreshGap = 0;
    // What must never happen.
    int movedStanding = 0, movedTooSoon = 0, touchedStanding = 0, touchedTooSoon = 0, sideLookStanding = 0,
        lookOffBoard = 0, lateComeBack = 0, afterEnd = 0, otherGame = 0;

    int opp() const { return 1 - you; }
    bool ourTurn() const { return game != 0 && !over && int(chess.position().sideToMove()) == you; }
    size_t plies() const { return chess.moves().size(); }
    int bad() const {
        return movedStanding + movedTooSoon + touchedStanding + touchedTooSoon + sideLookStanding + lookOffBoard + lateComeBack +
               afterEnd + otherGame;
    }

    void replay(const net::OnlineGame& g) {
        chess.reset();
        for (const net::OnlineGame::MoveRec& r : g.moves) {
            const chess::Move mv = chess.position().findLegal(chess::Square(net::moveFrom(r.move)), chess::Square(net::moveTo(r.move)),
                                                              chess::PieceType(net::movePromo(r.move)));
            CHECK(mv.valid());
            if (mv.valid()) chess.play(mv);
        }
    }
    void on(const Event& e, double now) {
        switch (e.kind) {
        case Kind::LoginResult: loggedIn = e.ok; break;
        case Kind::Welcome: welcomeAt = now; break;
        case Kind::ConnectionChanged:
            if (e.state == net::ConnState::Reconnecting) {   // the scene shows it seated until told otherwise
                stance = 0;
                lastStanceMsg = -1e300;
            }
            break;
        case Kind::GameSnapshot:
            snap = e.game;
            snapshotAt = now;
            if (e.game.id != game) {
                game = e.game.id;
                you = e.game.you;
                stance = 0;
                stanceAt = seatedAt = lastStanceMsg = -1e300;
                comeBackFrom = -1;
            }
            over = e.game.status != int(net::proto::GameStatus::Ongoing);
            replay(e.game);
            break;
        case Kind::GameEnd:
            if (e.gameId == game) over = true;
            break;
        case Kind::MoveMade:
            if (e.gameId == game) moveMade(e, now);
            break;
        case Kind::OpponentGesture:
            if (e.gameId == game) gesture(e.gesture, now);
            break;
        case Kind::OpponentStance: stanceMsg(e, now); break;
        default: break;
        }
    }
    void moveMade(const Event& e, double now) {
        if (e.ply != int(plies())) return;   // one the snapshot brought already
        const chess::Move mv = chess.position().findLegal(chess::Square(net::moveFrom(e.move)), chess::Square(net::moveTo(e.move)),
                                                          chess::PieceType(net::movePromo(e.move)));
        CHECK(mv.valid());
        if (!mv.valid()) return;
        chess.play(mv);
        if (e.mine) {
            ++ourMoves;
            if (stance != 0) comeBackFrom = now;
            return;
        }
        ++theirMoves;
        theirMoveTimes.push_back(now);
        if (stance != 0) ++movedStanding;
        if (now - seatedAt < kSeatedBeforeHandMs) ++movedTooSoon;
    }
    void gesture(const net::Gesture& g, double now) {
        if (g.touch != net::Gesture::kNoSquare) {
            ++touches;
            if (stance != 0) ++touchedStanding;
            if (now - seatedAt < kSeatedBeforeHandMs) ++touchedTooSoon;
            return;
        }
        if (stance == 0) return;
        if (g.flags & (net::proto::GestureFlag::Glance | net::proto::GestureFlag::Side)) ++sideLookStanding;
        if (now - stanceAt < 1000.0) return;   // its head still turning from where it looked before
        ++standingLooks;
        if (!standingLookOnBoard(stance, g)) ++lookOffBoard;
    }
    void stanceMsg(const Event& e, double now) {
        if (e.gameId != game) {
            ++otherGame;
            return;
        }
        if (over) {
            ++afterEnd;
            return;
        }
        ++stanceMsgs;
        if (stance != 0 && lastStanceMsg > -1e299) maxRefreshGap = std::max(maxRefreshGap, now - lastStanceMsg);
        lastStanceMsg = now;
        if (e.stance == stance) return;
        changes.push_back({now, e.stance});
        if (comeBackFrom >= 0) {
            ++(now - comeBackFrom <= kTick ? cameBack : lateComeBack);
            comeBackFrom = -1;
        }
        if (e.stance == 0) {
            seatedAt = now;
        } else if (stance == 0) {
            ++outings;
            ++(ourTurn() ? ourTurnOutings : theirTurnOutings);
        }
        if (e.stance >= 2) ++sideVisits;
        stance = e.stance;
        stanceAt = now;
    }
};

// The local player's client against the fake server: events into the Watch every 50 ms of the
// virtual clock, and, when 'autoMove', a move (at random, from a seeded stream) after a think of
// 1.5 to 20 s; with 'autoChallenge', a new game (Bob_K, 10 min + 5 s) whenever none goes on.
struct Table {
    mock::FakeServer srv;
    Watch w;
    std::mt19937 rng{20261010u};
    bool autoMove = true, autoChallenge = false, challenged = false;
    size_t sentPly = size_t(-1), turnPly = size_t(-1);
    double turnFrom = 0, think = 0;
    std::function<void(Table&)> afterOurMove;

    Table() {
        net::ServerEndpoint ep;
        ep.host = "fake.example.org";
        srv.setServer(ep);
        srv.login("Paul_M", "correct horse battery");
        CHECK(runUntil([&] { return w.loggedIn; }, 5000.0));
        srv.connect();
        CHECK(runUntil([&] { return w.welcomeAt >= 0; }, 10000.0));
    }
    void challenge() {
        srv.challenge("Bob_K", 600, 5, false, 0);
        challenged = true;
    }
    void step() {
        const double now = mock::nowMs();
        Event e;
        while (srv.poll(e)) w.on(e, now);
        if (w.game != 0 && !w.over) challenged = false;
        if (autoChallenge && !challenged && (w.game == 0 || w.over)) challenge();
        if (autoMove && w.ourTurn() && w.plies() != sentPly) {
            if (turnPly != w.plies()) {
                static const double thinks[] = {1500.0, 4000.0, 9000.0, 20000.0};
                turnPly = w.plies();
                turnFrom = now;
                think = thinks[rng() % 4];
            }
            if (now - turnFrom >= think) {
                const std::vector<chess::Move> legal = w.chess.position().legalMoves();
                if (!legal.empty()) {
                    const chess::Move mv = legal[rng() % legal.size()];
                    srv.sendMove(w.game, int(w.plies()), net::packMove(int(mv.from), int(mv.to), int(mv.promotion)), w.chess.position().fen(),
                                 uint32_t(think), false);
                    sentPly = w.plies();
                    if (afterOurMove) afterOurMove(*this);
                }
            }
        }
        mock::advance(kTick);
    }
    template <class Pred>
    bool runUntil(Pred pred, double maxMs) {
        for (double t = 0; t < maxMs; t += kTick) {
            if (pred()) return true;
            step();
        }
        return pred();
    }
    void runFor(double ms) {
        for (double t = 0; t < ms; t += kTick) step();
    }
};

}  // namespace

// Forty minutes of games against the fake, which gets up on its own.
TEST(mock_stance_the_fake_gets_up_and_sits_down_in_time) {
    VirtualClock vc;
    ForcedStance automatic(-1);
    Table t;
    t.autoChallenge = true;
    t.runFor(40.0 * 60000.0);
    const Watch& w = t.w;
    CHECK(w.theirMoves >= 60);
    // It gets up both while we think and while it thinks, sometimes to an end of the table.
    CHECK(w.outings >= 6);
    CHECK(w.ourTurnOutings >= 2);
    CHECK(w.theirTurnOutings >= 2);
    CHECK(w.sideVisits >= 2);
    CHECK(w.cameBack >= 1);            // we moved while it stood: back at once
    CHECK(w.standingLooks >= 30);
    CHECK(w.touches >= 60);
    CHECK(w.stanceMsgs > w.outings * 3);   // refreshed while it stands
    CHECK(w.maxRefreshGap > 0.0 && w.maxRefreshGap <= kRefreshGapMaxMs);
    // And never anything a standing player cannot do.
    CHECK_EQ(w.movedStanding, 0);
    CHECK_EQ(w.movedTooSoon, 0);
    CHECK_EQ(w.touchedStanding, 0);
    CHECK_EQ(w.touchedTooSoon, 0);
    CHECK_EQ(w.sideLookStanding, 0);
    CHECK_EQ(w.lookOffBoard, 0);
    CHECK_EQ(w.lateComeBack, 0);
    CHECK_EQ(w.afterEnd, 0);
    CHECK_EQ(w.otherGame, 0);
    // Every game ended seated as far as the scene knows: nothing came after its end (above), and
    // each change is to a stance of this minor.
    for (const std::pair<double, int>& c : w.changes) CHECK(c.second >= 0 && c.second <= 3);
}

// Held by the developer switch: at an end of the table on its turn, its clock running and no
// move; released, it comes back through the front of its chair, sits down and plays 2 s later.
// Held again, it is sent again after our own reconnection, and silent once the game is over.
TEST(mock_stance_forced_holds_until_released) {
    VirtualClock vc;
    ForcedStance held(0);   // seated: no outing of its own meanwhile
    Table t;
    t.challenge();
    REQUIRE(t.runUntil([&] { return t.w.game != 0 && t.w.plies() >= 4 && t.w.ourTurn(); }, 120000.0));
    CHECK_EQ(t.w.outings, 0);

    // Our move, and at once the switch: its hand is still free, so it goes to its left.
    t.afterOurMove = [](Table&) { mock::forceOpponentStance(2); };
    REQUIRE(t.runUntil([&] { return t.w.stance == 2; }, 30000.0));
    t.afterOurMove = nullptr;
    const size_t plies = t.w.plies();
    CHECK(!t.w.ourTurn());
    const int movesBefore = t.w.theirMoves, touchesBefore = t.w.touches, msgsBefore = t.w.stanceMsgs;
    t.runFor(20000.0);
    CHECK_EQ(t.w.theirMoves, movesBefore);
    CHECK_EQ(t.w.touches, touchesBefore);
    CHECK_EQ(t.w.plies(), plies);
    CHECK(t.w.stanceMsgs - msgsBefore >= 18);   // its stance every second
    CHECK(t.w.maxRefreshGap <= kRefreshGapMaxMs);
    CHECK_EQ(t.w.stance, 2);
    t.srv.requestResync(t.w.game);
    t.runFor(200.0);
    CHECK_EQ(t.w.snap.running, t.w.opp());   // its clock runs
    CHECK_EQ(t.w.snap.moves.size(), plies);

    // Released: Standing at once (back to the front of its chair), Seated 2.5 s later, its move
    // after its robot sat down.
    const size_t changesBefore = t.w.changes.size();
    const double released = mock::nowMs();
    mock::forceOpponentStance(-1);
    REQUIRE(t.runUntil([&] { return t.w.theirMoves > movesBefore; }, 30000.0));
    REQUIRE(t.w.changes.size() >= changesBefore + 2);
    const std::pair<double, int> up = t.w.changes[changesBefore], down = t.w.changes[changesBefore + 1];
    CHECK_EQ(up.second, 1);
    CHECK(up.first - released <= 2.0 * kTick);
    CHECK_EQ(down.second, 0);
    CHECK(down.first - up.first >= 2500.0 - kTick && down.first - up.first <= 2500.0 + 2.0 * kTick);
    CHECK(t.w.theirMoveTimes.back() - down.first >= kSeatedBeforeHandMs);
    CHECK_EQ(t.w.stance, 0);

    // Held standing on our turn (we wait), then our link drops for a second: once back, the
    // stance comes again right after the snapshot, before any refresh would.
    t.autoMove = false;
    REQUIRE(t.runUntil([&] { return t.w.ourTurn(); }, 30000.0));
    mock::forceOpponentStance(1);
    REQUIRE(t.runUntil([&] { return t.w.stance == 1; }, 2000.0));
    const double welcomeBefore = t.w.welcomeAt;
    mock::connectionDrop(1);
    REQUIRE(t.runUntil([&] { return t.w.welcomeAt > welcomeBefore && t.w.stance == 1; }, 5000.0));
    CHECK(t.w.snapshotAt >= t.w.welcomeAt);
    CHECK(t.w.lastStanceMsg >= t.w.snapshotAt);
    CHECK(t.w.lastStanceMsg - t.w.welcomeAt <= 2.0 * kTick);

    // The game ends while it stands: nothing more from it for that game.
    t.srv.resign(t.w.game);
    REQUIRE(t.runUntil([&] { return t.w.over; }, 2000.0));
    const int msgsAtEnd = t.w.stanceMsgs;
    t.runFor(3000.0);
    CHECK_EQ(t.w.stanceMsgs, msgsAtEnd);
    CHECK_EQ(t.w.afterEnd, 0);
    // A new game while it is still held: it stands there from the start of that one.
    const uint64_t first = t.w.game;
    t.challenge();
    REQUIRE(t.runUntil([&] { return t.w.game != first && t.w.stance == 1; }, 10000.0));
    CHECK_EQ(t.w.bad(), 0);
}

// The fake friend of a direct match (FakeDirect) is the same fake: held at an end of the table,
// its stance comes for the direct game, again every second.
TEST(mock_stance_fake_direct_friend) {
    VirtualClock vc;
    mock::FakeDirect d;
    net::DirectHostOptions opt;
    opt.upnp = false;
    opt.playerName = "Paul";
    d.host(opt);
    Watch w;
    auto run = [&](double ms) {
        for (double t = 0; t < ms; t += kTick) {
            const double now = mock::nowMs();
            Event e;
            while (d.poll(e)) w.on(e, now);
            mock::advance(kTick);
        }
    };
    for (int i = 0; i < 200 && w.game == 0; ++i) run(kTick);
    REQUIRE(w.game != 0);
    ForcedStance held(3);
    run(5000.0);
    CHECK_EQ(w.stance, 3);
    CHECK(w.stanceMsgs >= 4);
    CHECK(w.maxRefreshGap <= kRefreshGapMaxMs);
    CHECK_EQ(w.otherGame, 0);
    mock::forceOpponentStance(-1);
    run(3000.0);   // back to the front of its chair, then down
    CHECK_EQ(w.stance, 0);
    CHECK_EQ(w.bad(), 0);
}
