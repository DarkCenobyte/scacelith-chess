// Unit tests for src/game/replay: the pace of a replayed game (recorded times, clock differences,
// the default pace, the cap on long thinks, the speeds), its controls and the clocks it shows.
#include "test.h"
#include "game/replay.h"

#include <string>
#include <vector>

using namespace game;
namespace pgn = chess::pgn;

namespace {

pgn::Record record(const std::vector<const char*>& sans, const char* timeControl = "-") {
    pgn::Record r;
    r.setTag("TimeControl", timeControl);
    chess::Position pos = r.startPosition();
    for (const char* s : sans) {
        pgn::Ply p;
        p.move = pos.parseSAN(s);
        CHECK(p.move.valid());
        p.san = pos.toSAN(p.move);
        pos.makeMove(p.move);
        r.plies.push_back(p);
    }
    return r;
}

// Runs the clock for 'seconds' in small steps; returns the events seen, answering each Play with
// moveDone() at once when 'autoDone'.
std::vector<replay::Event> run(replay::ReplayClock& c, float seconds, bool autoDone = true, float step = 0.01f) {
    std::vector<replay::Event> out;
    for (float t = 0.0f; t < seconds - 1e-4f; t += step) {
        c.update(step);
        replay::Event e;
        while (c.poll(e)) {
            out.push_back(e);
            if (autoDone && e.kind == replay::Event::Kind::Play) c.moveDone();
        }
    }
    return out;
}

int count(const std::vector<replay::Event>& ev, replay::Event::Kind k) {
    int n = 0;
    for (const replay::Event& e : ev) n += e.kind == k;
    return n;
}

replay::Options noAnimation() {
    replay::Options o;
    o.moveAnimationMs = 0;
    o.minWaitMs = 0;
    return o;
}

}  // namespace

TEST(replay_timeline_from_emt_and_clocks) {
    // [%emt] gives the thinks; the clocks follow from the TimeControl's base and increment.
    pgn::Record r = record({"e4", "e5", "Nf3", "Nc6"}, "300+3");
    const int64_t emt[] = {2000, 5000, 61000, 0};
    for (int i = 0; i < 4; ++i) r.plies[size_t(i)].elapsedMs = emt[i];
    replay::Timeline t;
    t.build(r);
    CHECK(t.clocksKnown());
    CHECK_EQ(t.startClock(chess::White), int64_t(300000));
    CHECK_EQ(t.ply(0).thinkMs, int64_t(2000));
    CHECK(t.ply(0).measured);
    CHECK(t.ply(1).mover == chess::Black);
    CHECK_EQ(t.ply(0).clockAfter[chess::White], int64_t(301000));
    CHECK_EQ(t.ply(1).clockAfter[chess::Black], int64_t(298000));
    CHECK_EQ(t.ply(2).clockBefore, int64_t(301000));
    CHECK_EQ(t.ply(2).clockAfter[chess::White], int64_t(243000));
    CHECK_EQ(t.ply(2).clockAfter[chess::Black], int64_t(298000));
    // [%clk] alone (lichess): the thinks are the clock differences plus the increment.
    pgn::Record l = record({"d4", "d5", "c4", "e6"}, "180+2");
    const int64_t clk[] = {180000, 180000, 171000, 175500};
    for (int i = 0; i < 4; ++i) l.plies[size_t(i)].clockMs = clk[i];
    t.build(l);
    CHECK(t.clocksKnown());
    CHECK_EQ(t.ply(0).thinkMs, int64_t(2000));
    CHECK_EQ(t.ply(2).thinkMs, int64_t(11000));   // 180 - 171 + 2
    CHECK_EQ(t.ply(3).thinkMs, int64_t(6500));    // 180 - 175.5 + 2
    CHECK_EQ(t.ply(3).clockAfter[chess::White], int64_t(171000));
    // Clocks without a base time: each side starts from its first clock.
    l.setTag("TimeControl", "?");
    t.build(l);
    CHECK_EQ(t.startClock(chess::Black), int64_t(180000));
    CHECK_EQ(t.ply(2).thinkMs, int64_t(9000));    // no increment known
}

TEST(replay_default_pace) {
    pgn::Record r = record({"e4", "e5", "Nf3", "Nc6", "Bb5", "a6", "Ba4", "Nf6", "O-O", "Be7", "Re1", "b5", "Bb3", "d6"});
    replay::Timeline t;
    replay::Options o;
    t.build(r, o);
    CHECK(!t.clocksKnown());
    bool varied = false;
    for (int i = 0; i < t.plies(); ++i) {
        CHECK(!t.ply(i).measured);
        const int64_t ms = t.ply(i).thinkMs;
        const double scale = i < 10 ? 0.5 : 1.0;
        CHECK(ms >= int64_t(o.defaultThinkMs * 0.55 * scale) - 1);
        CHECK(ms <= int64_t(o.defaultThinkMs * 1.45 * scale) + 1);
        if (i > 0 && ms != t.ply(i - 1).thinkMs) varied = true;
    }
    CHECK(varied);
    // Deterministic: the same record gives the same pace.
    replay::Timeline again;
    again.build(r, o);
    for (int i = 0; i < t.plies(); ++i) CHECK_EQ(again.ply(i).thinkMs, t.ply(i).thinkMs);
}

