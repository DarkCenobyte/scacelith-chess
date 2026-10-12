// Coach mode, the table through whole reviews: the session (src/coach/session.*) performs its
// scripts on a fake Stage whose table works as the game scene's (GameScene::runCoachTable,
// game_scene_coach.cpp): one job at a time; a demonstration move played from the table's position
// (the game and the demonstrations still standing), its victims and the coach's promoted pawns set
// down in the coach's half (coach::demoCaptureSlot), a promotion's new piece taken out of the
// reserve; a rewind taking the last demonstration moves back one ply at a time with
// coach::planRewind; a takeback taking the demonstrations still standing back first, then undoing
// the game and taking its moves back; the game's moves reset the table's position
// (GameScene::coachMoveCompleted). The players' moves are played on the table as the scene plays
// them (a victim beside its capturer, a promoted pawn beside its owner) and on a second board that
// only ever sees the game: whenever the coach may play, the table must stand exactly as that board
// (every piece, its square or its spot beside the board), show Game::position() square by square,
// hold beside the board exactly the game's own captures and promoted pawns, and no takeback card
// may be up. The reviews run to the end, with every answer to the card; cut short by Space, the
// pause menu's takeback, a resignation or a draw at sampled frames; twice in one game; and in the
// cases the offer policy leaves without a card (the level's takebacks used up among them).
#include "test.h"
#include "coach_fakes.h"

#include "chess/chess.h"
#include "coach/review.h"
#include "coach/rewind.h"
#include "coach/session.h"
#include "game/physical_board.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace coach;
using chess::Color;
using chess::Game;
using chess::Move;
using chess::Position;
using game::PhysicalBoard;
using game::PieceObject;

namespace {

ai::PvLine pvl(int cp, const char* pv, int mate = 0) {
    ai::PvLine l;
    l.depth = 16;
    l.score.cp = cp;
    l.score.mate = mate;
    std::istringstream in(pv);
    std::string m;
    while (in >> m) l.pv.push_back(m);
    return l;
}

ai::Analysis analysisOf(std::vector<ai::PvLine> lines) {
    ai::Analysis a;
    a.ok = true;
    a.depth = 16;
    for (size_t i = 0; i < lines.size(); ++i) lines[i].multipv = int(i + 1);
    a.lines = lines;
    if (!lines.empty() && !lines[0].pv.empty()) a.bestMove = lines[0].pv[0];
    return a;
}

float towards(Color c) { return c == chess::White ? 1.0f : -1.0f; }   // White sits at +Z

// A move of the game as the scene plays it on the table (GameScene::planMove, planPromotionSwap):
// the victim beside its capturer, a promoted pawn beside its owner, the new piece taken by takeSpare.
void playOnTable(PhysicalBoard& b, const Position& before, const Move& mv) {
    const Color side = before.sideToMove();
    const int moverId = b.idAt(mv.from);
    int victimId = b.idAt(mv.to);
    if (mv.flags & chess::MoveEnPassant) victimId = b.idAt(chess::Square(mv.to + (side == chess::White ? -8 : 8)));
    m::vec3 victimSlot, pawnSlot;
    int spareId = -1;
    if (victimId >= 0) victimSlot = b.nextCaptureSlot(chess::opposite(b.byId(victimId)->color), victimId);
    b.setOnSquare(moverId, mv.to);
    if (victimId >= 0) b.setCaptured(victimId, victimSlot);
    if (mv.flags & (chess::MoveCastleKing | chess::MoveCastleQueen)) {
        const int rank = chess::rankOf(mv.from);
        const bool king = (mv.flags & chess::MoveCastleKing) != 0;
        b.setOnSquare(b.idAt(chess::makeSquare(king ? 7 : 0, rank)), chess::makeSquare(king ? 5 : 3, rank));
    }
    if (mv.promotion != chess::NoPiece) {
        pawnSlot = b.nextCaptureSlot(side, moverId);
        spareId = b.takeSpare(mv.promotion, side);
        b.setCaptured(moverId, pawnSlot);
        b.setOnSquare(spareId, mv.to);
    }
    b.updateRestingTransforms();
}

// Takes one ply back by hand (planRewind's trips, carried out in order). False when the table does
// not show 'target' afterwards.
bool takeOnePlyBack(PhysicalBoard& b, const Position& target) {
    for (const PieceTrip& t : planRewind(b, target)) applyTrip(b, t);
    b.updateRestingTransforms();
    return tableMatches(b, target);
}

// Two tables stand alike: every piece of 'ref' at the same place on 'table' (square, or spot beside
// the board, captured or in the reserve), and the pieces only 'table' has (spares the arbiter
// brought for a demonstration) back in the reserve. Prints the first difference.
bool standsAs(const PhysicalBoard& table, const PhysicalBoard& ref) {
    const auto& t = table.pieces();
    const auto& r = ref.pieces();
    if (t.size() < r.size()) {
        std::fprintf(stderr, "  the table has %zu pieces, the game's board %zu\n", t.size(), r.size());
        return false;
    }
    for (size_t i = 0; i < t.size(); ++i) {
        const PieceObject& p = t[i];
        if (p.held) {
            std::fprintf(stderr, "  piece %d is still in a hand\n", p.id);
            return false;
        }
        if (i >= r.size()) {
            if (!p.inReserve || !p.spare) {
                std::fprintf(stderr, "  spare %d (type %d) is not back in the reserve\n", p.id, int(p.type));
                return false;
            }
            continue;
        }
        const PieceObject& q = r[i];
        const bool same = p.type == q.type && p.color == q.color && p.square == q.square && p.captured == q.captured &&
                          p.inReserve == q.inReserve && m::length(p.basePos - q.basePos) < 1e-5f;
        if (!same) {
            std::fprintf(stderr, "  piece %d (type %d, colour %d): table square %d captured %d reserve %d at (%.3f %.3f), "
                                 "game's board square %d captured %d reserve %d at (%.3f %.3f)\n",
                         p.id, int(p.type), int(p.color), int(p.square), int(p.captured), int(p.inReserve), p.basePos.x,
                         p.basePos.z, int(q.square), int(q.captured), int(q.inReserve), q.basePos.x, q.basePos.z);
            return false;
        }
        if (p.capturedBesideOwner) {
            std::fprintf(stderr, "  piece %d is still marked as a demonstration's victim\n", p.id);
            return false;
        }
    }
    return true;
}

// Pieces standing beside the board: [colour][type][half], half 0 = the piece's own half, 1 = the
// opponent's.
struct Beside {
    int n[2][7][2] = {};
};

Beside besideOf(const PhysicalBoard& b) {
    Beside s;
    for (const PieceObject& p : b.pieces())
        if (p.captured) ++s.n[p.color][p.type][towards(p.color) * p.basePos.z > 0.0f ? 0 : 1];
    return s;
}

// The pieces beside the board are exactly the game's own: those set aside for its start position
// ('start'), each victim of the game in its capturer's half, each promoted pawn in its owner's half,
// and nothing else (no demonstration's victim).
bool capturesMatchGame(const PhysicalBoard& b, const Game& g, const Beside& start) {
    int want[2][7][2] = {};
    std::memcpy(want, start.n, sizeof want);
    const Beside now = besideOf(b);
    const auto& have = now.n;
    for (size_t i = 0; i < g.moves().size(); ++i) {
        const Position& p = g.positionAt(i);
        const Move& mv = g.moves()[i];
        const Color side = p.sideToMove();
        if (mv.flags & chess::MoveEnPassant) ++want[chess::opposite(side)][chess::Pawn][1];
        else if (!p.at(mv.to).empty()) ++want[p.at(mv.to).color][p.at(mv.to).type][1];
        if (mv.promotion != chess::NoPiece) ++want[side][chess::Pawn][0];
    }
    for (int c = 0; c < 2; ++c)
        for (int t = 0; t < 7; ++t)
            for (int h = 0; h < 2; ++h)
                if (want[c][t][h] != have[c][t][h]) {
                    std::fprintf(stderr, "  beside the board: colour %d type %d in the %s half: %d, the game's: %d\n", c, t,
                                 h == 0 ? "owner's" : "capturer's", have[c][t][h], want[c][t][h]);
                    return false;
                }
    return true;
}

// The Stage of coach_fakes.h with the scene's table: demonstrations, rewinds and takebacks are jobs
// run one after the other on a PhysicalBoard (GameScene::runCoachTable).
class TableStage : public fake::Stage {
public:
    PhysicalBoard board;
    Color coach = chess::Black;
    int moveFrames = 8;        // frames a demonstration move or a ply taken back keeps a hand busy
    int fastFrames = 2;        // ... briskly (Space)

