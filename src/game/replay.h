// Replay of a saved game: when each move is played and what the clocks show. Engine-free (no GL, no
// Stockfish), unit-tested in tests/replay_tests.cpp. The scene's replay mode drives the robots and
// the observer camera with it: the clock says when the next move begins, the robots play it, the
// scene says when they are done (moveDone), and the next player starts "thinking".
//
// Pace. The time each player spent on a move comes from the record: [%emt] when present, else the
// difference of their [%clk] values (plus the increment of the TimeControl tag), else a natural
// default pace (a little quicker in the opening, never the same twice in a row). At x1 a move takes
// the time it took in the game, its animation included (Options::moveAnimationMs is part of it);
// a long think waits at most Options::thinkCapMs: the clock still shows the whole time spent, it
// runs faster meanwhile. x2, x4, x8 divide the waits; Instant waits not at all (each move begins as
// soon as the robots finished the previous one).
//
// Clocks. From [%clk] when present (or the TimeControl's base time minus the [%emt] times plus the
// increments): during a turn the mover's clock runs down from its value before the move to its
// value after it (as the scaled turn goes on), the other one stands still; the move done, both show
// the record's values (the increment appears at the press). Without any of that the clocks show
// dashes (ClockView::known false), as in an untimed game.
//
// Control. Pause/resume, the speed, one move forward (the next move is played now, animated),
// one move back and a jump to any ply (the board is set at once: SetPosition). Stepping pauses.
//
// Scene loop (GameScene, replay mode):
//     clock.update(dt);
//     replay::Event e;
//     while (clock.poll(e)) {
//         if (e.kind == replay::Event::Kind::Play) startRobotMove(e.ply);   // record.plies[e.ply]
//         else if (e.kind == replay::Event::Kind::SetPosition) rebuildBoard(e.ply);  // positions()[e.ply]
//         else showEndOfGame();
//     }
//     ... when the robot's move (and its clock press) is over: clock.moveDone();
//     ClockView v = clock.clocks();  -> ClockDisplay (dashes when !v.known)
#pragma once
#include "../chess/pgn.h"
#include <cstdint>
#include <string>
#include <vector>

namespace game {
namespace replay {

enum class Speed { X1, X2, X4, X8, Instant };
float speedFactor(Speed s);              // 1, 2, 4, 8; 0 for Instant
Speed faster(Speed s);                   // x1 -> x2 -> x4 -> x8 -> Instant (stays)
Speed slower(Speed s);                   // Instant -> x8 -> ... -> x1 (stays)

struct Options {
    int64_t thinkCapMs = 20000;          // the longest think waited at x1
    int64_t minWaitMs = 400;             // the shortest wait before a move at x1 (divided by the speed)
    int64_t defaultThinkMs = 2500;       // average time per move without times in the record
    int64_t moveAnimationMs = 1800;      // a robot's move, reach to clock press: part of the move's time
};

// What the record says of one ply (index = ply).
struct PlyTiming {
    chess::Color mover = chess::White;
    int64_t thinkMs = 0;                 // the time the mover spent on it
    bool measured = false;               // from [%emt] or [%clk]; false = the default pace
    int64_t clockBefore = -1;            // the mover's clock when the turn began, -1 = unknown
    int64_t clockAfter[2] = {-1, -1};    // both clocks once it is played (White, Black), -1 = unknown
};

class Timeline {
public:
    void build(const chess::pgn::Record& record, const Options& options = Options());
    int plies() const { return int(plies_.size()); }
    const PlyTiming& ply(int i) const { return plies_[size_t(i)]; }
    bool clocksKnown() const { return clocksKnown_; }
    int64_t startClock(chess::Color c) const { return start_[c]; }   // -1 = unknown
    // Real time to wait before ply i begins, at speed s (0 at Instant).
    int64_t waitMs(int i, Speed s) const;
    // Real time of ply i's whole turn at speed s: the wait plus the expected animation.
    int64_t turnMs(int i, Speed s) const;
    // The think time the clock shows for ply i (the whole of it, even when the wait is capped).
    int64_t shownThinkMs(int i) const { return plies_[size_t(i)].thinkMs; }
    // c's clock with 'ply' moves played (-1 = unknown).
    int64_t clockAfter(chess::Color c, int ply) const;
    const Options& options() const { return options_; }

private:
    std::vector<PlyTiming> plies_;
    Options options_;
    bool clocksKnown_ = false;
    int64_t start_[2] = {-1, -1};
};

struct Event {
    enum class Kind {
        Play,          // the robots play move 'ply' (from position ply to ply + 1), then moveDone()
        SetPosition,   // put the board at position 'ply' at once (no animation)
        Finished       // the last move is done: the end of the game (result card)
    };
    Kind kind = Kind::Play;
    int ply = 0;
};

// What the chess clock shows now.
struct ClockView {
    bool known = false;                  // false: dashes
    int64_t ms[2] = {0, 0};              // White, Black
    int running = -1;                    // 0 White, 1 Black, -1 none (paused, finished, a move being set)
};
// Both clocks as they stood with 'ply' moves played, nothing running (the Analysis mode, whose
// steps are no turns of the game).
ClockView clocksAt(const Timeline& timeline, int ply);

class ReplayClock {
public:
    // Starts at the start position, running at x1 (call pause() to start paused).
    void load(const chess::pgn::Record& record, const Options& options = Options());
    void update(float dtSeconds);        // advances the wait of the next move (not while paused)
    bool poll(Event& out);               // the next event, in order; false when none
    void moveDone();                     // the robots finished the move of the last Play event