TEST(replay_waits_speeds_and_cap) {
    pgn::Record r = record({"e4", "e5", "Nf3"}, "900+10");
    r.plies[0].elapsedMs = 5000;
    r.plies[1].elapsedMs = 180000;  // a three-minute think
    r.plies[2].elapsedMs = 300;
    replay::Timeline t;
    replay::Options o;  // cap 20 s, animation 1.8 s, minimum 0.4 s
    t.build(r, o);
    CHECK_EQ(t.waitMs(0, replay::Speed::X1), int64_t(3200));
    CHECK_EQ(t.waitMs(0, replay::Speed::X2), int64_t(700));
    CHECK_EQ(t.waitMs(0, replay::Speed::X8), int64_t(50));      // the minimum, scaled
    CHECK_EQ(t.waitMs(1, replay::Speed::X1), int64_t(18200));   // capped at 20 s
    CHECK_EQ(t.waitMs(1, replay::Speed::X4), int64_t(3200));
    CHECK_EQ(t.waitMs(2, replay::Speed::X1), int64_t(400));
    CHECK_EQ(t.waitMs(1, replay::Speed::Instant), int64_t(0));
    CHECK_EQ(t.turnMs(1, replay::Speed::X1), int64_t(20000));
    CHECK_EQ(t.shownThinkMs(1), int64_t(180000));                // the clock shows it all
    CHECK(replay::faster(replay::Speed::X8) == replay::Speed::Instant);
    CHECK(replay::faster(replay::Speed::Instant) == replay::Speed::Instant);
    CHECK(replay::slower(replay::Speed::X1) == replay::Speed::X1);
    CHECK(replay::slower(replay::Speed::X4) == replay::Speed::X2);
}

TEST(replay_clock_plays_moves_in_time) {
    pgn::Record r = record({"e4", "e5", "Nf3", "Nc6"});
    const int64_t emt[] = {1000, 2000, 3000, 500};
    for (int i = 0; i < 4; ++i) r.plies[size_t(i)].elapsedMs = emt[i];
    replay::ReplayClock c;
    c.load(r, noAnimation());
    replay::Event e;
    CHECK(c.poll(e));
    CHECK(e.kind == replay::Event::Kind::SetPosition);
    CHECK_EQ(e.ply, 0);
    CHECK(!c.poll(e));
    CHECK(c.toMove() == chess::White);
    // Nothing before 1 s, the first move at 1 s.
    std::vector<replay::Event> ev = run(c, 0.95f);
    CHECK(ev.empty());
    ev = run(c, 0.1f);
    CHECK_EQ(int(ev.size()), 1);
    if (!ev.empty()) {
        CHECK(ev[0].kind == replay::Event::Kind::Play);
        CHECK_EQ(ev[0].ply, 0);
    }
    CHECK_EQ(c.ply(), 1);
    CHECK(c.toMove() == chess::Black);
    // The robots' time: nothing happens until moveDone().
    ev = run(c, 1.95f, true);
    CHECK(ev.empty());
    ev = run(c, 0.1f, false);
    CHECK_EQ(count(ev, replay::Event::Kind::Play), 1);
    CHECK(c.moving());
    ev = run(c, 30.0f, false);
    CHECK(ev.empty());                 // still waiting for the robots
    c.moveDone();
    CHECK_EQ(c.ply(), 2);
    // The rest, then the end.
    ev = run(c, 4.0f);
    CHECK_EQ(count(ev, replay::Event::Kind::Play), 2);
    CHECK_EQ(count(ev, replay::Event::Kind::Finished), 1);
    CHECK(c.finished());
    CHECK_EQ(c.ply(), 4);
}

