// coach_audit review: every recorded human move through the coach's Reviewer at each level, what the
// coach would say and show, and the oracles: each claim checked against the board, independently of
// the detectors that made it (they share only tactics.h's board primitives).
#include "audit.h"

#include "coach/catalog.h"
#include "coach/openings.h"
#include "coach/review_internal.h"
#include "coach/tactics.h"
#include "core/files.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace audit {

using namespace chess;
using namespace coach;

namespace {

struct Finding {
    std::string code, detail;
    bool error = true;
};

const char* className(MoveClass c) { return moveClassName(c); }

std::string pieceName(PieceType t) {
    static const char* n[7] = {"-", "pawn", "knight", "bishop", "rook", "queen", "king"};
    return n[t];
}

// English variants of a key ("ex.fork.b2", "ex.fork.b2.2", ...).
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

bool usesPlaceholder(const std::string& tmpl, const std::string& name) {
    return tmpl.find("{" + name + "}") != std::string::npos || tmpl.find("{" + name + ":") != std::string::npos;
}

// Replays a space-separated SAN line from p; false when a move does not parse.
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

// The human's material lost for good at 'end' relative to p0's balance: what the side to move could
// still win back at once is taken into account when that side is the human.
int heldLossAt(const Position& end, Color human, int base) {
    int lost = base - materialBalance(end, human);
    if (end.sideToMove() == human && end.hasLegalMove()) lost -= bestCapturePoints(end, human);
    return lost;
}

// The worst loss for good along a line played from 'start' (relative to base), at most 'plies'
// plies: after each ply, what the human, to move there, takes back at once is taken off.
int worstLoss(const std::vector<LineStep>& line, const Position& start, Color human, int base, int plies) {
    int worst = 0;
    Position q = start;
    for (int i = 0; i < int(line.size()) && i < plies; ++i) {
        q.makeMove(line[size_t(i)].move);
        worst = std::max(worst, heldLossAt(q, human, base));
    }
    return worst;
}

bool isMaterialType(ExType t) {
    return t == ExType::Fork || t == ExType::Discovered || t == ExType::Skewer || t == ExType::Pin ||
           t == ExType::Trapped || t == ExType::BackRank || t == ExType::Hanging || t == ExType::Exchange;
}
bool isGenericType(ExType t) {
    return t == ExType::Endgame || t == ExType::KingSafety || t == ExType::Positional || t == ExType::Opening ||
           t == ExType::BadTrade;
}

struct Ctx {
    const Record* rec = nullptr;
    int level = 1;
    Game g;          // the human's move played
    Position p0, p1;
    Color human = White, coach = Black;
    int base = 0;
    const ai::PvLine* lp = nullptr;   // the played move's line
    std::vector<LineStep> playedLine, bestLine;
    Position playedEnd, bestEnd;
    std::vector<LineStep> refut;   // the refutation from p1, as the Reviewer builds it (A2 from level 4)
};

// Whether the coach takes the human's piece standing on 'sq' after the first 'from' plies of the
// refutation, within 'plies' more plies (the piece followed as it moves).
bool takenLater(const Ctx& c, Square sq, int from, int plies) {
    for (int i = from; i < int(c.refut.size()) && i < from + plies; ++i) {
        const LineStep& st = c.refut[size_t(i)];
        if (st.mover == c.human && st.move.from == sq) sq = st.move.to;
        else if (st.mover == c.coach && st.move.to == sq && st.captured != NoPiece) return true;
    }
    return false;
}

std::string render(const Line& l) {
    if (l.key.empty()) return std::string();
    return Catalog::shared().renderVariant(l, "en", false, 1).text;
}

std::string argsText(const Line& l) {
    std::string s;
    for (const auto& a : l.args) {
        const Arg& x = a.second;
        if (x.kind != Arg::Kind::Piece || x.square == NoSquare) continue;
        s += " " + a.first + "=" + (x.color == White ? "w" : "b") + pieceName(x.piece) + "@" + squareName(x.square);
    }
    return s;
}

std::string marksText(const Beat& b) {
    std::string s;
    for (const Mark& m : b.marks) {
        if (m.kind == Mark::Kind::Arrow) s += " arrow:" + squareName(m.from) + squareName(m.to);
        else s += std::string(m.kind == Mark::Kind::Piece ? " piece:" : " sq:") + squareName(m.square);
    }
    return s;
}

// ---- The oracles ----------------------------------------------------------------------------------

void checkPieceArgs(const Ctx& c, const Beat& b, const Position& table, bool demoNarration,
                    std::vector<Finding>& out) {
    const bool missedFamily = b.line.key.rfind("ex.missed", 0) == 0 || b.line.key.rfind("ex.mate_missed", 0) == 0 ||
                              b.line.key.rfind("ex.promotion_missed", 0) == 0;
    for (const auto& a : b.line.args) {
        const Arg& x = a.second;
        if (x.kind != Arg::Kind::Piece) continue;
        if (x.own != (x.color == c.human))
            out.push_back({"OWN_FLAG", b.line.key + " {" + a.first + "} own flag wrong"});
        if (x.square == NoSquare) continue;
        const Piece on = table.at(x.square);
        if (on.type == x.piece && on.color == x.color) continue;
        const Piece onP0 = c.p0.at(x.square);
        const bool p0ok = onP0.type == x.piece && onP0.color == x.color;
        if (demoNarration && p0ok) continue;
        out.push_back({p0ok ? "PIECE_ON_P0" : "PIECE_MISMATCH",
                       b.line.key + " {" + a.first + "} = " + pieceName(x.piece) + " on " + squareName(x.square) +
                           ", the table has " + (on.empty() ? std::string("nothing") : pieceName(on.type)),
                       !(p0ok && missedFamily)});
    }
}

// The human's loss for good after the first 'plies' plies of the refutation: what the human, to
// move there, takes back at once is taken off, unless the line shows it never comes (the human's
// next move takes nothing, and the human is still as much down four plies later; at most what the
// board shows then).
int shownLoss(const Ctx& c, int plies) {
    Position q = c.p1;
    const int n = int(c.refut.size());
    for (int i = 0; i < plies && i < n; ++i) q.makeMove(c.refut[size_t(i)].move);
    int l = heldLossAt(q, c.human, c.base);
    const int board = c.base - materialBalance(q, c.human);
    if (plies < n && l < board && c.refut[size_t(plies)].mover == c.human && c.refut[size_t(plies)].captured == NoPiece) {
        Position later = q;
        for (int i = plies; i < plies + 4 && i < n; ++i) later.makeMove(c.refut[size_t(i)].move);
        const int held = heldLossAt(later, c.human, c.base);
        if (held > l) l = std::min(board, held);
    }
    return l;
}

void checkPts(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const Arg* pts = b.line.arg("pts");
    const Arg* line = b.line.arg("line");
    const Arg* reply = b.line.arg("reply");
    if (!pts) return;
    for (const std::string& v : variantsOf(b.line.key)) {
        if (!usesPlaceholder(v, "pts")) continue;
        if (usesPlaceholder(v, "line") && line) {
            Position end;
            int plies = 0;
            const bool fromP1 = replaySan(c.p1, line->san, end, plies);
            if (!fromP1 && !replaySan(c.p0, line->san, end, plies)) {
                out.push_back({"LINE_UNPLAYABLE", b.line.key + " {line} " + line->san});
                return;
            }
            const int lost = fromP1 ? shownLoss(c, plies) : heldLossAt(end, c.human, c.base);
            if (pts->number > lost)
                out.push_back({"PTS_OVERCLAIM", b.line.key + " says " + std::to_string(pts->number) + " after " +
                                                    line->san + ", the board shows " + std::to_string(lost)});
            return;
        }
        if (usesPlaceholder(v, "reply") && reply) {
            const Move m = c.p1.parseUCI(reply->uci);
            const int took = m.valid() && !c.p1.at(m.to).empty() ? kPiecePoints[c.p1.at(m.to).type] : 0;
            if (pts->number != took)
                out.push_back({"PTS_REPLY", b.line.key + " says " + std::to_string(pts->number) + ", " + reply->san +
                                                " takes " + std::to_string(took)});
            return;
        }
    }
}

// Every placeholder of every phrasing has its argument, and every gesture and mark is anchored on a
// placeholder each phrasing says (the director times them on it).
void checkAnchors(const Beat& b, std::vector<Finding>& out) {
    if (b.line.key.empty()) return;
    const std::vector<std::string> vs = variantsOf(b.line.key);
    if (vs.empty()) {
        out.push_back({"KEY_MISSING", b.line.key});
        return;
    }
    for (const std::string& v : vs) {
        for (size_t i = v.find('{'); i != std::string::npos; i = v.find('{', i + 1)) {
            const size_t e = v.find_first_of(":}", i);
            if (e == std::string::npos) break;
            const std::string name = v.substr(i + 1, e - i - 1);
            if (!b.line.arg(name)) out.push_back({"NO_ARG", b.line.key + " {" + name + "}"});
        }
        for (const Gesture& g : b.gestures)
            if (!g.anchor.empty() && !usesPlaceholder(v, g.anchor))
                out.push_back({"ANCHOR", b.line.key + " gesture on {" + g.anchor + "}: \"" + v + "\""});
        for (const Mark& m : b.marks)
            if (!m.anchor.empty() && !usesPlaceholder(v, m.anchor))
                out.push_back({"ANCHOR", b.line.key + " mark on {" + m.anchor + "}: \"" + v + "\""});
    }
}

// "I can win your queen" / "your rook can't be saved": the piece named is taken by the coach along
// the refutation, and not traded (the human taking back as much on that square at once).
void checkWon(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const bool claim = b.line.key.rfind("ex.material.b1", 0) == 0 || b.line.key.rfind("ex.material.b2", 0) == 0 ||
                       b.line.key == "ex.point.piece";
    const Arg* t1 = b.line.arg("t1");
    if (!claim || !t1 || t1->square == NoSquare) return;
    // Follow the piece along the line from where it stands on the table, or where the demonstration
    // stopped (ex.point.piece: its square then; found by matching the piece along the line).
    Position q = c.p1;
    Square at = NoSquare;
    for (size_t i = 0; i <= c.refut.size(); ++i) {
        if (q.at(t1->square).type == t1->piece && q.at(t1->square).color == c.human && at == NoSquare) at = t1->square;
        if (i == c.refut.size()) break;
        const LineStep& st = c.refut[i];
        if (at != NoSquare) {
            if (st.mover == c.human && st.move.from == at) at = st.move.to;
            else if (st.mover == c.coach && st.move.to == at && st.captured != NoPiece) {
                const bool traded = i + 1 < c.refut.size() && c.refut[i + 1].mover == c.human &&
                                    c.refut[i + 1].move.to == at && kPiecePoints[c.refut[i + 1].captured] >= kPiecePoints[st.captured];
                if (traded) out.push_back({"TRADED_NAMED", b.line.key + " names a " + pieceName(st.captured) + " that is traded"});
                return;
            }
        }
        q.makeMove(st.move);
    }
    out.push_back({"NAMED_NOT_TAKEN", b.line.key + " names a " + pieceName(t1->piece) + " the line never takes"});
}

void checkFree(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    const bool hanging = k.rfind("ex.hanging.", 0) == 0 || k.rfind("ex.hanging_guard", 0) == 0 ||
                         k.rfind("ex.hanging_threat", 0) == 0;
    const bool missed = k.rfind("ex.missed_capture", 0) == 0;
    if (!hanging && !missed) return;
    const Position& p = hanging ? c.p1 : c.p0;
    const Arg* target = b.line.arg(hanging ? "your" : "my");
    if (!target || target->square == NoSquare) return;
    if (!isUndefended(p, target->square))
        out.push_back({"NOT_FREE", k + ": " + squareName(target->square) + " has a defender"});
}

void checkFork(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    const bool allowed = k.rfind("ex.fork.b", 0) == 0, missed = k.rfind("ex.missed_fork", 0) == 0;
    if (!allowed && !missed) return;
    const Arg* mv = b.line.arg(allowed ? "reply" : "best");
    const Arg* t1 = b.line.arg("t1");
    const Arg* t2 = b.line.arg("t2");
    if (!mv || !t1 || !t2) return;
    Position q = allowed ? c.p1 : c.p0;
    const Move m = q.parseUCI(mv->uci);
    if (!m.valid()) {
        out.push_back({"FORK_MOVE", k + ": " + mv->uci + " not legal"});
        return;
    }
    q.makeMove(m);
    const uint64_t att = q.attacksFrom(m.to);
    for (const Arg* t : {t1, t2}) {
        if (!(att & squareBit(t->square)))
            out.push_back({"FORK_TARGET", k + ": " + squareName(m.to) + " does not attack " + squareName(t->square)});
        else if (q.at(t->square).type != t->piece)
            out.push_back({"FORK_TARGET_PIECE", k + ": " + squareName(t->square)});
    }
}

void checkMateCount(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.find("mate") == std::string::npos) return;
    const Arg* m = b.line.arg("m");
    const Arg* line = b.line.arg("line");
    if (!m || m->kind != Arg::Kind::Number) return;
    for (const std::string& v : variantsOf(k)) {
        if (!usesPlaceholder(v, "line") || !line) continue;
        Position end;
        int plies = 0;
        const bool missed = k.rfind("ex.mate_missed", 0) == 0;
        if (!replaySan(missed ? c.p0 : c.p1, line->san, end, plies)) {
            out.push_back({"MATE_LINE_UNPLAYABLE", k + " " + line->san});
            return;
        }
        if (!end.isCheckmate() || plies != 2 * m->number - 1)
            out.push_back({"MATE_LINE", k + ": mate in " + std::to_string(m->number) + " but " + line->san +
                                            (end.isCheckmate() ? " mates in " + std::to_string(plies) + " plies"
                                                               : " is no mate")});
        return;
    }
}

}  // namespace

