// coach_audit commentary: the Analysis mode's comment on each recorded move (the position before it
// analysed with A0, the position after it with A2, else with A1's line past its first move), printed
// with the claims checked against the board: the material a line "wins", the piece a fork or an
// unprotected piece costs (counted from before the move, net of what the move took), and whether
// the best move would have lost as much.
#include "audit.h"

#include "analysis/commentary.h"
#include "analysis/review.h"
#include "coach/catalog.h"
#include "coach/review_internal.h"
#include "coach/tactics.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

namespace audit {

using namespace chess;
using coach::Arg;
using coach::Catalog;
using coach::Line;
using coach::LineStep;

namespace {

struct Finding {
    std::string code, detail;
};

// The position after A1's first move, analysed: its line past that move, its score for the side to
// move there. Not for a mate (the move counts would need care): A2 only then.
bool afterOf(const Record& r, ai::Analysis& out) {
    if (r.hasA2 && r.a2.ok && !r.a2.lines.empty() && !r.a2.lines[0].pv.empty()) {
        out = r.a2;
        return true;
    }
    const ai::PvLine* l = nullptr;
    if (r.hasA1 && r.a1.ok && !r.a1.lines.empty()) l = &r.a1.lines[0];
    else
        for (const ai::PvLine& x : r.a0.lines)
            if (!x.pv.empty() && x.pv[0] == r.played) l = &x;
    if (!l || l->pv.size() < 2 || l->pv[0] != r.played || l->score.mate != 0 || l->score.matedNow || l->score.matesNow)
        return false;
    out = ai::Analysis();
    out.ok = true;
    out.whiteToMove = !r.a0.whiteToMove;
    out.depth = std::max(1, l->depth - 1);
    ai::PvLine p = *l;
    p.multipv = 1;
    p.depth = out.depth;
    p.pv.erase(p.pv.begin());
    p.score.cp = -l->score.cp;
    p.score.bound = ai::Score::Bound::Exact;
    out.lines.push_back(p);
    out.bestMove = p.pv[0];
    return true;
}

std::string render(const Line& l) { return Catalog::shared().renderVariant(l, "en", false, 1).text; }

std::vector<std::string> variantsOf(const std::string& key) {
    std::vector<std::string> v;
    const Catalog& cat = Catalog::shared();
    const int n = cat.variants(key);
    for (int i = 1; i <= n; ++i) {
        const std::string* t = cat.find("en", i == 1 ? key : key + "." + std::to_string(i));
        if (t) v.push_back(*t);
    }
    return v;
}

// The mover's material lost for good at 'q' (relative to the position before the move): what the
// mover, to move there, takes back at once is taken off.
int lossAt(const Position& q, Color mover, int base) {
    int l = base - coach::materialBalance(q, mover);
    if (q.sideToMove() == mover) l -= coach::bestCapturePoints(q, mover);
    return l;
}

// The worst loss for good along the first 'plies' plies of a UCI line from 'start'.
int worstLoss(Position q, const std::vector<std::string>& pv, size_t from, int plies, Color mover, int base) {
    int worst = 0;
    for (size_t i = from; i < pv.size() && int(i - from) < plies; ++i) {
        const Move m = q.parseUCI(pv[i]);
        if (!m.valid()) break;
        q.makeMove(m);
        worst = std::max(worst, lossAt(q, mover, base));
    }
    return worst;
}

bool replaySan(Position p, const std::string& line, Position& end, int& plies) {
    std::istringstream in(line);
    std::string s;
    plies = 0;
    while (in >> s) {
        const Move m = p.parseSAN(s);
        if (!m.valid()) return false;
        p.makeMove(m);
        ++plies;
    }
    end = p;
    return true;
}

// The mover's loss for good after the first 'plies' plies of the reply: what the mover, to move
// there, takes back at once is taken off, unless the line shows it never comes (the mover's next
// move takes nothing, and four plies later the board still shows as much lost; at most what the
// board shows then).
int shownLoss(const Position& afterPos, const std::vector<std::string>& reply, int plies, Color mover, int base) {
    Position q = afterPos;
    const int n = int(reply.size());
    for (int i = 0; i < plies && i < n; ++i) q.makeMove(q.parseUCI(reply[size_t(i)]));
    int l = lossAt(q, mover, base);
    const int board = base - coach::materialBalance(q, mover);
    if (plies < n && l < board) {
        const Move next = q.parseUCI(reply[size_t(plies)]);
        if (next.valid() && q.sideToMove() == mover && q.at(next.to).empty() && !(next.flags & MoveEnPassant)) {
            Position later = q;
            for (int i = plies; i < plies + 4 && i < n; ++i) later.makeMove(later.parseUCI(reply[size_t(i)]));
            const int still = lossAt(later, mover, base);
            if (still > l) l = std::min(board, still);
        }
    }
    return l;
}

}  // namespace

int commentaryMain(const Options& o) {
    std::ifstream f(o.in, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "coach_audit: cannot read %s\n", o.in.c_str());
        return 1;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::vector<Record> recs;
    std::string err;
    if (!readRecords(ss.str(), recs, err)) {
        std::fprintf(stderr, "coach_audit: %s\n", err.c_str());
        return 1;
    }
    Catalog::shared().load();
    FILE* out = o.out.empty() ? stdout : std::fopen(o.out.c_str(), "w");
    if (!out) return 1;
    int reviewed = 0, commented = 0;
    std::map<std::string, int> codes, keys;
    for (size_t ri = 0; ri < recs.size(); ++ri) {
        const Record& r = recs[ri];
        ai::Analysis after;
        if (!r.a0.ok || r.a0.lines.empty() || !afterOf(r, after)) continue;
        Position start;   // the standard start unless the record names another
        if (!r.startFen.empty() && !start.setFEN(r.startFen)) continue;
        Position p = start;
        std::vector<Move> moves;
        bool ok = true;
        for (const std::string& u : r.moves) {
            const Move m = p.parseUCI(u);
            if (!m.valid()) {
                ok = false;
                break;
            }
            moves.push_back(m);
            p.makeMove(m);
        }
        const Position before = p;
        const Move pm = p.parseUCI(r.played);
        if (!ok || !pm.valid()) continue;
        moves.push_back(pm);
        Position afterPos = before;
        afterPos.makeMove(pm);
        analysis::GameReview g;
        g.reset(start, moves);
        const int n = int(moves.size()) - 1;   // the position before the move
        g.accept(n, r.a0);
        g.accept(n + 1, after);
        analysis::Commentator c;
        c.reset(analysis::GameInfo());
        const analysis::Comment k = c.commentAt(g, n + 1);
        ++reviewed;
        const analysis::Verdict v = g.verdict(n);
        if (k.empty() || !v.known) continue;
        // The move's own lines only (not a forced mate announced on the position, nor its end).
        bool about = false;
        for (const Line& l : k.lines)
            if (l.key == "an.blunder" || l.key == "an.mistake" || l.key == "an.missed_chance") about = true;
        if (!about && !o.all) continue;
        ++commented;
        std::vector<Finding> fs;
        const Color mover = before.sideToMove();
        const int base = coach::materialBalance(before, mover);
        const std::vector<std::string>& reply = after.lines[0].pv;
        const int replyLoss = worstLoss(afterPos, reply, 0, 8, mover, base);
        const int moveLoss = lossAt(afterPos, mover, base);
        const int playedLoss = std::max(moveLoss, replyLoss);
        const int bestLoss = worstLoss(before, r.a0.lines[0].pv, 0, 8, mover, base);
        const int cpBest = coach::detail::whiteCp(r.a0.lines[0].score, true);
        const int cpPlayed = -coach::detail::whiteCp(after.lines[0].score, true);
        const int drop = std::max(0, cpBest - cpPlayed);
        for (const Line& l : k.lines) {
            ++keys[l.key];
            // Every placeholder has its argument; every mark of the comment is on a placeholder of a line.
            for (const std::string& vt : variantsOf(l.key))
                for (size_t i = vt.find('{'); i != std::string::npos; i = vt.find('{', i + 1)) {
                    const size_t e = vt.find_first_of(":}", i);
                    if (e == std::string::npos) break;
                    if (!l.arg(vt.substr(i + 1, e - i - 1)))
                        fs.push_back({"NO_ARG", l.key + " {" + vt.substr(i + 1, e - i - 1) + "}"});
                }
            const bool material = l.key == "an.material" || l.key == "an.fork" || l.key == "an.hanging";
            if (!material) continue;
            if (playedLoss < 1) fs.push_back({"NO_LOSS", l.key + ": the line loses nothing for good"});
            if (bestLoss >= 1 && playedLoss - bestLoss < (bestLoss == 0 ? 1 : 2))
                fs.push_back({"AVOIDABLE?", l.key + ": best line loses " + std::to_string(bestLoss) + ", played " +
                                                std::to_string(playedLoss)});
            int named = playedLoss;
            if (const Arg* pts = l.arg("pts")) {
                named = pts->number;
                Position end;
                int plies = 0;
                const Arg* line = l.arg("line");
                if (line && replaySan(afterPos, line->san, end, plies)) {
                    const int lost = shownLoss(afterPos, reply, plies, mover, base);
                    if (pts->number > lost)
                        fs.push_back({"PTS_OVERCLAIM", l.key + " says " + std::to_string(pts->number) + ", the board shows " +
                                                           std::to_string(lost)});
                }
            }
            if (named < 3 && named * 250 < drop)
                fs.push_back({"INSUFFICIENT", l.key + " names " + std::to_string(named) + " for a drop of " +
                                                  std::to_string(drop) + " cp"});
            if (l.key == "an.fork") {
                // One of the two targets is taken in the reply's first five plies.
                const Arg* t1 = l.arg("t1");
                const Arg* t2 = l.arg("t2");
                bool taken = false;
                Position q = afterPos;
                for (size_t i = 0; i < reply.size() && i < 5; ++i) {
                    const Move m = q.parseUCI(reply[i]);
                    if (!m.valid()) break;
                    if (i >= 2 && !q.at(m.to).empty() && q.at(m.to).color == mover &&
                        ((t1 && m.to == t1->square) || (t2 && m.to == t2->square)))
                        taken = true;
                    q.makeMove(m);
                }
                if (!taken) fs.push_back({"FORK_NOTHING_TAKEN", "neither target is taken"});
            }
        }
        for (const Finding& x : fs) ++codes[x.code];
        std::fprintf(out, "=== #%zu %s %s%s\n", ri, mover == White ? "W" : "B", before.fen().c_str(),
                     fs.empty() ? "" : "  ERROR");
        std::fprintf(out, "  move %s  W%% %.1f -> %.1f  best %s  drop %d cp  loss %d (best %d)\n", v.san.c_str(), v.wBest,
                     v.wPlayed, r.a0.lines[0].pv.empty() ? "-" : r.a0.lines[0].pv[0].c_str(), drop, playedLoss, bestLoss);
        std::string rl;
        for (size_t i = 0; i < reply.size() && i < 10; ++i) rl += (i ? " " : "") + reply[i];
        std::fprintf(out, "  reply line: %s\n", rl.c_str());
        for (const Line& l : k.lines) std::fprintf(out, "  SAY [%s] %s\n", l.key.c_str(), render(l).c_str());
        for (const Finding& x : fs) std::fprintf(out, "  !! %s: %s\n", x.code.c_str(), x.detail.c_str());
    }
    std::fprintf(out, "\n==== %d moves, %d commented\n  lines:\n", reviewed, commented);
    for (const auto& kv : keys) std::fprintf(out, "    %-22s %d\n", kv.first.c_str(), kv.second);
    std::fprintf(out, "  findings:\n");
    for (const auto& kv : codes) std::fprintf(out, "    %-22s %d\n", kv.first.c_str(), kv.second);
    if (out != stdout) std::fclose(out);
    return 0;
}

}  // namespace audit