    void pause();
    void resume();
    void togglePause() { paused_ ? resume() : pause(); }
    bool paused() const { return paused_; }
    void setSpeed(Speed s);
    Speed speed() const { return speed_; }
    void stepForward();                  // pauses; the next move is played now (Play)
    void stepBack();                     // pauses; the board goes one move back (SetPosition)
    void jumpTo(int ply);                // the board at position 'ply' (SetPosition), the pace goes on from there

    int ply() const { return ply_; }     // moves on the board (0 = the start position)
    int plies() const { return timeline_.plies(); }
    bool moving() const { return phase_ == Phase::Moving; }      // a Play event waits for moveDone()
    bool finished() const { return phase_ == Phase::Finished; }
    chess::Color toMove() const;         // the player whose turn it is (thinking or moving)
    float thinkProgress() const;         // 0..1 of the wait before the next move (gaze, hesitation)
    ClockView clocks() const;
    const Timeline& timeline() const { return timeline_; }

private:
    enum class Phase { Thinking, Moving, Finished };
    Timeline timeline_;
    chess::Color first_ = chess::White;  // side to move at the start position
    std::vector<Event> events_;
    size_t next_ = 0;                    // first unread event
    Phase phase_ = Phase::Finished;
    int ply_ = 0;
    bool paused_ = false;
    Speed speed_ = Speed::X1;
    float waitDone_ = 0.0f;              // fraction of the current wait elapsed
    double moveAge_ = 0.0;               // ms since the Play event (clock display)
    void push(Event::Kind kind, int ply);
    void play();
    void settle(int ply);                // the board at 'ply', thinking (or finished)
    int64_t clockOf(chess::Color c, int ply) const;   // c's clock with 'ply' moves played, -1 = unknown
};

// The i18n key of how a recorded game ended ("reason.checkmate"), "" when the record does not
// say: the ScacelithEnd tag of the game's own saves, else the final position (checkmate,
// stalemate, dead position), else a Termination "time forfeit".
std::string endReasonKey(const chess::pgn::Record& record);

// Move numbers of a record whose start position may come from a FEN ("40... Kd7"): the record's
// number of the move of the last of 'plies' plies played (the first move's for 0), and the rows
// those plies fill on a scoresheet numbered from 1, where a first move by Black goes in Black's
// column of the first row (0 for no ply).
int moveNumberAfter(const chess::Position& start, int plies);
int sheetRowsAfter(const chess::Position& start, int plies);

}  // namespace replay
}  // namespace game