    // What went wrong on the table.
    int illegalDemos = 0;      // a demonstration move not legal on the table's position
    int mismatches = 0;        // a ply taken back without the table showing the position before it
    int movesOverDemo = 0;     // a game move made while demonstration moves stood on the table
    int demoMoves = 0, pliesBack = 0, gameUndos = 0;

    struct Job {
        enum class Kind { Demo, Rewind, TakeBack } kind = Kind::Demo;
        std::string uci;
        int plies = 0;
        bool fast = false;
    };
    std::deque<Job> jobs;
    bool running = false;
    Job job;
    int left = 0;
    struct Ply {
        Position target;
        bool demo = false;
    };
    std::vector<Ply> plies;                // still to take back in this job (next = back())
    Position demoPos;                      // the table's position: the game and the demonstrations
    std::vector<Position> demoBefore;      // before each demonstration move still on the table
    // The game's board follows a takeback: the scene undoes the game when the job starts.
    std::function<void(int)> onUndo;

    void setUp(const Game& g, Color coachColor) {
        board.reset(true);
        board.syncTo(g.position());
        coach = coachColor;
        demoPos = g.position();
        demoBefore.clear();
    }
    // GameScene::coachMoveCompleted: a move of the game resets the table's position.
    void gameMoved(const Game& g) {
        if (!demoBefore.empty()) {
            ++movesOverDemo;
            std::fprintf(stderr, "  a game move while %zu demonstration moves stand on the table\n", demoBefore.size());
        }
        demoPos = g.position();
        demoBefore.clear();
    }

    // ---- coach::Stage --------------------------------------------------------------------------------
    void demoMove(const std::string& uci, float pause) override {
        log("demoMove", uci).a = pause;
        Job j;
        j.kind = Job::Kind::Demo;
        j.uci = uci;
        jobs.push_back(j);
    }
    void rewindDemo(int n, bool fast) override {
        Ev& e = log("rewindDemo");
        e.n = n;
        e.flag = fast;
        if (fast) hurryTable();
        if (n <= 0) return;
        Job j;
        j.kind = Job::Kind::Rewind;
        j.plies = n;
        j.fast = fast;
        jobs.push_back(j);
    }
    void hurryTable() override {
        log("hurryTable");
        if (running && (job.kind == Job::Kind::Rewind || job.kind == Job::Kind::TakeBack)) job.fast = true;
        for (Job& j : jobs)
            if (j.kind == Job::Kind::Rewind) j.fast = true;
    }
    void takeBack(int n) override {
        log("takeBack").n = n;
        if (n <= 0) return;
        Job j;
        j.kind = Job::Kind::TakeBack;
        j.plies = n;
        jobs.push_back(j);
    }
    bool tableBusy() const override { return running || !jobs.empty(); }