// ---- One move at one level ---------------------------------------------------------------------------

namespace {

struct Stats {
    std::map<std::string, int> codes;
    std::map<std::string, int> types;   // voiced explanation types
    int reviews = 0, scripts = 0;
};

bool buildCtx(const Record& r, int level, Ctx& c) {
    c.rec = &r;
    c.level = level;
    if (!r.startFen.empty() && !c.g.resetFromFEN(r.startFen)) return false;
    for (const std::string& u : r.moves) {
        const Move m = c.g.position().parseUCI(u);
        if (!m.valid() || !c.g.play(m)) return false;
    }
    c.p0 = c.g.position();
    const Move m = c.p0.parseUCI(r.played);
    if (!m.valid() || !c.g.play(m)) return false;
    c.p1 = c.g.position();
    c.human = r.human;
    c.coach = opposite(r.human);
    c.base = materialBalance(c.p0, c.human);
    if (r.a0.ok && !r.a0.lines.empty()) {
        c.lp = r.a0.line(r.played);
        if ((!c.lp || (c.lp->score.bound != ai::Score::Bound::Exact && c.lp != &r.a0.lines[0])) && r.hasA1 && r.a1.ok &&
            !r.a1.lines.empty())
            c.lp = &r.a1.lines[0];
        c.bestLine = replayLine(c.p0, r.a0.lines[0].pv, c.human, 16);
        c.bestEnd = detail::lineEnd(c.p0, c.bestLine);
        if (c.lp) {
            c.playedLine = replayLine(c.p0, c.lp->pv, c.human, 16);
            c.playedEnd = detail::lineEnd(c.p0, c.playedLine);
            std::vector<std::string> refutation(c.lp->pv.begin() + (c.lp->pv.empty() ? 0 : 1), c.lp->pv.end());
            if (level >= 4 && r.hasA2 && r.a2.ok && !r.a2.lines.empty()) {
                const std::vector<std::string>& pv = r.a2.lines[0].pv;
                if (pv.size() > refutation.size() && (refutation.empty() || pv[0] == refutation[0])) refutation = pv;
            }
            c.refut = replayLine(c.p1, refutation, c.human, 16);
        }
    }
    return true;
}

}  // namespace