TEST(replay_pause_speed_and_steps) {
    pgn::Record r = record({"e4", "e5", "Nf3", "Nc6", "Bb5"});
    for (pgn::Ply& p : r.plies) p.elapsedMs = 4000;
    replay::ReplayClock c;
    c.load(r, noAnimation());
    replay::Event e;
    while (c.poll(e)) {
    }
    // Paused: nothing moves.
    c.pause();
    CHECK(run(c, 10.0f).empty());
    CHECK_EQ(c.thinkProgress(), 0.0f);
    c.resume();
    run(c, 2.0f);
    CHECK(c.thinkProgress() > 0.45f && c.thinkProgress() < 0.55f);
    // Faster: the half that is left takes 0.5 s at x4.
    c.setSpeed(replay::Speed::X4);
    std::vector<replay::Event> ev = run(c, 0.45f);
    CHECK(ev.empty());
    ev = run(c, 0.1f);
    CHECK_EQ(count(ev, replay::Event::Kind::Play), 1);
    CHECK_EQ(c.ply(), 1);
    // Step forward: paused, the next move now (animated).
    c.stepForward();
    CHECK(c.paused());
    CHECK(c.poll(e));
    CHECK(e.kind == replay::Event::Kind::Play);
    CHECK_EQ(e.ply, 1);
    // Step back during that move: the board before it.
    c.stepBack();
    CHECK(c.poll(e));
    CHECK(e.kind == replay::Event::Kind::SetPosition);
    CHECK_EQ(e.ply, 1);
    CHECK(!c.moving());
    c.moveDone();                      // a late answer for the cancelled move is ignored
    CHECK_EQ(c.ply(), 1);
    c.stepBack();
    CHECK(c.poll(e));
    CHECK_EQ(e.ply, 0);
    c.stepBack();                      // nothing before the start
    CHECK(!c.poll(e));
    // Jumps: to the end (and its result), back into the game.
    c.jumpTo(99);
    CHECK(c.poll(e));
    CHECK(e.kind == replay::Event::Kind::SetPosition);
    CHECK_EQ(e.ply, 5);
    CHECK(c.poll(e));
    CHECK(e.kind == replay::Event::Kind::Finished);
    CHECK(c.finished());
    c.stepBack();
    CHECK(c.poll(e));
    CHECK_EQ(e.ply, 4);
    CHECK(!c.finished());
    c.jumpTo(2);
    c.resume();
    c.setSpeed(replay::Speed::Instant);
    std::vector<replay::Event> rest = run(c, 0.05f);
    CHECK_EQ(count(rest, replay::Event::Kind::Play), 3);
    CHECK(c.finished());
}

TEST(replay_clock_display) {
    pgn::Record r = record({"e4", "e5", "Nf3"}, "300+5");
    const int64_t clk[] = {303000, 298000, 280000};  // White 2 s, Black 7 s, White 28 s (cap not reached)
    for (int i = 0; i < 3; ++i) r.plies[size_t(i)].clockMs = clk[i];
    replay::Options o;
    o.moveAnimationMs = 1000;
    o.minWaitMs = 0;
    replay::ReplayClock c;
    c.load(r, o);
    replay::ClockView v = c.clocks();
    CHECK(v.known);
    CHECK_EQ(v.ms[0], int64_t(300000));
    CHECK_EQ(v.ms[1], int64_t(300000));
    CHECK_EQ(v.running, 0);
    // White's 2 s turn: 1 s of wait, 1 s of animation. Halfway through the wait, 0.5 s used.
    run(c, 0.5f, false);
    v = c.clocks();
    CHECK(v.ms[0] <= 299500 + 20 && v.ms[0] >= 299500 - 20);
    CHECK_EQ(v.ms[1], int64_t(300000));
    run(c, 0.55f, false);              // the move began
    CHECK(c.moving());
    run(c, 3.0f, false);               // the robots are slow: the clock stops at the time the move took
    v = c.clocks();
    CHECK_EQ(v.ms[0], int64_t(298000));
    c.moveDone();                      // the press: the increment
    v = c.clocks();
    CHECK_EQ(v.ms[0], int64_t(303000));
    CHECK_EQ(v.running, 1);
    c.pause();
    CHECK_EQ(c.clocks().running, -1);
    c.jumpTo(3);
    v = c.clocks();
    CHECK_EQ(v.ms[0], int64_t(280000));
    CHECK_EQ(v.ms[1], int64_t(298000));
    CHECK_EQ(v.running, -1);
    // A long think: the wait is capped, the clock still loses the whole time.
    pgn::Record slow = record({"d4", "d5"}, "600+0");
    slow.plies[0].elapsedMs = 120000;
    slow.plies[1].elapsedMs = 1000;
    replay::ReplayClock s;
    s.load(slow, o);
    run(s, 18.9f, false);              // 18.9 s of 19 s of wait (cap 20 s minus the animation)
    CHECK(!s.moving());
    run(s, 0.2f, false);
    CHECK(s.moving());
    v = s.clocks();
    CHECK(v.ms[0] < 600000 - 110000);  // about 600 - 120 x 19/20
    // No clocks in the record: dashes.
    replay::ReplayClock u;
    u.load(record({"e4"}), o);
    CHECK(!u.clocks().known);
}

