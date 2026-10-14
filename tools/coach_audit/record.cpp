// coach_audit record: the analyses the coach's review asks for (coach/review.h: A0, A1, A2, A3), run
// with the embedded Stockfish on every move of the "human" side of some games, saved to a text file
// that `coach_audit review` replays through the Reviewer at every level without the engine.
//
// Games: PGN files (the human is the side not named "Coach", else both sides), or games played here
// between a weak "human" (Stockfish at a random weak setting, sometimes a random move from a shallow
// MultiPV) and the coach at one of its levels (ai::coachLevelSettings).
#include "audit.h"

#include "ai/engine.h"
#include "chess/pgn.h"
#include "coach/tactics.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <thread>

namespace audit {

using namespace chess;

namespace {

bool waitFor(ai::Engine& e, uint32_t id, ai::Analysis& out, int timeoutMs) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!e.analysisReady(id)) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeoutMs)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return e.takeAnalysis(id, out);
}

ai::Analysis analyse(ai::Engine& e, ai::AnalysisRequest r) {
    r.moveTimeMs = 0;   // depth only: reproducible
    ai::Analysis a;
    if (!waitFor(e, e.requestAnalysis(r), a, 600000)) a.ok = false;
    return a;
}

std::string waitMove(ai::Engine& e, const Game& g) {
    ai::ClockInfo clock;
    const Position& s = g.startPosition();
    e.requestMoveFrom(s.isStandardStart() ? std::string() : s.fen(), g.uciMoves(), clock);
    const auto t0 = std::chrono::steady_clock::now();
    while (!e.moveReady()) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(120)) return std::string();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return e.takeMove();
}

// Every analysis of one human move, before and after it is played (g holds the moves before it).
Record recordMove(ai::Engine& e, const Game& before, const Move& m, int depth) {
    Record rec;
    rec.startFen = before.startPosition().isStandardStart() ? std::string() : before.startPosition().fen();
    rec.moves = before.uciMoves();
    rec.played = before.position().toUCI(m);
    rec.human = before.position().sideToMove();
    coach::Reviewer rv;
    rv.reset(1, rec.human);
    ai::AnalysisRequest q0 = rv.beforeRequest(before);
    q0.depth = depth;
    rec.a0 = analyse(e, q0);
    rec.a3 = analyse(e, rv.shallowRequest(before));
    Game after = before;
    after.play(m);
    if (rec.a0.ok && coach::Reviewer::needsPlayedRequest(rec.a0, rec.played)) {
        rec.a1 = analyse(e, rv.playedRequest(after, rec.a0));
        rec.hasA1 = true;
    }
    if (after.position().hasLegalMove()) {
        rec.a2 = analyse(e, rv.afterRequest(after));
        rec.hasA2 = true;
    }
    return rec;
}

// The move leaves the opponent a capture that wins 'points' or more by exchange.
bool leavesMaterial(const Game& before, const Move& m, int points = 2) {
    Position p = before.position();
    p.makeMove(m);
    return coach::bestCapturePoints(p, p.sideToMove()) >= points;
}

// A move that costs what one of the bands says (win-% points at 'depth', every legal move searched;
// the review, deeper, mostly agrees on its class; a band picked at random, 10.5 to 14.5 when there is
// none: a mistake), picked at random among those that fit and differ from 'other'; an invalid move
// when there is none.
Move badMove(ai::Engine& e, const Game& g, std::mt19937& rng, const std::vector<std::pair<double, double>>& bands,
             int depth = 12, const std::string& other = std::string()) {
    const std::pair<double, double> band = bands.empty() ? std::make_pair(10.5, 14.5) : bands[rng() % bands.size()];
    ai::AnalysisRequest r;
    const Position& s = g.startPosition();
    r.startFen = s.isStandardStart() ? std::string() : s.fen();
    r.moves = g.uciMoves();
    r.multiPV = std::min(64, int(g.position().legalMoves().size()));
    r.depth = depth;
    const ai::Analysis a = analyse(e, r);
    if (!a.ok || a.lines.size() < 2) return Move{};
    const double best = coach::winPercent(a.lines[0].score);
    std::vector<Move> picks;
    for (const ai::PvLine& l : a.lines) {
        const double cost = best - coach::winPercent(l.score);
        if (!l.pv.empty() && l.pv[0] != other && cost >= band.first && cost <= band.second)
            picks.push_back(g.position().parseUCI(l.pv[0]));
    }
    return picks.empty() ? Move{} : picks[rng() % picks.size()];
}