int reviewMain(const Options& o) {
    std::ifstream in(o.in, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::vector<Record> recs;
    std::string err;
    if (!readRecords(ss.str(), recs, err)) {
        std::fprintf(stderr, "%s: %s\n", o.in.c_str(), err.c_str());
        return 1;
    }
    Catalog::shared().load();
    FILE* out = o.out.empty() ? stdout : files::create(o.out.c_str());   // mode 0644, whatever the umask
    if (!out) return 1;
    Stats st;
    for (size_t ri = 0; ri < recs.size(); ++ri) {
        const Record& r = recs[ri];
        for (int level = 1; level <= 6; ++level) {
            if (o.level && level != o.level) continue;
            Ctx c;
            if (!buildCtx(r, level, c)) continue;
            Reviewer rv;
            rv.reset(level, c.human);
            ReviewInput ri2;
            ri2.game = &c.g;
            ri2.before = &r.a0;
            ri2.played = r.hasA1 ? &r.a1 : nullptr;
            ri2.after = level >= 4 && r.hasA2 ? &r.a2 : nullptr;
            ri2.shallow = level == 3 || level == 4 ? &r.a3 : nullptr;
            ri2.inBook = OpeningBook::instance().lookup(c.p1.hash());
            const Review rev = rv.review(ri2);
            ++st.reviews;
            const PlyVerdict& v = rev.verdict;
            std::vector<Finding> f;

            // Simulate the script on the table.
            Position table = c.p1;
            std::vector<Position> stack;
            std::vector<const Beat*> sinceDemo;
            bool demo = false, offer = false;
            std::string verdictKey;
            Position demoEnd;
            std::vector<const Beat*> tail;
            bool sawEnd = false;
            int demoPlies = 0;
            for (const Beat& b : rev.script) {
                checkAnchors(b, f);
                switch (b.kind) {
                case BeatKind::Say:
                case BeatKind::OfferTakeback:
                    checkPieceArgs(c, b, table, false, f);
                    checkPts(c, b, f);
                    checkWon(c, b, f);
                    checkFree(c, b, f);
                    checkFork(c, b, f);
                    checkMateCount(c, b, f);
                    if (b.line.key.rfind("ex.verdict.", 0) == 0) verdictKey = b.line.key;
                    if (b.line.key.rfind("ex.result.", 0) == 0 || b.line.key.rfind("ex.result_back.", 0) == 0) {
                        const Arg* pts = b.line.arg("pts");
                        const int lost = shownLoss(c, demoPlies);
                        if (pts && pts->number > lost)
                            f.push_back({"RESULT_PTS", std::to_string(pts->number) + " said, the table shows " +
                                                           std::to_string(lost)});
                    }
                    if (b.kind == BeatKind::OfferTakeback) offer = true;
                    if (!stack.empty()) sinceDemo.push_back(&b);
                    break;
                case BeatKind::DemoMove: {
                    checkPieceArgs(c, b, table, true, f);
                    const Move m = table.parseUCI(b.uci);
                    if (!m.valid()) {
                        f.push_back({"ILLEGAL_DEMO", b.uci});
                        break;
                    }
                    stack.push_back(table);
                    table.makeMove(m);
                    demo = true;
                    if (!sawEnd) ++demoPlies;
                    sinceDemo.clear();
                    break;
                }
                case BeatKind::Rewind:
                    if (!sawEnd) {
                        demoEnd = table;
                        tail = sinceDemo;
                        sawEnd = true;
                    }
                    if (int(stack.size()) < b.count) f.push_back({"REWIND_COUNT", std::to_string(b.count)});
                    for (int i = 0; i < b.count && !stack.empty(); ++i) {
                        table = stack.back();
                        stack.pop_back();
                    }
                    sinceDemo.clear();
                    break;
                default: break;
                }
            }
            if (!stack.empty()) f.push_back({"NO_REWIND", std::to_string(stack.size())});

            const bool fault = v.cls == MoveClass::Inaccuracy || v.cls == MoveClass::Mistake || v.cls == MoveClass::Blunder;
            // A reason said: a line of the explanation's own (not only a verdict, the better move, the
            // offer or the rewind).
            bool causeSaid = false;
            for (const Beat& b : rev.script) {
                const std::string& k = b.line.key;
                if (b.kind == BeatKind::Say && k.rfind("ex.", 0) == 0 && k.rfind("ex.verdict.", 0) != 0 &&
                    k.rfind("ex.better.", 0) != 0 && k.rfind("ex.point.", 0) != 0 && k.rfind("ex.result.", 0) != 0)
                    causeSaid = true;
            }
            const bool explained = v.voiced && v.exType != ExType::None && causeSaid && !v.praised;
            const ExType t = v.exType;

            // The demonstration shows (or, from level 4, points at) the problem it announces.
            if (explained && demo && (isMaterialType(t) || t == ExType::MateAllowed)) {
                const int lost = shownLoss(c, demoPlies);
                const bool mate = demoEnd.isCheckmate();
                // Pointed at: a piece of the human's the coach wins next (at once, or along the line
                // within 3 moves), or the king about to be mated.
                bool pointed = false;
                for (const Beat* b : tail)
                    for (const Mark& m : b->marks) {
                        const Square s = m.kind == Mark::Kind::Arrow ? m.to : m.square;
                        if (s == NoSquare) continue;
                        const Piece pc = demoEnd.at(s);
                        if (pc.empty() || pc.color != c.human) continue;
                        if (pc.type != King && demoEnd.sideToMove() == c.coach && seeSquarePoints(demoEnd, s, c.coach) >= 1)
                            pointed = true;
                        if (pc.type != King && takenLater(c, s, demoPlies, 6)) pointed = true;
                    }
                // The mating move traced: the next move of the line mates.
                for (const Beat* b : tail)
                    if (b->line.key.rfind("ex.point.mate", 0) == 0 && demoPlies < int(c.refut.size()) &&
                        c.refut[size_t(demoPlies)].mate)
                        pointed = true;
                const bool shown = t == ExType::MateAllowed ? (mate || pointed) : (lost >= 1 || mate || pointed);
                if (!shown)
                    f.push_back({"DEMO_SHORT", std::string(exTypeName(t)) + ": demo ends with the human " +
                                                   std::to_string(lost) + " down, no mate, nothing pointed at"});
            }
            if (explained && !demo && level <= 2 && (isMaterialType(t) || t == ExType::MateAllowed) && fault)
                f.push_back({"NO_DEMO_BEGINNER", exTypeName(t), false});

            // Policy: small slips are not dramatised.
            if (explained && v.cls == MoveClass::Inaccuracy && demo &&
                worstLoss(c.playedLine, c.p0, c.human, c.base, 9) < 3)
                f.push_back({"INACCURACY_DEMO", exTypeName(t)});
            if (offer && v.cls == MoveClass::Inaccuracy && !(level <= 2 && t == ExType::MissedCapture))
                f.push_back({"OFFER_INACCURACY", exTypeName(t)});
            if (offer && v.cls != MoveClass::Blunder && t != ExType::MateAllowed && t != ExType::MateMissed &&
                t != ExType::Stalemate && t != ExType::MissedCapture)
                f.push_back({"OFFER_NOT_BLUNDER", className(v.cls)});
            if (offer && v.wPlayed >= 75.0) f.push_back({"OFFER_STILL_WINNING", std::to_string(int(v.wPlayed)), false});
            if (verdictKey.rfind("ex.verdict.blunder", 0) == 0 && v.cls != MoveClass::Blunder)
                f.push_back({"VERDICT_CLASS", verdictKey + " for " + className(v.cls)});
            if (!verdictKey.empty() && !demo)
                for (const std::string& vt : variantsOf(verdictKey))
                    if (vt.find("Look.") != std::string::npos || vt.find("Look:") != std::string::npos ||
                        vt.find("Watch") != std::string::npos || vt.find("show you") != std::string::npos ||
                        vt.find("Here's why") != std::string::npos) {
                        f.push_back({"VERDICT_PROMISES_DEMO", verdictKey});
                        break;
                    }

            // The loss the explanation names is one the best move avoids.
            if (explained && fault && (isMaterialType(t) || isGenericType(t)) && c.lp) {
                const int bestLoss = worstLoss(c.bestLine, c.p0, c.human, c.base, 8);
                const int refLoss = worstLoss(c.playedLine, c.p0, c.human, c.base, 9);
                if (isMaterialType(t) && bestLoss >= 2 && refLoss - bestLoss < 2)
                    f.push_back({"AVOIDABLE?", "best line loses " + std::to_string(bestLoss) + ", played line " +
                                                   std::to_string(refLoss)});
                bool mated = false;
                for (const LineStep& s : c.playedLine)
                    if (s.mate && s.mover == c.coach) mated = true;
                // King safety is about mates: only a material loss makes it the wrong reason.
                if (isGenericType(t) && (v.cls != MoveClass::Inaccuracy) &&
                    (refLoss - bestLoss >= 2 || (mated && t != ExType::KingSafety)))
                    f.push_back({"GENERIC_REASON", std::string(exTypeName(t)) + " while the played line loses " +
                                                       std::to_string(refLoss) + (mated ? " (mate)" : "")});
            }

            // A material reason that names much less than the engine's drop understates the problem.
            if (explained && fault && isMaterialType(t) && c.lp) {
                const int sign = c.human == White ? 1 : -1;
                const int drop = sign * (v.cpWhiteBefore - v.cpWhiteAfter);
                int named = 0;
                if (demo) named = shownLoss(c, demoPlies);
                for (const Beat& b : rev.script)
                    if (const Arg* p = b.line.arg("pts")) named = std::max(named, p->number);
                if (named < 3 && named * 250 < drop)
                    f.push_back({"INSUFFICIENT", "names " + std::to_string(named) + " points for a drop of " +
                                                     std::to_string(drop) + " cp"});
            }

            if (!rev.script.empty()) ++st.scripts;
            if (explained) ++st.types[exTypeName(t)];
            bool anyError = false;
            for (const Finding& x : f) {
                ++st.codes[x.code];
                anyError = anyError || x.error;
            }
            if (rev.script.empty() && !o.all && f.empty()) continue;

            // The transcript.
            std::fprintf(out, "=== #%zu L%d %s %s  %s\n", ri, level, c.human == White ? "W" : "B",
                         c.p0.fen().c_str(), anyError ? "ERROR" : (f.empty() ? "" : "warn"));
            std::fprintf(out, "  move %s  class %s  W%% %.1f -> %.1f (d %.1f)  best %s  ex %s%s%s\n", v.san.c_str(),
                         className(v.cls), v.wBest, v.wPlayed, v.delta, v.bestSan.c_str(), exTypeName(t),
                         v.voiced ? " voiced" : "", v.offered ? " offer" : "");
            std::fprintf(out, "  best line: %s\n", sanLine(c.bestLine, 0, std::min<size_t>(10, c.bestLine.size())).c_str());
            std::fprintf(out, "  played line: %s\n",
                         sanLine(c.playedLine, 0, std::min<size_t>(10, c.playedLine.size())).c_str());
            for (const Beat& b : rev.script) {
                switch (b.kind) {
                case BeatKind::Say:
                    std::fprintf(out, "  SAY  [%s] %s%s%s\n", b.line.key.c_str(), render(b.line).c_str(),
                                 argsText(b.line).c_str(), marksText(b).c_str());
                    break;
                case BeatKind::DemoMove:
                    std::fprintf(out, "  DEMO %s  [%s] %s\n", b.uci.c_str(), b.line.key.c_str(), render(b.line).c_str());
                    break;
                case BeatKind::Rewind:
                    std::fprintf(out, "  BACK %d  [%s] %s\n", b.count, b.line.key.c_str(), render(b.line).c_str());
                    break;
                case BeatKind::OfferTakeback:
                    std::fprintf(out, "  OFFER [%s] %s\n", b.line.key.c_str(), render(b.line).c_str());
                    break;
                default: break;
                }
            }
            for (const Finding& x : f)
                std::fprintf(out, "  %s %s: %s\n", x.error ? "!!" : "..", x.code.c_str(), x.detail.c_str());
        }
    }
    std::fprintf(out, "\n==== %d reviews, %d with a script\n", st.reviews, st.scripts);
    for (const auto& k : st.codes) std::fprintf(out, "  %-24s %d\n", k.first.c_str(), k.second);
    std::fprintf(out, "  voiced explanations:\n");
    for (const auto& k : st.types) std::fprintf(out, "    %-16s %d\n", k.first.c_str(), k.second);
    if (out != stdout) std::fclose(out);
    return 0;
}

}  // namespace audit