// The Analysis mode's clocks: both as they stood at a position, nothing running.
TEST(replay_clocks_at_a_position) {
    pgn::Record r = record({"e4", "e5", "Nf3"}, "300+5");
    const int64_t clk[] = {303000, 298000, 280000};
    for (int i = 0; i < 3; ++i) r.plies[size_t(i)].clockMs = clk[i];
    replay::Timeline t;
    t.build(r);
    replay::ClockView v = replay::clocksAt(t, 0);
    CHECK(v.known);
    CHECK_EQ(v.ms[0], int64_t(300000));
    CHECK_EQ(v.ms[1], int64_t(300000));
    CHECK_EQ(v.running, -1);
    v = replay::clocksAt(t, 2);
    CHECK_EQ(v.ms[0], int64_t(303000));
    CHECK_EQ(v.ms[1], int64_t(298000));
    v = replay::clocksAt(t, 3);
    CHECK_EQ(v.ms[0], int64_t(280000));
    CHECK_EQ(v.ms[1], int64_t(298000));
    CHECK_EQ(replay::clocksAt(t, 9).ms[0], int64_t(280000));   // past the end: the last values
    replay::Timeline u;
    u.build(record({"e4"}));
    CHECK(!replay::clocksAt(u, 1).known);
}

TEST(replay_end_reasons) {
    pgn::Record mate = record({"f3", "e5", "g4", "Qh4#"});
    mate.result = "0-1";
    CHECK_EQ(replay::endReasonKey(mate), std::string("reason.checkmate"));
    pgn::Record own = record({"e4", "e5"});
    own.result = "1-0";
    own.setTag("ScacelithEnd", "resignation");
    CHECK_EQ(replay::endReasonKey(own), std::string("reason.resignation"));
    own.setTag("ScacelithEnd", "../../etc");
    CHECK_EQ(replay::endReasonKey(own), std::string(""));
    own.eraseTag("ScacelithEnd");
    own.setTag("Termination", "Time forfeit");
    CHECK_EQ(replay::endReasonKey(own), std::string("reason.timeout"));
    own.result = "*";
    CHECK_EQ(replay::endReasonKey(own), std::string(""));
}

TEST(replay_move_numbers) {
    // The standard start: plies 1 and 2 are move 1, ply 3 is move 2.
    chess::Position std0;
    CHECK_EQ(replay::moveNumberAfter(std0, 0), 1);
    CHECK_EQ(replay::moveNumberAfter(std0, 1), 1);
    CHECK_EQ(replay::moveNumberAfter(std0, 2), 1);
    CHECK_EQ(replay::moveNumberAfter(std0, 3), 2);
    CHECK_EQ(replay::moveNumberAfter(std0, 31), 16);
    CHECK_EQ(replay::sheetRowsAfter(std0, 0), 0);
    CHECK_EQ(replay::sheetRowsAfter(std0, 1), 1);
    CHECK_EQ(replay::sheetRowsAfter(std0, 2), 1);
    CHECK_EQ(replay::sheetRowsAfter(std0, 3), 2);
    // From "40... Kd7" to "43. Ke3" (6 plies): moves 40 to 43, on rows 1 to 4 of the sheets.
    chess::Position fen;
    CHECK(fen.setFEN("4k3/8/8/8/8/8/4P3/4K3 b - - 0 40"));
    CHECK_EQ(replay::moveNumberAfter(fen, 0), 40);
    CHECK_EQ(replay::moveNumberAfter(fen, 1), 40);   // 40... Kd7
    CHECK_EQ(replay::moveNumberAfter(fen, 2), 41);   // 41. Kd2
    CHECK_EQ(replay::moveNumberAfter(fen, 3), 41);   // 41... Kd6
    CHECK_EQ(replay::moveNumberAfter(fen, 6), 43);   // 43. Ke3
    CHECK_EQ(replay::sheetRowsAfter(fen, 0), 0);
    CHECK_EQ(replay::sheetRowsAfter(fen, 1), 1);
    CHECK_EQ(replay::sheetRowsAfter(fen, 2), 2);
    CHECK_EQ(replay::sheetRowsAfter(fen, 5), 3);
    CHECK_EQ(replay::sheetRowsAfter(fen, 6), 4);
    // White to move at move 20.
    chess::Position w20;
    CHECK(w20.setFEN("4k3/8/8/8/8/8/4P3/4K3 w - - 0 20"));
    CHECK_EQ(replay::moveNumberAfter(w20, 1), 20);
    CHECK_EQ(replay::moveNumberAfter(w20, 3), 21);
    CHECK_EQ(replay::sheetRowsAfter(w20, 3), 2);
}