// The side to move's win percentage in the position after the game's moves, at depth 10 (-1 when
// the search fails; 0 or 100 when the game is over).
double winAt(ai::Engine& e, const Game& g) {
    if (g.position().isCheckmate()) return 0.0;
    if (g.isOver()) return 50.0;
    ai::AnalysisRequest r;
    const Position& s = g.startPosition();
    r.startFen = s.isStandardStart() ? std::string() : s.fen();
    r.moves = g.uciMoves();
    r.multiPV = 1;
    r.depth = 10;
    const ai::Analysis a = analyse(e, r);
    return a.ok && !a.lines.empty() ? coach::winPercent(a.lines[0].score) : -1.0;
}

// One field of a comma-separated line (no quoting in the puzzle CSV).
std::string csvField(const std::string& line, int n) {
    size_t at = 0;
    for (int i = 0; i < n; ++i) {
        at = line.find(',', at);
        if (at == std::string::npos) return std::string();
        ++at;
    }
    const size_t end = line.find(',', at);
    return line.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

ai::EngineSettings humanSettings(std::mt19937& rng) {
    ai::EngineSettings s;
    s.humanize = false;
    s.useClock = false;
    s.hashMB = 16;
    switch (std::uniform_int_distribution<int>(0, 3)(rng)) {
    case 0: s.skillLevel = 0; s.depth = 1 + int(rng() % 4); s.multiPV = 1 + int(rng() % 4); break;
    case 1: s.skillLevel = int(rng() % 6); s.depth = 4 + int(rng() % 4); break;
    case 2: s.limitStrength = true; s.elo = 1320 + int(rng() % 500); s.moveTimeMs = 150; break;
    default: s.skillLevel = 3 + int(rng() % 8); s.depth = 6 + int(rng() % 4); break;
    }
    return s;
}

}  // namespace