    // ---- Every frame (after the session's update, as runCoachTable after Session::update) ----------
    void tick() {
        if (!running) {
            if (jobs.empty()) return;
            job = jobs.front();
            jobs.pop_front();
            running = true;
            plies.clear();
            if (demoBefore.empty()) demoPos = game->position();
            if (!startJob()) {
                running = false;
                return;
            }
        }
        if (--left > 0) return;
        if (job.kind == Job::Kind::Demo) {
            running = false;
            return;
        }
        // A ply is taken back: check the table, then the next one.
        const Ply done = plies.back();
        plies.pop_back();
        ++pliesBack;
        if (!takeOnePlyBack(board, done.target)) {
            ++mismatches;
            std::fprintf(stderr, "  the table does not show %s after a ply taken back\n", done.target.fen().c_str());
            board.syncTo(done.target);
        }
        if (done.demo && !demoBefore.empty()) {
            demoPos = demoBefore.back();
            demoBefore.pop_back();
        }
        if (plies.empty()) {
            if (job.kind == Job::Kind::TakeBack) demoPos = game->position();
            running = false;
            return;
        }
        left = job.fast ? fastFrames : moveFrames;
    }

private:
    bool startJob() {
        if (job.kind == Job::Kind::Demo) {
            const Move mv = demoPos.parseUCI(job.uci);
            if (!mv.valid()) {
                ++illegalDemos;
                std::fprintf(stderr, "  demonstration move %s is not legal on the table (%s)\n", job.uci.c_str(),
                             demoPos.fen().c_str());
                return false;
            }
            // As the scene plays it: every piece leaving the board goes to the coach's half.
            const Color side = demoPos.sideToMove();
            const int moverId = board.idAt(mv.from);
            int victimId = board.idAt(mv.to);
            if (mv.flags & chess::MoveEnPassant) victimId = board.idAt(chess::Square(mv.to + (side == chess::White ? -8 : 8)));
            m::vec3 victimSlot, pawnSlot;
            int spareId = -1;
            if (victimId >= 0) victimSlot = demoCaptureSlot(board, victimId, coach);
            if (mv.promotion != chess::NoPiece) {
                pawnSlot = board.nextCaptureSlot(coach, moverId);
                spareId = board.takeSpare(mv.promotion, side);
            }
            board.setOnSquare(moverId, mv.to);
            if (victimId >= 0) board.setCaptured(victimId, victimSlot);
            if (mv.flags & (chess::MoveCastleKing | chess::MoveCastleQueen)) {
                const int rank = chess::rankOf(mv.from);
                const bool king = (mv.flags & chess::MoveCastleKing) != 0;
                board.setOnSquare(board.idAt(chess::makeSquare(king ? 7 : 0, rank)), chess::makeSquare(king ? 5 : 3, rank));
            }
            if (mv.promotion != chess::NoPiece) {
                board.setCaptured(moverId, pawnSlot);
                board.setOnSquare(spareId, mv.to);
            }
            board.updateRestingTransforms();
            demoBefore.push_back(demoPos);
            demoPos.makeMove(mv);
            ++demoMoves;
            left = moveFrames;
            return true;
        }
        // The plies to take back, last first: the demonstrations still on the table, then (a
        // takeback) the game's moves.
        for (size_t i = demoBefore.size(); i-- > 0;) {
            if (job.kind == Job::Kind::Rewind && int(demoBefore.size() - i) > job.plies) break;
            plies.insert(plies.begin(), Ply{demoBefore[i], true});
        }
        if (job.kind == Job::Kind::TakeBack) {
            const int n = int(game->moves().size());
            const int k = std::min(job.plies, n);
            if (k <= 0) return false;
            std::vector<Ply> order;
            for (int j = n - k; j < n; ++j) order.push_back(Ply{game->positionAt(size_t(j)), false});
            for (const Ply& p : plies) order.push_back(p);
            plies = order;
            game->undo(k);
            ++gameUndos;
            if (onUndo) onUndo(k);
        }
        if (plies.empty()) return false;
        left = job.fast ? fastFrames : moveFrames;
        return true;
    }
};

// A coach game on that table: the session, the two boards, the players' moves.
struct Scene {
    TableStage stage;
    fake::Analyst analyst;
    Session session;
    Game game;
    PhysicalBoard ref;              // the table as the game's moves alone leave it
    std::vector<Position> refBefore;   // the game's position before each move on 'ref'
    Beside startBeside;             // set aside for the start position (a FEN's missing pieces)
    Color human = chess::White;
    float dt = 1.0f / 60.0f;
    std::vector<Line> lines;
    std::vector<std::string> demoKeys;  // the review scripts' DemoMove ucis, in order
    int offersShown = 0;            // takeback cards opened
    int offerTableFaults = 0;       // a card opened while the table did not show the game
    int holdFaults = 0;             // frames the coach was let play over a card or a demonstration
    bool offerWasOpen = false;

    Scene() {
        stage.game = &game;
        stage.onUndo = [this](int k) {
            for (int i = 0; i < k && !refBefore.empty(); ++i) {
                CHECK(takeOnePlyBack(ref, refBefore.back()));
                refBefore.pop_back();
            }
        };
        session.director().setObserver([this](const Beat& b) {
            if (!b.line.key.empty()) lines.push_back(b.line);
        });
    }
    void start(const char* fen, int level, Color humanColor, bool offers = true) {
        if (fen) CHECK(game.resetFromFEN(fen));
        human = humanColor;
        stage.setUp(game, chess::opposite(human));
        ref.reset(true);
        ref.syncTo(game.position());
        startBeside = besideOf(ref);
        refBefore.clear();
        SessionConfig c;
        c.level = level;
        c.human = humanColor;
        c.offersEnabled = offers;
        session.start(stage, analyst, game, c);
    }
    void step() {
        stage.advance(dt);
        session.update(game, dt);
        stage.tick();
        const bool open = session.offerOpen();
        if (open && !offerWasOpen) {
            ++offersShown;
            // The card comes once the table is put back: it shows the game, nothing of the line.
            if (stage.tableBusy() || !stage.demoBefore.empty() || !tableMatches(stage.board, game.position()) ||
                !standsAs(stage.board, ref)) {
                ++offerTableFaults;
                std::fprintf(stderr, "  the takeback card opened over a table that does not show the game\n");
            }
        }
        offerWasOpen = open;
        // The coach's reply waits for the card's answer, and never stands over a demonstration
        // (between two of its moves the table has no job: the scene's own check sees nothing).
        const bool coachTurn = !game.isOver() && game.position().sideToMove() != human;
        if (coachTurn && session.coachMayMove() && (open || (!stage.tableBusy() && !stage.demoBefore.empty()))) {
            if (holdFaults == 0)
                std::fprintf(stderr, "  the coach may play while %s\n", open ? "the takeback card is up" : "a demonstration stands");
            ++holdFaults;
        }
    }
    bool until(const std::function<bool()>& cond, float seconds) {
        for (float t = 0.0f; t < seconds; t += dt) {
            if (cond()) return true;
            step();
        }
        return cond();
    }
    void run(float seconds) {
        for (float t = 0.0f; t < seconds; t += dt) step();
    }
    bool quiet(float seconds = 120.0f) {
        return until([&] { return session.director().idle() && !stage.bodyBusy() && !stage.tableBusy(); }, seconds);
    }
    // The scene's coachMayTouch(): no table job, no card, the session's word.
    bool humanMayMove() const {
        return !stage.tableBusy() && !session.offerOpen() && session.playerMayMove(game);
    }
    // The scene's coachHoldsMove() (its hand at rest aside).
    bool coachHolds() const { return !session.coachMayMove() || stage.tableBusy() || stage.bodyBusy(); }

    // The table stands as the game alone would leave it.
    bool tableShowsGame() {
        const bool ok = stage.demoBefore.empty() && tableMatches(stage.board, game.position()) &&
                        standsAs(stage.board, ref) && capturesMatchGame(stage.board, game, startBeside);
        if (!ok) std::fprintf(stderr, "  table check failed at ply %zu (%s)\n", game.moves().size(), game.position().fen().c_str());
        return ok;
    }