int recordMain(const Options& o) {
    ai::Engine e;
    if (!e.start() || !e.waitReady(60000)) {
        std::fprintf(stderr, "engine unavailable\n");
        return 1;
    }
    std::ofstream out(o.out, std::ios::app);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", o.out.c_str());
        return 1;
    }
    int written = 0;
    const auto stopped = [&]() { return !o.stop.empty() && std::ifstream(o.stop).good(); };
    auto emit = [&](const Record& r) {
        out << writeRecord(r);
        out.flush();
        ++written;
    };

    // Bad moves in recorded positions: each position before a human move whose best line leaves
    // the game undecided (12 to 88 win-%), with another move that costs what a band says (searched
    // at depth 10: the self-play's depth 12 for every move costs twice the time, for the class the
    // review measures again).
    std::mt19937 prng(o.seed);
    for (const std::string& path : o.positions) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        std::vector<Record> recs;
        std::string err;
        if (!readRecords(ss.str(), recs, err)) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
            continue;
        }
        const int before = written;
        for (const Record& src : recs) {
            if (stopped()) break;
            if (!src.a0.ok || src.a0.lines.empty()) continue;
            const double w = coach::winPercent(src.a0.lines[0].score);
            if (w < 12.0 || w > 88.0) continue;
            Game g;
            if (!src.startFen.empty() && !g.resetFromFEN(src.startFen)) continue;
            bool ok = true;
            for (const std::string& u : src.moves) {
                const Move m = g.position().parseUCI(u);
                if (!m.valid() || !g.play(m)) {
                    ok = false;
                    break;
                }
            }
            if (!ok || g.isOver()) continue;
            ai::EngineSettings host;
            host.hashMB = 64;
            host.threads = o.threads;
            e.configure(host);
            e.newGame();
            const Move m = badMove(e, g, prng, o.bands, 10, src.played);
            if (m.valid()) emit(recordMove(e, g, m, o.depth));
        }
        std::fprintf(stderr, "positions %s: %d records\n", path.c_str(), written - before);
    }

    // Lichess puzzles (PuzzleId,FEN,Moves,Rating,RatingDeviation,Popularity,NbPlays,Themes,...): the
    // FEN is the position before the move that loses, the first of Moves, which the side to move
    // played in a real game; the rest is the refutation.
    for (const std::string& path : o.puzzles) {
        std::ifstream in(path, std::ios::binary);
        std::string line;
        const int before = written;
        while (std::getline(in, line) && !stopped()) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::string fen = csvField(line, 1), moves = csvField(line, 2);
            Game g;
            if (fen.empty() || moves.empty() || !g.resetFromFEN(fen) || g.isOver()) continue;
            const Move m = g.position().parseUCI(moves.substr(0, moves.find(' ')));
            if (!m.valid()) continue;
            ai::EngineSettings host;
            host.hashMB = 64;
            host.threads = o.threads;
            e.configure(host);
            e.newGame();
            Record r = recordMove(e, g, m, o.depth);
            r.tag = "puzzle " + csvField(line, 0) + " " + csvField(line, 3) + " " + csvField(line, 7);
            emit(r);
        }
        std::fprintf(stderr, "puzzles %s: %d records\n", path.c_str(), written - before);
    }

    // PGN games.
    for (const std::string& path : o.pgns) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const auto res = pgn::read(ss.str());
        for (const pgn::ParsedGame& pg : res.games) {
            if (!pg.ok()) continue;
            const bool coachWhite = pg.record.tag("White") == "Coach", coachBlack = pg.record.tag("Black") == "Coach";
            Game g;
            if (!pg.record.fen.empty() && !g.resetFromFEN(pg.record.fen)) continue;
            const std::vector<Position> pos = pg.record.positions();
            ai::EngineSettings host;
            host.hashMB = 64;
            host.threads = o.threads;
            e.configure(host);
            e.newGame();
            const std::string tag = "game " + pg.record.tag("Site") + " " + pg.record.tag("WhiteElo") + "/" +
                                    pg.record.tag("BlackElo") + " " + pg.record.tag("TimeControl");
            // A sample's picks: the plies its AuditPlies tag lists (from 1), recorded without the
            // --min-drop scan, its AuditTag added to their tag.
            std::set<size_t> picks;
            {
                std::istringstream ps(pg.record.tag("AuditPlies"));
                for (size_t n; ps >> n;) picks.insert(n);
            }
            // --min-drop: every position searched once (depth 10), a move's cost read from the
            // positions before and after it.
            std::vector<double> win;
            if (o.minDrop > 0 && picks.empty()) {
                Game scan = g;
                win.push_back(winAt(e, scan));
                for (const pgn::Ply& ply : pg.record.plies) {
                    const Move m = scan.position().parseSAN(ply.san);
                    if (!m.valid() || !scan.play(m) || stopped()) break;
                    win.push_back(winAt(e, scan));
                }
            }
            for (size_t i = 0; i < pg.record.plies.size() && !stopped(); ++i) {
                const Move m = g.position().parseSAN(pg.record.plies[i].san);
                if (!m.valid()) break;
                const Color mover = g.position().sideToMove();
                const bool human = mover == White ? !coachWhite : !coachBlack;
                double drop = 0.0;
                if (o.minDrop > 0 && picks.empty()) {
                    if (i + 1 >= win.size() || win[i] < 0 || win[i + 1] < 0) break;
                    drop = win[i] - (100.0 - win[i + 1]);
                }
                if (!picks.empty()) {
                    if (human && picks.count(i + 1)) {
                        Record r = recordMove(e, g, m, o.depth);
                        r.tag = tag + " ply " + std::to_string(i + 1) + " " + pg.record.tag("AuditTag");
                        emit(r);
                    }
                } else if (human && (!o.materialOnly || leavesMaterial(g, m)) && (o.minDrop <= 0 || drop >= o.minDrop)) {
                    Record r = recordMove(e, g, m, o.depth);
                    char d[48];
                    std::snprintf(d, sizeof d, " ply %zu drop %.1f", i + 1, drop);
                    r.tag = tag + d;
                    emit(r);
                }
                if (!picks.empty() && i + 1 >= *picks.rbegin()) break;
                if (!g.play(m)) break;
            }
            std::fprintf(stderr, "pgn %s: %d records\n", path.c_str(), written);
        }
    }

    // Games played here.
    std::mt19937 rng(o.seed);
    for (int game = 0; game < o.selfplay; ++game) {
        const int level = 1 + int(rng() % 6);
        const Color human = (rng() & 1) ? White : Black;
        ai::EngineSettings hs = humanSettings(rng);
        ai::EngineSettings cs = ai::coachLevelSettings(level);
        cs.humanize = false;
        cs.useClock = false;
        cs.hashMB = 16;
        if (!cs.depth && !cs.moveTimeMs && !cs.nodes) cs.moveTimeMs = 200;
        hs.threads = cs.threads = o.threads;
        const double randomMove = (rng() % 3 == 0) ? 0.08 : 0.0;   // some games get the odd wild move
        Game g;
        e.newGame();
        int plies = 0;
        while (!g.isOver() && plies < o.maxPlies) {
            const bool humanTurn = g.position().sideToMove() == human;
            Move m;
            if (humanTurn && o.hangRate > 0 && std::uniform_real_distribution<double>(0, 1)(rng) < o.hangRate) {
                std::vector<Move> hangs;
                for (const Move& x : g.position().legalMoves())
                    if (leavesMaterial(g, x, 3)) hangs.push_back(x);
                if (!hangs.empty()) m = hangs[rng() % hangs.size()];
            }
            bool picked = false;
            if (!m.valid() && humanTurn && o.mistakeRate > 0 &&
                std::uniform_real_distribution<double>(0, 1)(rng) < o.mistakeRate) {
                m = badMove(e, g, rng, o.bands);
                picked = m.valid();
            }
            if (!m.valid() && humanTurn && randomMove > 0 &&
                std::uniform_real_distribution<double>(0, 1)(rng) < randomMove) {
                const std::vector<Move> legal = g.position().legalMoves();
                m = legal[rng() % legal.size()];
            }
            if (!m.valid()) {
                e.configure(humanTurn ? hs : cs);
                const std::string uci = waitMove(e, g);
                m = g.position().parseUCI(uci);
            }
            if (!m.valid()) break;
            if (humanTurn && (!o.materialOnly || leavesMaterial(g, m)) && (!o.mistakesOnly || picked))
                emit(recordMove(e, g, m, o.depth));
            g.play(m);
            ++plies;
        }
        std::fprintf(stderr, "selfplay game %d (coach level %d, human %s): %d plies, %d records\n", game, level,
                     human == White ? "White" : "Black", plies, written);
    }
    e.shutdown();
    return 0;
}

int truthMain(const Options& o) {
    std::vector<Record> recs;
    {
        std::ifstream in(o.in, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string err;
        if (!in || !readRecords(ss.str(), recs, err)) {
            std::fprintf(stderr, "%s: %s\n", o.in.c_str(), err.empty() ? "cannot read" : err.c_str());
            return 1;
        }
    }
    size_t done = 0;
    {
        std::ifstream prev(o.out, std::ios::binary);
        std::string line;
        while (std::getline(prev, line))
            if (line == "end") ++done;
    }
    ai::Engine e;
    if (!e.start() || !e.waitReady(60000)) {
        std::fprintf(stderr, "engine unavailable\n");
        return 1;
    }
    ai::EngineSettings host;
    host.hashMB = 128;
    host.threads = o.threads;
    e.configure(host);
    std::ofstream out(o.out, std::ios::app);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", o.out.c_str());
        return 1;
    }
    for (size_t i = done; i < recs.size(); ++i) {
        if (!o.stop.empty() && std::ifstream(o.stop).good()) break;
        const Record& src = recs[i];
        Record t;
        t.tag = src.tag;
        t.startFen = src.startFen;
        t.moves = src.moves;
        t.played = src.played;
        t.human = src.human;
        Game g;
        bool ok = src.startFen.empty() || g.resetFromFEN(src.startFen);
        for (size_t k = 0; ok && k < src.moves.size(); ++k) {
            const Move m = g.position().parseUCI(src.moves[k]);
            ok = m.valid() && g.play(m);
        }
        if (ok && g.position().hasLegalMove() && g.position().parseUCI(src.played).valid()) {
            e.newGame();
            ai::AnalysisRequest r;
            r.startFen = src.startFen;
            r.moves = src.moves;
            r.multiPV = std::min(4, int(g.position().legalMoves().size()));
            r.depth = o.depth;
            t.a0 = analyse(e, r);
            const ai::PvLine* l = t.a0.ok ? t.a0.line(src.played) : nullptr;
            if (t.a0.ok && (!l || l->score.bound != ai::Score::Bound::Exact)) {
                ai::AnalysisRequest p = r;
                p.multiPV = 1;
                p.searchMoves = {src.played};
                t.a1 = analyse(e, p);
                t.hasA1 = true;
            }
        }
        out << writeRecord(t);
        out.flush();
    }
    e.shutdown();
    return 0;
}