    void play(const char* uci) {
        const Move mv = game.position().parseUCI(uci);
        CHECK(mv.valid());
        if (!mv.valid()) {
            std::fprintf(stderr, "  %s is not legal in %s\n", uci, game.position().fen().c_str());
            return;
        }
        const Position before = game.position();
        playOnTable(stage.board, before, mv);
        playOnTable(ref, before, mv);
        refBefore.push_back(before);
        game.play(mv);
        stage.gameMoved(game);
        session.onMove(game);
        if (game.isOver()) session.onGameOver(game, false);
    }
    // The human's move, once the scene lets them touch a piece.
    bool humanMove(const char* uci) {
        if (!until([&] { return humanMayMove(); }, 120.0f)) {
            std::fprintf(stderr, "  the human may not play %s\n", uci);
            return false;
        }
        CHECK(game.position().sideToMove() == human);
        play(uci);
        return true;
    }
    // The coach's reply, played as soon as the scene lets it: the table must show the game then.
    bool coachReply(const char* uci) {
        if (!until([&] { return !coachHolds(); }, 120.0f)) {
            std::fprintf(stderr, "  the coach may not play %s\n", uci);
            return false;
        }
        CHECK(!session.offerOpen());
        CHECK(tableShowsGame());
        CHECK(game.position().sideToMove() != human);
        play(uci);
        return true;
    }
    int count(const std::string& kind) const { return stage.count(kind); }
    bool queued(const std::string& key) const {
        for (const Line& l : lines)
            if (l.key == key) return true;
        return false;
    }
    int queuedCount(const std::string& prefix) const {
        int n = 0;
        for (const Line& l : lines) n += l.key.rfind(prefix, 0) == 0 ? 1 : 0;
        return n;
    }
    // Every fault the table counted is zero.
    void checkTable() {
        CHECK_EQ(stage.illegalDemos, 0);
        CHECK_EQ(stage.mismatches, 0);
        CHECK_EQ(stage.movesOverDemo, 0);
        CHECK_EQ(offerTableFaults, 0);
        CHECK_EQ(holdFaults, 0);
    }
};

// Plays the moves (human and coach in turn, from whichever side is to move) with every review left
// to run, the coach's replies checked against the table.
bool playLine(Scene& s, const std::vector<const char*>& ucis) {
    for (const char* u : ucis) {
        const bool humanTurn = s.game.position().sideToMove() == s.human;
        if (!(humanTurn ? s.humanMove(u) : s.coachReply(u))) return false;
    }
    return true;
}

// A blunder of the human's whose review shows a line on the table and offers the move back.
struct Blunder {
    const char* name;
    const char* fen;                     // nullptr: the standard start
    std::vector<const char*> setup;      // played first (human and coach in turn), every move reviewed
    const char* played;                  // the blunder (uci)
    std::vector<ai::PvLine> a0;          // A0 of the position before it; lines[0] is the better move
    const char* reply;                   // the coach's reply to the blunder
    const char* afterBetter;             // ... and to the better move
    int fromLevel, toLevel;              // levels at which the review demonstrates (and offers)
};

std::vector<Blunder> blunders() {
    return {
        // 1.Nd5?? exd5: one capture (levels 1-3; from level 4 the capture is only said).
        {"hanging knight", "6k1/5ppp/4p3/8/8/2N5/5PPP/6K1 w - - 0 1", {}, "c3d5",
         {pvl(20, "g1f1 g8f8"), pvl(-330, "c3d5 e6d5 g1f1")}, "e6d5", "g8f8", 1, 3},
        // 1.d4?? exd3 e.p. forks the rook and the knight: 2.Rc1 dxe2 (from level 4: the fork only).
        {"en passant fork", "6k1/4rppp/8/8/4p3/8/2RPN1PP/6K1 w - - 0 1", {}, "d2d4",
         {pvl(0, "g1f1 g8f8"), pvl(-380, "d2d4 e4d3 c2c1 d3e2")}, "e4d3", "g8f8", 1, 6},
        // 1.Rd5?? bxc1=Q+ (a capture that promotes, with check) 2.Rd1 Qxd1# (from level 4: up to
        // the mate).
        {"capture that promotes", "6k1/5ppp/8/8/8/8/1p3PPP/2BR2K1 w - - 0 1", {}, "d1d5",
         {pvl(300, "c1b2 g8f8"), pvl(0, "d1d5 b2c1q d5d1 c1d1", -2)}, "b2c1q", "g8f8", 1, 6},
        // 1.h5?? a2 2.Kc2 a1=Q: the coach's spare queen out of its reserve and back.
        {"promotion race", "8/8/5k2/8/7P/p2K4/8/8 w - - 0 1", {}, "h4h5",
         {pvl(300, "d3c2 a3a2 c2b2"), pvl(-800, "h4h5 a3a2 d3c2 a2a1q")}, "a3a2", "a3a2", 1, 3},
        // 1.a3?? Rd1+ 2.Rxd1 Rxd1#: a check, the human's recapture, the mate.
        {"back-rank mate in two", "3r2k1/3r1ppp/8/8/8/8/P4PPP/2R3K1 w - - 0 1", {}, "a2a3",
         {pvl(-20, "h2h3 d7d2"), pvl(-30, "g2g3 d7d2"), pvl(0, "a2a3 d7d1 c1d1 d8d1", -2)}, "d7d1", "d7d2", 1, 6},
        // 1.Bxa7?? b6, and the king walks over to take the bishop: seven plies (levels 2-3).
        {"trapped bishop", "3k4/ppp2ppp/8/8/8/4B3/5PPP/6K1 w - - 0 1", {}, "e3a7",
         {pvl(100, "g1f1 d8c8"), pvl(-250, "e3a7 b7b6 g1f1 d8c8 f1e2 c8b7 e2d3 b7a7")}, "b7b6", "d8c8", 2, 3},
        // The worked example after an opening: 4.Nxe5?? Qg5 5.Bxf7+ Kd8 6.O-O Qxe5 (a capture
        // already beside the board; a check, a king's move, castling; from level 4: Qg5 only).
        {"fork after an opening", nullptr, {"e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "c6d4"}, "f3e5",
         {pvl(138, "f3d4 e5d4 e1g1 c7c6 f1e1 d7d6"), pvl(125, "e1g1 d4f3 d1f3 d8f6 f3g3 d7d6"),
          pvl(-90, "f3e5 d8g5 c4f7 e8d8 e1g1 g5e5 c2c3 d4e6 d2d3 g7g5")},
         "d8g5", "e5d4", 1, 6},
        // 1.b8=Q (the human's spare queen on the board, its pawn beside it) f6 2.Qb3?? Nxb3: the
        // demonstration takes the spare queen to the coach's half, the rewind brings it back.
        {"queen promoted then lost", "8/1P3ppp/6k1/8/3n4/8/5PPP/6K1 w - - 0 1", {"b7b8q", "f7f6"}, "b8b3",
         {pvl(900, "g1f1 g6h6"), pvl(-100, "b8b3 d4b3 g1f1")}, "d4b3", "g6h6", 1, 3},
    };
}

// The FEN of the position before the blunder (the A0 key of the fake Analyst).
std::string rootOf(const Blunder& b) {
    Game g;
    if (b.fen) CHECK(g.resetFromFEN(b.fen));
    for (const char* u : b.setup) g.play(g.position().parseUCI(u));
    return g.position().fen();
}

// Starts the scenario at 'level' and plays up to the blunder (included).
bool startBlunder(Scene& s, const Blunder& b, int level) {
    s.analyst.results[rootOf(b) + "|A0"] = analysisOf(b.a0);
    s.start(b.fen, level, chess::White);
    if (!playLine(s, b.setup)) return false;
    return s.humanMove(b.played);
}

enum class Answer { Accept, Decline, PlayOn };

// Up to the card, then the answer and the game on from there.
void answerAndGoOn(Scene& s, const Blunder& b, Answer answer) {
    CHECK(s.until([&] { return s.session.offerOpen() || !s.coachHolds(); }, 180.0f));
    CHECK(s.session.offerOpen());
    if (!s.session.offerOpen()) return;
    s.run(0.5f);   // the card waits
    CHECK(s.session.offerOpen());
    switch (answer) {
    case Answer::Accept:
        s.session.onOfferAnswer(s.game, true);
        CHECK(s.until([&] { return s.humanMayMove(); }, 60.0f));
        CHECK(s.tableShowsGame());
        CHECK(s.humanMove(b.a0[0].pv[0].c_str()));
        CHECK(s.coachReply(b.afterBetter));
        break;
    case Answer::Decline:
        s.session.onOfferAnswer(s.game, false);
        CHECK(s.coachReply(b.reply));
        break;
    case Answer::PlayOn:
        s.session.onPlayerActive();
        CHECK(s.coachReply(b.reply));
        break;
    }
    CHECK(s.quiet());
    CHECK(s.tableShowsGame());
}

const Blunder* blunderNamed(const std::vector<Blunder>& all, const char* name) {
    for (const Blunder& b : all)
        if (std::string(b.name) == name) return &b;
    CHECK(false);
    return nullptr;
}

// Frames from the blunder to its card, left to run.
int framesToCard(const Blunder& b, int level) {
    Scene s;
    if (!startBlunder(s, b, level)) return -1;
    int n = 0;
    while (!s.session.offerOpen() && n < 60 * 180) {
        s.step();
        ++n;
    }
    return s.session.offerOpen() ? n : -1;
}

// The scenarios cut short below, at levels where their lines are long or touch the spares.
struct Case {
    const char* name;
    int level;
};
const std::vector<Case> kCuts = {
    {"fork after an opening", 1},      // five plies: a check, a king's move, castling, a recapture
    {"trapped bishop", 2},             // seven plies
    {"capture that promotes", 1},      // the coach's spare queen, its pawn beside the board
    {"promotion race", 1},             // the same, without a capture
    {"en passant fork", 1},            // en passant
    {"back-rank mate in two", 2},      // a check, the human's recapture, the mate
    {"capture that promotes", 5},      // stopped where the problem stands
    {"queen promoted then lost", 1},   // the human's spare queen taken by the line
};

enum class Cut { TakeBack, Resign, Draw };

// The game cut short at this frame as the scene lets it be: the pause menu's takeback (once the
// table has no job, GameScene::coachCanTakeBack), its resignation, a draw the coach agrees to (both
// on a quiet turn, which the review is: Turn::AiThinking). After it the table shows the game, no
// card is up, and the coach has played nothing but its reply to the human's new move (a takeback).
void cutAndGoOn(Scene& s, const Blunder& b, Cut cut) {
    const size_t plies = s.game.moves().size();
    switch (cut) {
    case Cut::TakeBack:
        CHECK(s.until([&] { return !s.stage.tableBusy() && s.session.canTakeBack(s.game); }, 120.0f));
        s.session.setPaused(true);   // the menu is open (the scene freezes its table meanwhile)
        s.run(0.3f);
        s.session.onTakeBackRequested(s.game);
        s.session.setPaused(false);
        CHECK(s.until([&] { return s.humanMayMove(); }, 120.0f));
        CHECK_EQ(s.stage.gameUndos, 1);
        CHECK_EQ(s.game.moves().size(), plies - 1);
        CHECK(s.tableShowsGame());
        CHECK(s.humanMove(b.a0[0].pv[0].c_str()));
        CHECK(s.coachReply(b.afterBetter));
        break;
    case Cut::Resign:
        s.game.resign(s.human);
        s.session.onGameOver(s.game, true);
        break;
    case Cut::Draw:
        s.session.onDrawAnswer(true);
        s.game.agreeDraw();
        s.session.onGameOver(s.game, false);
        break;
    }
    CHECK(s.quiet());
    CHECK(!s.session.offerOpen());
    CHECK(s.tableShowsGame());
    if (cut != Cut::TakeBack) CHECK_EQ(s.game.moves().size(), plies);
}

}  // namespace

// Every scenario at every level where it is demonstrated, each answer to the card: the card opens
// once the line is taken back, the coach waits for the answer, and whenever it plays the table
// stands exactly as the game left it.
TEST(coach_demo_table_restored_whatever_the_answer) {
    int runs = 0, plies = 0;
    for (const Blunder& b : blunders()) {
        for (int level = b.fromLevel; level <= b.toLevel; ++level) {
            for (Answer answer : {Answer::Accept, Answer::Decline, Answer::PlayOn}) {
                Scene s;
                if (!startBlunder(s, b, level)) {
                    CHECK(false);
                    continue;
                }
                answerAndGoOn(s, b, answer);
                if (s.stage.demoMoves == 0 || s.offersShown != 1)
                    std::fprintf(stderr, "  %s, level %d: %d demonstration moves, %d cards\n", b.name, level,
                                 s.stage.demoMoves, s.offersShown);
                CHECK(s.stage.demoMoves > 0);
                CHECK_EQ(s.offersShown, 1);
                CHECK_EQ(s.stage.gameUndos, answer == Answer::Accept ? 1 : 0);
                s.checkTable();
                ++runs;
                plies += s.stage.demoMoves;
            }
        }
    }
    std::fprintf(stderr, "  %d reviews, %d demonstration moves\n", runs, plies);
    CHECK(runs >= 90);
}

// Space at any moment of the review (once or twice): the narration goes, the moves still shown go
// back (briskly), the card still comes once the table is put back, silently, and the coach waits for
// its answer.
TEST(coach_demo_table_space_keeps_the_rewind_and_the_card) {
    const std::vector<Blunder> all = blunders();
    int runs = 0;
    for (const Case& c : kCuts) {
        const Blunder* b = blunderNamed(all, c.name);
        if (!b) continue;
        const int n = framesToCard(*b, c.level);
        CHECK(n > 0);
        if (n <= 0) continue;
        for (int i = 0; i <= 13; ++i) {
            const int k = i == 13 ? n + 20 : n * i / 12;   // the last one over the card
            Scene s;
            if (!startBlunder(s, *b, c.level)) {
                CHECK(false);
                continue;
            }
            for (int f = 0; f < k; ++f) s.step();
            s.session.skip();
            if (i % 2 == 1) {
                s.run(0.4f);
                s.session.skip();
            }
            const Answer answer = i % 3 == 0 ? Answer::Accept : i % 3 == 1 ? Answer::Decline : Answer::PlayOn;
            answerAndGoOn(s, *b, answer);
            if (s.offersShown != 1) std::fprintf(stderr, "  %s, level %d, Space at frame %d of %d\n", c.name, c.level, k, n);
            CHECK_EQ(s.offersShown, 1);
            s.checkTable();
            ++runs;
        }
    }
    CHECK(runs >= 90);
}

// The game cut short at any moment of the review: a takeback from the pause menu, a resignation, a
// draw agreed. The pieces shown go back first; the coach plays nothing over them.
TEST(coach_demo_table_cut_short_by_the_menu) {
    const std::vector<Blunder> all = blunders();
    int runs = 0;
    for (const Case& c : kCuts) {
        const Blunder* b = blunderNamed(all, c.name);
        if (!b) continue;
        const int n = framesToCard(*b, c.level);
        CHECK(n > 0);
        if (n <= 0) continue;
        for (Cut cut : {Cut::TakeBack, Cut::Resign, Cut::Draw}) {
            for (int i = 0; i <= 11; ++i) {
                const int k = i == 11 ? n + 20 : n * i / 10;   // the last one over the card
                Scene s;
                if (!startBlunder(s, *b, c.level)) {
                    CHECK(false);
                    continue;
                }
                for (int f = 0; f < k; ++f) s.step();
                const int shown = s.offersShown;
                cutAndGoOn(s, *b, cut);
                // No card after the game is over; after a takeback, none for the better move.
                CHECK_EQ(s.offersShown, shown);
                s.checkTable();
                ++runs;
            }
        }
    }
    CHECK(runs >= 200);
}

// Two blunders in one game, the first declined (its knight taken for real, beside the board in the
// coach's half), the second demonstrated with a knight of the same colour taken beside it: each
// rewind brings back the very piece the demonstration took, never the game's own victim.
TEST(coach_demo_table_two_demonstrations_in_one_game) {
    const char* fen = "r5k1/5pp1/4p3/7p/8/2N1N3/5PPP/6K1 w - - 0 1";
    Game g;
    CHECK(g.resetFromFEN(fen));
    const std::string root1 = g.position().fen();
    g.play(g.position().parseUCI("c3d5"));
    g.play(g.position().parseUCI("e6d5"));
    const std::string root2 = g.position().fen();
    int runs = 0;
    for (int level = 1; level <= 3; ++level) {
        for (Answer answer : {Answer::Accept, Answer::Decline, Answer::PlayOn}) {
            Scene s;
            s.analyst.results[root1 + "|A0"] = analysisOf({pvl(20, "g1f1 g8f8"), pvl(-200, "c3d5 e6d5 g1f1")});
            s.analyst.results[root2 + "|A0"] = analysisOf({pvl(-200, "g1f1 g8f8"), pvl(-550, "e3g4 h5g4 g1f1")});
            s.start(fen, level, chess::White);
            CHECK(s.humanMove("c3d5"));
            CHECK(s.until([&] { return s.session.offerOpen() || !s.coachHolds(); }, 180.0f));
            CHECK(s.session.offerOpen());
            s.session.onOfferAnswer(s.game, false);
            CHECK(s.coachReply("e6d5"));
            const int first = s.stage.demoMoves;
            CHECK(first > 0);
            Blunder second{"second knight", fen, {}, "e3g4", {pvl(-200, "g1f1 g8f8")}, "h5g4", "g8f8", level, level};
            CHECK(s.humanMove("e3g4"));
            answerAndGoOn(s, second, answer);
            if (s.stage.demoMoves <= first || s.offersShown != 2)
                std::fprintf(stderr, "  level %d: %d then %d demonstration moves, %d cards\n", level, first,
                             s.stage.demoMoves - first, s.offersShown);
            CHECK(s.stage.demoMoves > first);
            CHECK_EQ(s.offersShown, 2);
            s.checkTable();
            ++runs;
        }
    }
    CHECK_EQ(runs, 9);
}

namespace {

// A demonstration the offer policy (Reviewer::review, review.cpp) leaves without a card: the line
// is shown and taken back, then the coach plays its reply at once and says "Your move."
struct NoCard {
    const char* name;
    std::string fen;                     // the position before the human's move(s)
    std::vector<const char*> played;     // the human's moves: each but the last offered and taken back
    std::map<std::string, ai::Analysis> results;   // the fake Analyst's, by "fen|shape"
    const char* reply;                   // the coach's reply to the last one
    int fromLevel, toLevel;
};

std::string fenAfterSan(const std::vector<const char*>& sans) {
    Game g;
    for (const char* s : sans) {
        const Move m = g.position().parseSAN(s);
        CHECK(m.valid());
        if (m.valid()) g.play(m);
    }
    return g.position().fen();
}

std::vector<NoCard> noCards() {
    std::vector<NoCard> v;
    {
        // coach_review_rook_lost_in_a_lost_position: 16.Kg2? Nxg3+ 17.Kxg3 Qxh1, an inaccuracy by
        // the numbers (W% 16 -> 10) that loses the exchange and more. Shown at every level, without a
        // verdict (review.cpp, hiddenLoss) and without an offer.
        NoCard c{"rook lost in a lost position", "", {"f1g2"}, {}, "e4g3", 1, 6};
        c.fen = fenAfterSan({"d4", "d5", "e3", "Nf6", "Bb5+", "Bd7", "Bxd7+", "Nbxd7", "f4", "Rc8", "c4", "dxc4", "Na3",
                             "Nb6", "Bd2", "Qd5", "b3", "cxb3", "axb3", "Ne4", "g3", "e6", "Bc1", "Qa5+", "Kf1", "Qd5",
                             "Nc4", "Rd8", "Ne5", "a6"});
        c.results[c.fen + "|A0"] = analysisOf({pvl(-447, "d1f3 f8d6 e5g4 c7c5 a1a5 f7f5 g4f2 e8g8 f1g2 b6d7"),
                                               pvl(-468, "d1c2 f7f6 e5f3 c7c5 a1a5 g7g5 f4g5 f6g5"),
                                               pvl(-486, "e5f3 f8b4 f1g2 e8g8 d1c2 c7c5 d4c5 d8c8")});
        c.results[c.fen + "|A1"] =
            analysisOf({pvl(-582, "f1g2 e4g3 g2g3 d5h1 e5f3 g7g5 f4g5 h7h6 e3e4 h8g8 a1a2 h6g5")});
        v.push_back(c);
    }
    {
        // A blunder that leaves the human still clearly ahead (W% 92 -> 76): no offer from 75 on.
        NoCard c{"a blunder still ahead", "6k1/5ppp/4p3/8/8/2N5/5PPP/R5K1 w - - 0 1", {"c3d5"}, {}, "e6d5", 1, 3};
        c.results[c.fen + "|A0"] = analysisOf({pvl(650, "g1f1 g8f8"), pvl(320, "c3d5 e6d5 g1f1")});
        v.push_back(c);
    }
    {
        // Three knights lost on the same move: the first two offered and taken back, the third shown
        // without a card (review.cpp: two offers per ply at most).
        NoCard c{"a third try at the same move", "r5k1/6pp/p1p1p3/5p2/8/2N5/5PPP/6K1 w - - 0 1",
                 {"c3d5", "c3b5", "c3e4"}, {}, "f5e4", 1, 3};
        c.results[c.fen + "|A0"] = analysisOf({pvl(20, "g1f1 g8f8"), pvl(-330, "c3d5 e6d5 g1f1"),
                                               pvl(-330, "c3b5 c6b5 g1f1"), pvl(-330, "c3e4 f5e4 g1f1")});
        v.push_back(c);
    }
    return v;
}

}  // namespace

// The demonstrations without a card (see the report's list): the coach plays only once the line is
// taken back, never over it, and the table shows the game when it does.
TEST(coach_demo_table_no_card_after_some_demonstrations) {
    int runs = 0;
    for (const NoCard& c : noCards()) {
        for (int level = c.fromLevel; level <= c.toLevel; ++level) {
            Scene s;
            s.analyst.results = c.results;
            s.start(c.fen.c_str(), level, chess::White);
            const int taken = int(c.played.size()) - 1;   // offered and taken back first
            bool ok = true;
            for (int i = 0; i < taken && ok; ++i) {
                ok = s.humanMove(c.played[size_t(i)]) &&
                     s.until([&] { return s.session.offerOpen() || !s.coachHolds(); }, 180.0f) && s.session.offerOpen();
                if (ok) s.session.onOfferAnswer(s.game, true);
            }
            const int before = s.stage.demoMoves;
            ok = ok && s.humanMove(c.played.back());
            CHECK(ok);
            if (!ok) continue;
            // The coach plays once the line is back (coachReply checks the table), with no card;
            // its move queues "Your move." (the observer sees beats as they are queued).
            CHECK(s.until([&] { return !s.coachHolds(); }, 180.0f));
            const size_t said = s.lines.size();
            CHECK(s.coachReply(c.reply));
            const int shown = s.stage.demoMoves - before;
            CHECK(s.quiet());
            bool yourMove = false;
            for (size_t i = said; i < s.lines.size(); ++i) yourMove = yourMove || s.lines[i].key == "event.your_move";
            if (shown == 0 || s.offersShown != taken || !yourMove)
                std::fprintf(stderr, "  %s, level %d: %d demonstration moves, %d cards, \"Your move\" %d\n", c.name, level,
                             shown, s.offersShown, int(yourMove));
            CHECK(shown > 0);
            CHECK_EQ(s.offersShown, taken);
            CHECK(yourMove);
            CHECK(s.tableShowsGame());
            s.checkTable();
            ++runs;
        }
    }
    CHECK_EQ(runs, 6 + 3 + 3);
}

// A mistake, not a blunder (the en passant fork already a piece down, W% 19 -> 7): said in words
// (the verdict at levels 3-6, then the fork), without a demonstration or a card. The coach then plays
// its move on the table, and the player answers it ("Your move.").
TEST(coach_demo_table_mistake_said_then_played) {
    const std::string fen = "6k1/4rppp/8/8/4p3/8/2RPN1PP/6K1 w - - 0 1";
    for (int level = 2; level <= 6; ++level) {
        Scene s;
        s.analyst.results[fen + "|A0"] = analysisOf({pvl(-400, "g1f1 g8f8"), pvl(-700, "d2d4 e4d3 c2c1 d3e2")});
        s.start(fen.c_str(), level, chess::White);
        const int before = s.stage.demoMoves;
        CHECK(s.humanMove("d2d4"));
        CHECK(s.until([&] { return !s.coachHolds(); }, 180.0f));
        CHECK(!s.session.offerOpen());
        bool fork = false, verdict = false;
        for (const Line& l : s.lines) {
            fork = fork || l.key.rfind("ex.fork.", 0) == 0;
            verdict = verdict || l.key.rfind("ex.verdict.mistake.", 0) == 0;
        }
        CHECK(fork);
        CHECK_EQ(verdict, level >= 3);
        const size_t said = s.lines.size();
        CHECK(s.coachReply("e4d3"));
        CHECK(s.quiet());
        CHECK_EQ(s.stage.demoMoves - before, 0);
        CHECK_EQ(s.offersShown, 0);
        bool yourMove = false;
        for (size_t i = said; i < s.lines.size(); ++i) yourMove = yourMove || s.lines[i].key == "event.your_move";
        CHECK(yourMove);
        CHECK(s.tableShowsGame());
        s.checkTable();
    }
}

// The takeback cap of levels 4-6 (band(): 5, 3 and 2 takebacks a game, only those the player
// accepts counted): declined, the card comes back every time; accepted, past the cap a blunder is
// still demonstrated, without a card, the coach saying once that no takeback is left before its
// "Now I play my move" (review.cpp; mates are not capped). The two forks alternate, each at its own
// ply (two offers per ply is another limit).
TEST(coach_demo_table_takeback_cap_leaves_the_demonstration) {
    const std::vector<Blunder> all = blunders();
    const Blunder* forks[2] = {blunderNamed(all, "en passant fork"), blunderNamed(all, "fork after an opening")};
    if (!forks[0] || !forks[1]) return;
    for (int level = 4; level <= 6; ++level) {
        const int cap = level == 4 ? 5 : level == 5 ? 3 : 2;
        Reviewer declining, accepting;
        declining.reset(level, chess::White);
        accepting.reset(level, chess::White);
        for (int i = 0; i <= cap + 1; ++i) {
            const Blunder& b = *forks[i % 2];
            Game g;
            if (b.fen) CHECK(g.resetFromFEN(b.fen));
            for (const char* u : b.setup) g.play(g.position().parseUCI(u));
            g.play(g.position().parseUCI(b.played));
            const ai::Analysis a0 = analysisOf(b.a0);
            ReviewInput in;
            in.game = &g;
            in.before = &a0;
            CHECK(declining.review(in).offersTakeback);
            declining.takebackDeclined();
            CHECK_EQ(declining.takebacksLeft(), cap);
            CHECK_EQ(accepting.takebacksLeft(), std::max(0, cap - i));
            const Review r = accepting.review(in);
            int demos = 0, noneLeft = -1;
            for (size_t k = 0; k < r.script.size(); ++k) {
                demos += r.script[k].kind == BeatKind::DemoMove ? 1 : 0;
                if (r.script[k].line.key == "tb.none_left") noneLeft = int(k);
            }
            if (demos == 0 || r.offersTakeback != (i < cap) || (noneLeft >= 0) != (i == cap))
                std::fprintf(stderr, "  level %d, blunder %d: %d demonstration moves, offer %d, \"none left\" at %d\n", level,
                             i + 1, demos, int(r.offersTakeback), noneLeft);
            CHECK(demos > 0);
            CHECK_EQ(r.offersTakeback, i < cap);
            CHECK_EQ(noneLeft >= 0, i == cap);
            if (r.offersTakeback) {
                g.undo(1);
                accepting.takebackAccepted(g);
            } else {
                CHECK(!r.script.empty() && r.script.back().line.key == "demo.play_on");
                if (noneLeft >= 0) CHECK_EQ(noneLeft, int(r.script.size()) - 2);
            }
        }
        CHECK_EQ(accepting.takebacksLeft(), 0);
    }
}

// Level 6 (two takebacks a game) through the table: the en passant fork offered and taken back at
// plies 0 and 2 (the kings step aside and back in between), then played a third time at ply 4,
// shown without a card. The count the HUD shows (Session::takebacksLeft) stays while a card is up
// and drops when it is accepted; the third review says that no takeback is left, then "Now I play
// my move", and the coach plays once the line is back.
TEST(coach_demo_table_takebacks_run_out) {
    const std::vector<Blunder> all = blunders();
    const Blunder* fork = blunderNamed(all, "en passant fork");
    if (!fork) return;
    const std::vector<const char*> steps = {"g1f1", "g8f8", "f1g1", "f8g8"};
    Scene s;
    Game roots;
    CHECK(roots.resetFromFEN(fork->fen));
    s.analyst.results[roots.position().fen() + "|A0"] = analysisOf(fork->a0);
    roots.play(roots.position().parseUCI(steps[0]));
    roots.play(roots.position().parseUCI(steps[1]));
    s.analyst.results[roots.position().fen() + "|A0"] =
        analysisOf({pvl(0, "f1g1 f8g8"), pvl(-380, "d2d4 e4d3 c2c1 d3e2")});
    roots.play(roots.position().parseUCI(steps[2]));
    roots.play(roots.position().parseUCI(steps[3]));
    s.analyst.results[roots.position().fen() + "|A0"] = analysisOf(fork->a0);
    s.start(fork->fen, 6, chess::White);
    CHECK(s.quiet());
    CHECK_EQ(s.session.takebacksLeft(), 2);
    for (int round = 0; round < 2; ++round) {
        CHECK(s.humanMove(fork->played));
        CHECK(s.until([&] { return s.session.offerOpen() || !s.coachHolds(); }, 180.0f));
        CHECK(s.session.offerOpen());
        if (!s.session.offerOpen()) return;
        CHECK_EQ(s.session.takebacksLeft(), 2 - round);   // the card shows the count before the answer
        CHECK(s.session.offerUsesTakeback());
        s.session.onOfferAnswer(s.game, true);
        CHECK_EQ(s.session.takebacksLeft(), 1 - round);
        CHECK(s.until([&] { return s.humanMayMove(); }, 60.0f));
        CHECK(s.tableShowsGame());
        CHECK(s.humanMove(steps[size_t(2 * round)]));
        CHECK(s.coachReply(steps[size_t(2 * round + 1)]));
        CHECK(s.quiet());
    }
    CHECK_EQ(s.game.moves().size(), size_t(4));
    CHECK(!s.queued("tb.none_left"));
    const size_t said = s.lines.size();
    const int demosBefore = s.stage.demoMoves;
    CHECK(s.humanMove(fork->played));
    CHECK(s.until([&] { return !s.coachHolds(); }, 180.0f));
    CHECK(!s.session.offerOpen());
    CHECK(s.coachReply(fork->reply));
    CHECK(s.quiet());
    CHECK_EQ(s.offersShown, 2);
    CHECK_EQ(s.session.takebacksLeft(), 0);
    CHECK_EQ(s.queuedCount("tb.none_left"), 1);
    CHECK(s.stage.demoMoves > demosBefore);
    // The third review's lines: "none left" right before "Now I play my move".
    size_t noneLeft = s.lines.size(), playOn = s.lines.size();
    for (size_t i = said; i < s.lines.size(); ++i) {
        if (s.lines[i].key == "tb.none_left") noneLeft = i;
        if (s.lines[i].key == "demo.play_on") playOn = i;
    }
    CHECK(noneLeft < s.lines.size() && playOn == noneLeft + 1);
    CHECK(s.tableShowsGame());
    s.checkTable();
}