int reanalyseMain(const Options& o) {
    std::vector<Record> recs;
    {
        std::ifstream in(o.in, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string err;
        if (!in || !readRecords(ss.str(), recs, err)) {
            std::fprintf(stderr, "%s: %s\n", o.in.c_str(), err.empty() ? "cannot read" : err.c_str());
            return 1;
        }
    }
    // A rerun goes on after the records the output already holds (each one written whole).
    size_t done = 0;
    {
        std::ifstream prev(o.out, std::ios::binary);
        std::string line;
        while (std::getline(prev, line))
            if (line == "end") ++done;
    }
    ai::Engine e;
    if (!e.start() || !e.waitReady(60000)) {
        std::fprintf(stderr, "engine unavailable\n");
        return 1;
    }
    ai::EngineSettings host;
    host.hashMB = 64;
    host.threads = o.threads;
    e.configure(host);
    std::ofstream out(o.out, std::ios::app);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", o.out.c_str());
        return 1;
    }
    std::string lastStart;
    std::vector<std::string> lastMoves;
    bool first = true;
    for (size_t i = done; i < recs.size(); ++i) {
        if (!o.stop.empty() && std::ifstream(o.stop).good()) break;
        const Record& src = recs[i];
        Game g;
        if (!src.startFen.empty() && !g.resetFromFEN(src.startFen)) continue;
        bool ok = true;
        for (const std::string& u : src.moves) {
            const Move m = g.position().parseUCI(u);
            if (!m.valid() || !g.play(m)) {
                ok = false;
                break;
            }
        }
        const Move m = ok ? g.position().parseUCI(src.played) : Move{};
        if (!m.valid()) {
            std::fprintf(stderr, "record %zu: cannot replay, kept as it was\n", i);
            out << writeRecord(src);
            out.flush();
            continue;
        }
        // A new game for the engine unless this record goes on from the last one's moves (the hash
        // table as a recording of the same game leaves it).
        const bool goesOn = !first && src.startFen == lastStart && lastMoves.size() <= src.moves.size() &&
                            std::equal(lastMoves.begin(), lastMoves.end(), src.moves.begin());
        if (!goesOn) e.newGame();
        first = false;
        lastStart = src.startFen;
        lastMoves = src.moves;
        lastMoves.push_back(src.played);
        Record r = recordMove(e, g, m, src.a0.ok && src.a0.depth >= 10 ? src.a0.depth : o.depth);
        r.tag = src.tag;
        out << writeRecord(r);
        out.flush();
    }
    e.shutdown();
    return 0;
}

}  // namespace audit
