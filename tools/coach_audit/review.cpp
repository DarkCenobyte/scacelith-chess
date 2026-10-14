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
#include <memory>
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

// The human's loss after the first 'plies' plies of a line played from 'start' (relative to
// 'base'): a capture there that the other side takes back on the next ply is a whole exchange, and
// so are the captures on that square that follow it at once (Bxa6 ... Rxa6 Rxa6 Nxa6: the exchange
// read where it ends); where the line stops with the human to move, what the human takes back at
// once is taken off.
int lossAt(const std::vector<LineStep>& line, const Position& start, Color human, int base, size_t plies) {
    while (plies > 0 && plies < line.size() && line[plies - 1].captured != NoPiece &&
           line[plies].captured != NoPiece && line[plies].move.to == line[plies - 1].move.to)
        ++plies;
    Position q = start;
    for (size_t i = 0; i < plies && i < line.size(); ++i) q.makeMove(line[i].move);
    return plies >= line.size() || plies == 0 ? heldLossAt(q, human, base) : base - materialBalance(q, human);
}

// Where a line settles after its first 'plies' plies: the least loss from there to four plies
// later (what the human wins back meanwhile, an in-between check first, was no loss).
int settledLoss(const std::vector<LineStep>& line, const Position& start, Color human, int base, size_t plies) {
    plies = std::min(plies, line.size());
    int l = lossAt(line, start, human, base, plies);
    for (size_t j = plies + 1; j <= std::min(line.size(), plies + 4); ++j) l = std::min(l, lossAt(line, start, human, base, j));
    return l;
}

// The most the human is down for good along the first 'plies' plies of a line, a loss counted only
// where it still holds (settledLoss) four plies later.
int lastingLoss(const std::vector<LineStep>& line, const Position& start, Color human, int base, size_t plies) {
    int worst = 0;
    for (size_t i = 1; i <= std::min(plies, line.size()); ++i) worst = std::max(worst, settledLoss(line, start, human, base, i));
    return worst;
}

// What the human is down where a line ends: with the human to move, what the human takes at once
// taken off (lossAt); else the worse of the last two plies (the human's capture the line stops on
// counts as not made, the take-back not shown).
int endLoss(const std::vector<LineStep>& line, const Position& start, Color human, int base) {
    if (line.empty()) return 0;
    const int atEnd = lossAt(line, start, human, base, line.size());
    return line.back().mover != human ? atEnd : std::max(atEnd, lossAt(line, start, human, base, line.size() - 1));
}

// The points of the human's piece that stands on 'onP0' before the move that the best line gives up
// too: the other side takes it within ten plies (the piece followed as it moves) and the captures on
// that square right after do not win as much back; what the piece took on its way is taken off. 0
// when it survives or is traded.
int lostAnyway(const std::vector<LineStep>& best, const Position& p0, Color human, Square onP0) {
    const Piece was = p0.at(onP0);
    if (was.empty() || was.color != human) return 0;
    Square at = onP0;
    int took = 0;
    for (size_t i = 0; i < best.size() && i < 10; ++i) {
        const LineStep& st = best[i];
        if (st.mover == human) {
            if (st.move.from == at) {
                at = st.move.to;
                took += kPiecePoints[st.captured];
            } else if (st.piece == King && (st.move.flags & (MoveCastleKing | MoveCastleQueen))) {
                const int rank = rankOf(st.move.to);
                const bool kingSide = fileOf(st.move.to) == 6;
                if (at == makeSquare(kingSide ? 7 : 0, rank)) at = makeSquare(kingSide ? 5 : 3, rank);
            }
            continue;
        }
        const Square on = (st.move.flags & MoveEnPassant) ? Square(st.move.to + (human == White ? 8 : -8)) : st.move.to;
        if (st.captured == NoPiece || on != at) continue;
        int net = kPiecePoints[st.captured];
        for (size_t j = i + 1, last = i; j < best.size() && j <= last + 3; ++j) {
            if (best[j].captured == NoPiece || best[j].move.to != st.move.to) continue;
            net += best[j].mover == human ? -kPiecePoints[best[j].captured] : kPiecePoints[best[j].captured];
            last = j;
        }
        return std::max(0, std::min(net, int(kPiecePoints[st.captured])) - took);
    }
    return 0;
}

bool isMaterialType(ExType t) {
    return t == ExType::Fork || t == ExType::Discovered || t == ExType::Skewer || t == ExType::Pin ||
           t == ExType::Trapped || t == ExType::BackRank || t == ExType::Hanging || t == ExType::Exchange ||
           t == ExType::Material;
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
                              b.line.key.rfind("ex.promotion_missed", 0) == 0 || b.line.key.rfind("ex.reveal", 0) == 0;
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
// the refutation, and not traded (the human taking back as much on that square at once, or having
// taken as much there with it just before; a pawn that promotes as it captures is a pawn taken
// back). 'from': the plies of the refutation shown before the line is said (ex.point.piece names
// the piece where it stands then).
void checkWon(const Ctx& c, const Beat& b, size_t from, std::vector<Finding>& out) {
    const bool claim = b.line.key.rfind("ex.material.b1", 0) == 0 || b.line.key.rfind("ex.material.b2", 0) == 0 ||
                       b.line.key == "ex.point.piece";
    const Arg* t1 = b.line.arg("t1");
    if (!claim || !t1 || t1->square == NoSquare) return;
    // "Can't be saved", the human to move: the line still costs the human, for good, from where it
    // is said (Nxe5 dxe5 Bxb5 costs nothing more from Nxe5 on: the bishop could move away).
    Position said = c.p1;
    for (size_t i = 0; i < from && i < c.refut.size(); ++i) said.makeMove(c.refut[i].move);
    if (b.line.key == "ex.point.piece" && said.sideToMove() == c.human) {
        const int then = c.base - materialBalance(said, c.human);
        int worst = then;
        for (size_t n = from + 1; n <= std::min(c.refut.size(), from + 6); ++n)
            worst = std::max(worst, settledLoss(c.refut, c.p1, c.human, c.base, n));
        if (worst - then < std::min(2, kPiecePoints[t1->piece]))
            out.push_back({"POINT_NO_LOSS", b.line.key + ": the line costs " + std::to_string(worst - then) +
                                                " more from there, the " + pieceName(t1->piece) + " could be saved"});
    }
    // Follow the piece along the line from where it stands on the table, or where the demonstration
    // stopped (ex.point.piece: its square then; found by matching the piece along the line).
    if (b.line.key != "ex.point.piece") from = 0;
    Position q = c.p1;
    Square at = NoSquare;
    for (size_t i = 0; i <= c.refut.size(); ++i) {
        if (i >= from && q.at(t1->square).type == t1->piece && q.at(t1->square).color == c.human && at == NoSquare)
            at = t1->square;
        if (i == c.refut.size()) break;
        const LineStep& st = c.refut[i];
        if (at != NoSquare) {
            if (st.mover == c.human && st.move.from == at) at = st.move.to;
            else if (st.mover == c.coach && st.captured != NoPiece &&
                     ((st.move.flags & MoveEnPassant) ? Square(st.move.to + (c.human == White ? 8 : -8)) : st.move.to) == at) {
                const int back = st.promotion != NoPiece ? kPiecePoints[Pawn]
                                 : i + 1 < c.refut.size() ? kPiecePoints[c.refut[i + 1].captured] : 0;
                // Or what the piece itself took there just before (Rdxd7+ Rxd7 Rxd7+: a rook for a rook),
                // unless it only took back as much as the coach had taken there (Rxe1+ Nxe1 Qxe1+: the
                // rooks traded, the knight lost).
                const auto tookAt = [&](size_t j, Color who) {
                    return j < c.refut.size() && c.refut[j].mover == who && c.refut[j].captured != NoPiece && c.refut[j].move.to == at
                               ? kPiecePoints[c.refut[j].captured] : 0;
                };
                const bool backThere = i + 1 < c.refut.size() && c.refut[i + 1].mover == c.human && c.refut[i + 1].move.to == at;
                int took = i >= 1 ? tookAt(i - 1, c.human) : 0;
                if (took > 0 && i >= 2 && tookAt(i - 2, c.coach) >= took) took = 0;
                // What the human takes back, less what the coach takes back then (Qxb4+ Qxb4 Bxb4+).
                const int kept = backThere ? std::max(0, back - tookAt(i + 2, c.coach)) : 0;
                const bool traded = kept + took >= kPiecePoints[st.captured];
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
    if (!missed) return;
    // Taken along the played line all the same (after a check or a trade): nothing was missed.
    for (size_t i = 1; i < c.refut.size() && i <= 5; i += 2) {
        if (c.refut[i - 1].move.from == target->square) break;
        if (c.refut[i].mover == c.human && c.refut[i].move.to == target->square && c.refut[i].captured == target->piece) {
            out.push_back({"MISSED_TAKEN", k + ": " + squareName(target->square) + " is taken along the line all the same"});
            break;
        }
    }
    // Free means kept: the better line still holds the piece's worth after the reply (an exchange
    // there read where it ends), and the move played does not win as much along its own line. A
    // piece within a point of its worth, a pawn whole.
    const int worth = std::max(1, kPiecePoints[target->piece] - 1);
    if (-lossAt(c.bestLine, c.p0, c.human, c.base, 2) < worth)
        out.push_back({"MISSED_NOT_FREE", k + ": the reply to the capture takes material back at once"});
    // Won and kept through the next replies (Bxd6 Nxf3+ Qxf3 Qxd6 wins nothing in the end).
    for (size_t n = 1; n <= 3; n += 2) {
        int kept = -lossAt(c.playedLine, c.p0, c.human, c.base, n);
        for (size_t j = n + 1; j <= std::min(c.playedLine.size(), n + 3); ++j)
            kept = std::min(kept, -lossAt(c.playedLine, c.p0, c.human, c.base, j));
        if (kept >= worth) {
            out.push_back({"MISSED_GAINED", k + ": the move played wins as much along its line"});
            break;
        }
    }
}

// A trapped piece: the reply does not take one of its attackers (a second attacker then wins it, a
// fork rather than a trap).
void checkTrapped(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("ex.trapped.", 0) != 0 || k.rfind("ex.trapped.tip", 0) == 0 || c.refut.size() < 2) return;
    const Arg* target = b.line.arg("your");
    if (!target || target->square == NoSquare) return;
    Position p2 = c.p1;
    p2.makeMove(c.refut[0].move);
    const LineStep& reply = c.refut[1];
    if (reply.mover == c.human && reply.captured != NoPiece &&
        (p2.attackersTo(target->square, c.coach) & squareBit(reply.move.to)))
        out.push_back({"TRAPPED_ATTACKER_TAKEN", k + ": the reply takes a piece attacking " + squareName(target->square)});
}

// A sacrifice praised: the move gives up material by exchange and the reply takes it, or the player is
// still 2 points down after the reply and their own next move along the played line (a capture won
// straight back is none, nor a unit taken that the coach could win before the move).
void checkSacrifice(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("praise.sacrifice", 0) != 0 && k.rfind("praise.brilliant", 0) != 0) return;
    const Move m = c.p0.parseUCI(c.rec->played);
    // Taken, and won back at once elsewhere by the player's next move: a piece lost anyway, no
    // sacrifice (Bxf7+ Kxf7 hxg3, the bishop on d5 hung to the queen).
    if (m.valid() && c.playedLine.size() >= 3 && c.playedLine[1].captured != NoPiece && c.playedLine[1].move.to == m.to &&
        c.playedLine[2].captured != NoPiece && c.playedLine[2].move.to != m.to && c.playedLine[2].balance >= c.base - 1) {
        out.push_back({"SACRIFICE_WON_BACK", k + ": " + c.playedLine[2].san + " wins it back at once"});
        return;
    }
    // Given up by exchange: a legal capture there wins the coach two points more than the move took,
    // and the reply makes it (Nxf2 left alone, Kxf2 Rxd2+ winning the bishop back, is none).
    if (m.valid()) {
        int back = 0;
        for (const Move& r : c.p1.legalMoves())
            if (r.to == m.to && (r.flags & MoveCapture)) back = std::max(back, seePoints(c.p1, r));
        const Piece took = c.p0.at(m.to);
        const bool accepted = c.playedLine.size() >= 2 && c.playedLine[1].move.to == m.to && c.playedLine[1].captured != NoPiece;
        if (back - (took.empty() ? 0 : kPiecePoints[took.type]) >= 2 && accepted) return;
    }
    const size_t n = c.playedLine.size();
    const bool down = n >= 3 ? c.playedLine[2].balance <= c.base - 2 : n == 2 && c.playedLine[1].balance <= c.base - 2;
    if (!down) {
        out.push_back({"SACRIFICE_NONE", k + ": nothing is given up after the player's next move"});
        return;
    }
    // The reply takes elsewhere a unit the coach could already win before the move, the turn passed:
    // a loss the move did not cause (Ne2 Bxh1 Rxh1, the rook trapped on h1).
    if (m.valid() && n >= 2 && c.playedLine[1].captured != NoPiece && c.playedLine[1].move.to != m.to) {
        Position q = c.p0;
        if (q.passTurn())
            for (const Move& x : q.legalMoves())
                if (x.to == c.playedLine[1].move.to && (x.flags & MoveCapture) && seePoints(q, x) > 0) {
                    out.push_back({"SACRIFICE_HUNG_BEFORE", k + ": " + squareName(x.to) + " was en prise before the move"});
                    break;
                }
    }
}

// "You saved your piece": a piece, not a pawn, that the coach could win where it stood, and nothing
// of the player's left en prise once it moved (Rb1 out of the queen's reach while the queen on b3
// still hangs to a rook).
void checkSaved(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("praise.saved", 0) != 0) return;
    const uint64_t left = hangingPieces(c.p1, c.human, false);
    if (left) out.push_back({"SAVED_OTHER_HANGS", k + ": " + squareName(Square(__builtin_ctzll(left))) + " still hangs"});
    const Move m = c.p0.parseUCI(c.rec->played);
    if (!m.valid()) return;
    const PieceType t = c.p0.at(m.from).type;
    if (t == Pawn) out.push_back({"SAVED_PAWN", k + ": the unit on " + squareName(m.from) + " is a pawn"});
    // Attacked where it stood: with the turn passed, a legal capture there wins material.
    Position q = c.p0;
    if (q.passTurn()) {
        int win = 0;
        for (const Move& x : q.legalMoves())
            if (x.to == m.from && (x.flags & MoveCapture)) win = std::max(win, seePoints(q, x));
        if (win <= 0) out.push_back({"SAVED_NOT_ATTACKED", k + ": nothing wins the " + pieceName(t) + " on " + squareName(m.from)});
    }
}

// "Nice fork": the move attacks two of the coach's units or more, and the player's next move along
// the played line takes one of them, not a pawn (Nd3+ Qxd3 Qxd3 wins the queen that took the knight,
// a decoy; Rxc7+ Rd7 exd7 the rook put in the way).
void checkPraiseFork(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("praise.fork", 0) != 0) return;
    const Move m = c.p0.parseUCI(c.rec->played);
    if (!m.valid()) return;
    const uint64_t hit = c.p1.attacksFrom(m.to) & c.p1.pieces(c.coach);
    if (__builtin_popcountll(hit) < 2) out.push_back({"FORK_PRAISE_ONE_TARGET", k + ": " + squareName(m.to) + " attacks one unit"});
    const bool taken = c.playedLine.size() >= 3 && c.playedLine[2].mover == c.human && c.playedLine[2].captured != NoPiece &&
                       (hit & squareBit(c.playedLine[2].move.to));
    if (!taken) out.push_back({"FORK_PRAISE_NOT_WON", k + ": the next move takes no unit the move attacks"});
    else if (c.playedLine[2].captured == Pawn) out.push_back({"FORK_PRAISE_PAWN", k + ": the unit taken is a pawn"});
}

// "You took my knight for free" / "it was loose": the move takes a unit, and the coach has no legal
// capture back on that square.
void checkPraiseCapture(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("praise.capture", 0) != 0) return;
    const Move m = c.p0.parseUCI(c.rec->played);
    if (!m.valid()) return;
    if (c.p0.at(m.to).empty()) {
        out.push_back({"CAPTURE_NONE", k + ": " + c.rec->played + " takes nothing"});
        return;
    }
    for (const Move& x : c.p1.legalMoves())
        if (x.to == m.to && (x.flags & MoveCapture)) {
            out.push_back({"CAPTURE_TAKEN_BACK", k + ": " + squareName(x.from) + " takes back on " + squareName(m.to)});
            break;
        }
}

// "Good trade: you're ahead": the player is 2 points up or more before the move, and the move takes a
// unit the coach can take back (a trade, not a piece won).
void checkPraiseTrade(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("praise.trade", 0) != 0) return;
    const Move m = c.p0.parseUCI(c.rec->played);
    if (!m.valid()) return;
    if (c.base < 2) out.push_back({"TRADE_NOT_AHEAD", k + ": the balance is " + std::to_string(c.base)});
    if (c.p0.at(m.to).empty()) {
        out.push_back({"TRADE_NO_CAPTURE", k + ": " + c.rec->played + " takes nothing"});
        return;
    }
    bool back = false;
    for (const Move& x : c.p1.legalMoves()) back = back || (x.to == m.to && (x.flags & MoveCapture));
    if (!back) out.push_back({"TRADE_NOT_TAKEN_BACK", k + ": nothing takes back on " + squareName(m.to)});
}

// Praise in a lost position (best line at 10 % or less) only for the move the lines prefer: the win
// chances no longer tell the other moves apart.
void checkPraiseLost(const Ctx& c, const PlyVerdict& v, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("praise.", 0) != 0 || v.wBest > 10.0 || c.bestLine.empty()) return;
    if (c.bestLine[0].move != c.p0.parseUCI(c.rec->played))
        out.push_back({"PRAISE_LOST_NOT_BEST", k + " at " + std::to_string(int(v.wBest)) + " %"});
}

// A line that is only the move named beside it ("Missed mate: Qxa7#, starting with Qxa7#"); a mate
// in one said by the lines made for longer mates ("You had mate in 1 move, starting with Qxa7#").
void checkLineRepeats(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const Arg* m = b.line.arg("m");
    if (b.line.key.rfind("ex.mate_missed.b", 0) == 0 && b.line.key != "ex.mate_missed.b1" && m && m->number == 1)
        out.push_back({"MATE_ONE_WORDING", b.line.key});
    const Arg* ln = b.line.arg("line");
    if (!ln || ln->kind != Arg::Kind::Moves || ln->san.find(' ') != std::string::npos) return;
    // The idea after the better move holds the player's next move, not my reply alone.
    if (b.line.key.rfind("ex.better.b5", 0) == 0) {
        if (!ln->san.empty()) out.push_back({"IDEA_ONE_MOVE", b.line.key + ": " + ln->san});
        return;
    }
    auto bare = [](std::string t) {
        while (!t.empty() && (t.back() == '+' || t.back() == '#')) t.pop_back();
        return t;
    };
    // The better move, the same move from the same position (a line that starts after the move
    // played may name another move the same way: Bxh3 answered by Bxh3).
    const Arg* mv = b.line.arg("best");
    if (!mv || bare(mv->san) != bare(ln->san) || c.p0.parseSAN(ln->san) != c.p0.parseUCI(mv->uci)) return;
    for (const std::string& v : variantsOf(b.line.key))
        if (usesPlaceholder(v, "line") && usesPlaceholder(v, "best")) {
            out.push_back({"LINE_REPEATS_MOVE", b.line.key + ": " + ln->san});
            return;
        }
}

// The king said exposed or weakened: the coach checks it within its first three moves of the
// refutation, and a line said holds such a check (the lines start with the coach's reply, so its
// moves are the even ones).
void checkKingLine(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("ex.king_exposed.", 0) != 0 && k.rfind("ex.king_shield.", 0) != 0) return;
    bool checked = false;
    for (size_t i = 0; i < c.refut.size() && i < 6; ++i) checked = checked || (c.refut[i].mover == c.coach && c.refut[i].check);
    if (!checked) out.push_back({"KING_NO_CHECK", k});
    const Arg* ln = b.line.arg("line");
    if (!ln) return;
    bool lineUsed = false;
    for (const std::string& v : variantsOf(k)) lineUsed = lineUsed || usesPlaceholder(v, "line");
    if (!lineUsed) return;
    // The line said wins none of the human's pieces (not taken back as much at once): a queen it
    // takes is the story, not the king (Qh6 Rxg2+ Bxg2 Bxh6, the rook uncovering the bishop).
    {
        std::istringstream said(ln->san);
        std::vector<std::string> sans;
        for (std::string w; said >> w;) sans.push_back(w);
        Position q = c.p1;
        for (size_t i = 0; i < sans.size(); ++i) {
            const Move m = q.parseSAN(sans[i]);
            if (!m.valid()) break;
            const Piece taken = q.at(m.to);
            const bool coachMove = q.sideToMove() == c.coach;
            q.makeMove(m);
            if (!coachMove || taken.empty() || taken.type == Pawn) continue;
            // The take-back may come just after the line said ends, as the refutation goes on.
            int back = 0;
            Move r;
            if (i + 1 < sans.size()) r = q.parseSAN(sans[i + 1]);
            else if (i + 1 < c.refut.size() && c.refut[i].move == m) r = c.refut[i + 1].move;
            if (r.valid() && r.to == m.to) back = kPiecePoints[q.at(r.to).type];
            if (kPiecePoints[taken.type] - back >= 2)
                out.push_back({"KING_LINE_WINS_PIECE", k + ": " + ln->san + " takes a " + pieceName(taken.type)});
        }
    }
    std::istringstream in(ln->san);
    std::string san;
    for (int i = 0; in >> san; ++i)
        if (i % 2 == 0 && (san.back() == '+' || san.back() == '#')) return;
    out.push_back({"KING_LINE_NO_CHECK", k + ": " + ln->san});
}

// The human's piece on 'sq' stands safe in 'p', the human to move: passing the turn, the coach has no
// legal capture of it that wins anything (false when the human is in check).
bool safeWhereItStands(const Position& p, Square sq) {
    Position t = p;
    if (t.inCheck() || !t.passTurn()) return false;
    for (const Move& m : t.legalMoves())
        if (m.to == sq && !t.at(m.to).empty() && seePoints(t, m) > 0) return false;
    return true;
}

// A loss said that the line gives away by its own choice: the human moves a piece from where it
// stood safe to a square the coach takes it on at once, not as a recapture there nor a trade, where
// the engine's score after the move is a pawn better than the material then, or with check while
// mated anyway (d5 Nb8 Bg5 Nf6 Nxe5 Qxe5: the knight stood safe on f3, +1.7 two points down, and
// the pinned knight on f6 falls later). When that pair is what brings the loss to the amount said,
// the amount must be lost before it. The material line said ({line} from the table), or its
// demonstration (ex.result).
void checkGivenAway(const Ctx& c, const PlyVerdict& v, const Beat& b, int demoPlies, std::vector<Finding>& out) {
    const Arg* pts = b.line.arg("pts");
    if (!pts || pts->number <= 0) return;
    const std::string& k = b.line.key;
    const bool result = k.rfind("ex.result", 0) == 0;
    if (k.rfind("ex.material", 0) != 0 && !(result && v.exType == ExType::Material)) return;
    int plies = 0;
    if (const Arg* ln = b.line.arg("line")) {
        Position end;
        if (!replaySan(c.p1, ln->san, end, plies)) return;
    } else if (result) {
        plies = demoPlies;
    } else {
        return;
    }
    Position q = c.p1;
    for (int i = 0; i + 1 < plies && i + 1 < int(c.refut.size()); ++i) {
        const LineStep& st = c.refut[size_t(i)];
        const LineStep& next = c.refut[size_t(i + 1)];
        const bool recapture = i >= 1 && c.refut[size_t(i - 1)].captured != NoPiece && c.refut[size_t(i - 1)].move.to == st.move.to;
        // What the move gives: the piece, less what it takes there and what the human takes back
        // there at once (Bg5 Bxg5 hxg5 is a trade).
        const bool backThere = i + 2 < int(c.refut.size()) && c.refut[size_t(i + 2)].mover == c.human &&
                               c.refut[size_t(i + 2)].captured != NoPiece && c.refut[size_t(i + 2)].move.to == st.move.to;
        const int given = kPiecePoints[q.at(st.move.from).type] - (st.captured != NoPiece ? kPiecePoints[st.captured] : 0) -
                          (backThere ? kPiecePoints[c.refut[size_t(i + 2)].captured] : 0);
        const int after = c.lp ? coach::detail::whiteCp(c.lp->score, true) : 0;
        const bool unborne = c.lp && after >= 100 * next.balance + 100;
        const bool spite = c.lp && st.check && after <= -1000;
        if (st.mover == c.human && next.captured != NoPiece && next.move.to == st.move.to && !recapture && given > 0 &&
            (spite || unborne) && safeWhereItStands(q, st.move.from)) {
            const int before = shownLoss(c, i);
            if (pts->number > std::max(0, before) && shownLoss(c, i + 2) >= pts->number)
                out.push_back({"GIVEN_AWAY", b.line.key + " says " + std::to_string(pts->number) + ": " + st.san +
                                                 " gives the piece away, " + std::to_string(before) + " lost before it"});
            return;
        }
        q.makeMove(st.move);
    }
}

// "That trade costs you the bishop pair": half a pawn, never the reason for a blunder, nor where the
// better move wins more material within 8 plies (Bxd4 when Nxb4 wins a pawn more).
void checkBishopPair(const Ctx& c, const PlyVerdict& v, const Beat& b, std::vector<Finding>& out) {
    if (b.line.key.rfind("ex.bishop_pair.", 0) != 0) return;
    if (v.cls == MoveClass::Blunder) out.push_back({"BISHOP_PAIR_BLUNDER", b.line.key});
    const int best = -lossAt(c.bestLine, c.p0, c.human, c.base, std::min<size_t>(8, c.bestLine.size()));
    const int played = -lossAt(c.playedLine, c.p0, c.human, c.base, std::min<size_t>(8, c.playedLine.size()));
    if (best > played)
        out.push_back({"BISHOP_PAIR_MORE_WON", b.line.key + ": the better line wins " + std::to_string(best) + ", the move " +
                                                   std::to_string(played)});
}

void checkFork(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    const bool allowed = k.rfind("ex.fork.b", 0) == 0, missed = k.rfind("ex.missed_fork", 0) == 0;
    if (!allowed && !missed) return;
    const Arg* mv = b.line.arg(allowed ? "reply" : "best");
    const Arg* t1 = b.line.arg("t1");
    const Arg* t2 = b.line.arg("t2");
    if (!mv || !t1 || !t2) return;
    // Two pawns are no fork worth the name ("Ng4 forked my pawn and my pawn"): one falls at most.
    if (t1->piece == Pawn && t2->piece == Pawn) out.push_back({"FORK_PAWNS", k + ": " + mv->san});
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

// ---- Tactical motifs said about the coach's reply (skewer, pin, discovered attack, back rank, a
// cheaper attacker, a bad trade) and the mate patterns named: each read again on the board ----------

bool isLinePiece(PieceType t) { return t == Bishop || t == Rook || t == Queen; }

// The first piece standing past 'via' on the line that runs from 'from' through 'via' (rank, file or
// diagonal), NoSquare when the two squares are not on one line or the line leaves the board first.
Square nextOnLine(const Position& p, Square from, Square via) {
    const int df = fileOf(via) - fileOf(from), dr = rankOf(via) - rankOf(from);
    if ((df == 0 && dr == 0) || (df != 0 && dr != 0 && std::abs(df) != std::abs(dr))) return NoSquare;
    const int sf = (df > 0) - (df < 0), sr = (dr > 0) - (dr < 0);
    for (int f = fileOf(via) + sf, r = rankOf(via) + sr; f >= 0 && f < 8 && r >= 0 && r < 8; f += sf, r += sr)
        if (!p.at(makeSquare(f, r)).empty()) return makeSquare(f, r);
    return NoSquare;
}

// The coach's reply a line says ({reply}, legal in p1), else the refutation's first move.
Move saidReply(const Ctx& c, const Beat& b) {
    if (const Arg* rp = b.line.arg("reply"); rp && !rp->uci.empty()) return c.p1.parseUCI(rp->uci);
    return c.refut.empty() ? Move() : c.refut[0].move;
}

// The ply of the refutation where the coach captures the human's piece that stands on 'sq' after
// the first 'from' plies (the piece followed as it moves), -1 when it is not taken within 'plies'
// more plies; 'by' gets the square the capturing piece came from.
int takenAt(const Ctx& c, Square sq, int from, int plies, Square* by = nullptr) {
    for (int i = from; i < int(c.refut.size()) && i < from + plies; ++i) {
        const LineStep& st = c.refut[size_t(i)];
        const Square on = (st.move.flags & MoveEnPassant) ? Square(st.move.to + (c.human == White ? 8 : -8)) : st.move.to;
        if (st.mover == c.human && st.move.from == sq) sq = st.move.to;
        else if (st.mover == c.coach && on == sq && st.captured != NoPiece) {
            if (by) *by = st.move.from;
            return i;
        }
    }
    return -1;
}

// What the coach keeps of the piece it takes at ply 'i' of the refutation: its points, less what the
// human takes back on that square with the next ply.
int keptAt(const Ctx& c, int i) {
    const LineStep& st = c.refut[size_t(i)];
    int kept = kPiecePoints[st.captured];
    if (i + 1 < int(c.refut.size())) {
        const LineStep& back = c.refut[size_t(i + 1)];
        if (back.mover == c.human && back.captured != NoPiece && back.move.to == st.move.to) kept -= kPiecePoints[back.captured];
    }
    return kept;
}

// "{reply} skewers {t1} and {t2}" (ex.skewer.*): the coach's reply moves {my} (where the line names
// it), a bishop, a rook or a queen, to a square from which it attacks {t1} along a line; the first
// piece past {t1} on that line is {t2}; both are the human's; {t1} is the king or worth at least as
// much as {t2} (a cheaper piece in front is a pin, not a skewer). "When it moves, I take {t2}": the
// refutation takes {t2} (followed as it moves) within six plies after the reply, the skewering piece
// still on the board (taken first, it skewered nothing: SKEWER_PIECE_TAKEN), best by that piece itself
// (another piece collecting {t2}: a warning).
void checkSkewer(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("ex.skewer.", 0) != 0) return;
    const Arg* t1 = b.line.arg("t1");
    const Arg* t2 = b.line.arg("t2");
    const Arg* my = b.line.arg("my");
    const Move m = saidReply(c, b);
    if (!t1 || !t2 || t1->square == NoSquare || t2->square == NoSquare || !m.valid()) {
        out.push_back({"SKEWER_ARGS", k});
        return;
    }
    if (my && my->square != NoSquare && my->square != m.from)
        out.push_back({"SKEWER_MOVER", k + ": {my} on " + squareName(my->square) + ", the reply moves " + squareName(m.from)});
    Position p2 = c.p1;
    p2.makeMove(m);
    const Piece a = p2.at(m.to);
    const std::string reply = c.p1.toSAN(m);
    if (!isLinePiece(a.type)) {
        out.push_back({"SKEWER_NOT_LINE_PIECE", k + ": " + reply + " moves a " + pieceName(a.type)});
    } else if (!(p2.attacksFrom(m.to) & squareBit(t1->square))) {
        out.push_back({"SKEWER_NO_LINE", k + ": after " + reply + " the " + pieceName(a.type) + " does not attack " +
                                             squareName(t1->square)});
    } else {
        const Square behind = nextOnLine(p2, m.to, t1->square);
        if (behind != t2->square)
            out.push_back({"SKEWER_NOT_BEHIND", k + ": past " + squareName(t1->square) + " the line meets " +
                                                    (behind == NoSquare ? std::string("nothing") : squareName(behind)) +
                                                    ", not " + squareName(t2->square)});
    }
    for (const Arg* t : {t1, t2}) {
        const Piece on = p2.at(t->square);
        if (on.color != c.human || on.type != t->piece || on.empty())
            out.push_back({"SKEWER_TARGET", k + ": " + squareName(t->square) + " holds no " + pieceName(t->piece) + " of the human's"});
    }
    if (t1->piece != King && kPiecePoints[t1->piece] < kPiecePoints[t2->piece])
        out.push_back({"SKEWER_FRONT_CHEAPER", k + ": the " + pieceName(t1->piece) + " in front is worth less than the " +
                                                   pieceName(t2->piece) + " behind (a pin)"});
    // The skewering piece followed along the line: it is the one that takes {t2}.
    Square by = NoSquare;
    const int at = takenAt(c, t2->square, 1, 6, &by);
    if (at < 0) {
        out.push_back({"SKEWER_BEHIND_KEPT", k + ": the line never takes the " + pieceName(t2->piece) + " on " +
                                                 squareName(t2->square)});
        return;
    }
    // Taken first itself (Bg4+ Kxg4 Qxe2+: the king took the bishop, a deflection; Qxb1+ Nxb1 Rc1+:
    // a trade, then another skewer), the skewering piece wins nothing: an error. Alive, it may leave
    // the capture to another piece (Qh1+ Kf2 Rxf6+ Ke3 Rxf1): a warning.
    Square skewerer = m.to;
    bool lost = false;
    for (int i = 1; i < at && !lost; ++i) {
        const LineStep& st = c.refut[size_t(i)];
        if (st.mover == c.coach && st.move.from == skewerer) skewerer = st.move.to;
        else if (st.mover == c.human && st.move.to == skewerer && st.captured != NoPiece) lost = true;
    }
    if (lost)
        out.push_back({"SKEWER_PIECE_TAKEN", k + ": " + reply + " is taken before " + c.refut[size_t(at)].san + " wins the " +
                                                 pieceName(t2->piece)});
    else if (by != skewerer)
        out.push_back({"SKEWER_OTHER_TAKER", k + ": " + c.refut[size_t(at)].san + " takes the " + pieceName(t2->piece) +
                                                 ", not the skewering piece", false});
}

// 'p' with the piece on 's' taken off the board (through the FEN); 'p' itself when that leaves no
// valid position.
Position withoutPiece(const Position& p, Square s) {
    std::string fen = p.fen();
    const size_t end = fen.find(' ');
    std::string board;
    for (char ch : fen.substr(0, end)) {
        if (ch >= '1' && ch <= '8') board.append(size_t(ch - '0'), '1');
        else board.push_back(ch);
    }
    // FEN ranks run 8 to 1, files a to h; '/' separates them.
    const int rank = s / 8, file = s % 8;
    size_t at = 0;
    for (int r = 7; r > rank; --r) at = board.find('/', at) + 1;
    board[at + size_t(file)] = '1';
    std::string packed;
    int run = 0;
    for (char ch : board) {
        if (ch == '1') { ++run; continue; }
        if (run) packed.push_back(char('0' + run)), run = 0;
        packed.push_back(ch);
    }
    if (run) packed.push_back(char('0' + run));
    Position q;
    return q.setFEN(packed + fen.substr(end)) ? q : p;
}

// "{your} is pinned to {t1}" (ex.pin.*; ex.pin_defender has its own checks): after the coach's reply
// a coach bishop, rook or queen stands on {sq}, attacks {your} along a line, and the first piece past
// {your} on that line is {t1}, the king or a piece worth more than {your} (a pin that costs something
// when the piece steps aside); {your} cannot take the pinner without losing by it. The phrasings
// where the reply itself pins ("{my} can go to {sq}", "{reply} pins") have the reply land on {sq}
// (from {my}'s square). Below a king, the pin bites: {your} stepping off the line lets the pinner win
// the piece behind. "I win it", "the pinned piece falls": the refutation takes {your} within six plies
// after the reply, the human does not take back as much there at once, and the pinner is still on the
// board then (taken first, the pin is not what wins the piece).
void checkPin(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("ex.pin.", 0) != 0) return;
    const Arg* your = b.line.arg("your");
    const Arg* t1 = b.line.arg("t1");
    const Arg* sq = b.line.arg("sq");
    const Arg* my = b.line.arg("my");
    const Move m = saidReply(c, b);
    if (!your || !t1 || !sq || your->square == NoSquare || t1->square == NoSquare || sq->square == NoSquare || !m.valid()) {
        out.push_back({"PIN_ARGS", k});
        return;
    }
    const std::string reply = c.p1.toSAN(m);
    // b1: "{my} can go to {sq}"; b4-b6: "{reply} pins".
    const bool replyPins = k.rfind("ex.pin.b1", 0) == 0 || k.rfind("ex.pin.b4", 0) == 0 || k.rfind("ex.pin.b5", 0) == 0 ||
                           k.rfind("ex.pin.b6", 0) == 0;
    if (replyPins && m.to != sq->square)
        out.push_back({"PIN_NOT_REPLY", k + ": " + reply + " does not go to " + squareName(sq->square)});
    if (k.rfind("ex.pin.b1", 0) == 0 && my && my->square != m.from)
        out.push_back({"PIN_NOT_REPLY", k + ": {my} on " + squareName(my->square) + ", " + reply + " moves " + squareName(m.from)});
    Position p2 = c.p1;
    p2.makeMove(m);
    const Piece pinner = p2.at(sq->square);
    if (pinner.empty() || pinner.color != c.coach || !isLinePiece(pinner.type)) {
        out.push_back({"PIN_NO_PINNER", k + ": after " + reply + " " + squareName(sq->square) + " holds no bishop, rook or queen of mine"});
        return;
    }
    if (!(p2.attacksFrom(sq->square) & squareBit(your->square))) {
        out.push_back({"PIN_NO_LINE", k + ": after " + reply + " the " + pieceName(pinner.type) + " on " + squareName(sq->square) +
                                          " does not attack " + squareName(your->square)});
        return;
    }
    const Square behind = nextOnLine(p2, sq->square, your->square);
    if (behind != t1->square || p2.at(behind).color != c.human || p2.at(behind).type != t1->piece)
        out.push_back({"PIN_NOT_BEHIND", k + ": past " + squareName(your->square) + " the line meets " +
                                             (behind == NoSquare ? std::string("nothing") : squareName(behind)) + ", not " +
                                             squareName(t1->square)});
    if (t1->piece != King && kPiecePoints[t1->piece] <= kPiecePoints[your->piece])
        out.push_back({"PIN_BEHIND_CHEAP", k + ": the " + pieceName(t1->piece) + " behind is worth no more than the pinned " +
                                               pieceName(your->piece)});
    // The pinned piece takes its pinner and comes out even or better: no pin.
    if (p2.sideToMove() == c.human)
        for (const Move& x : p2.legalMovesFrom(your->square))
            if (x.to == sq->square && seePoints(p2, x) >= 0) {
                out.push_back({"PIN_BREAKS", k + ": " + p2.toSAN(x) + " takes the pinner"});
                break;
            }
    // A pin to a piece other than the king bites: wherever the pinned piece steps off the line (when
    // it can), the pinner then wins material taking the piece behind (a pawn "pinned" by a queen to a
    // knight the rook guards is free to go: Qxb8 Rxb8 loses the queen).
    if (t1->piece != King && behind == t1->square) {
        const uint64_t line = squaresBetween(sq->square, t1->square) | squareBit(sq->square);
        bool offLine = false, bites = false;
        for (const Move& x : p2.legalMovesFrom(your->square)) {
            if (line & squareBit(x.to)) continue;
            offLine = true;
            Position q = p2;
            q.makeMove(x);
            const Move take = q.findLegal(sq->square, t1->square, Queen);
            if (take.valid() && seePoints(q, take) >= 1) bites = true;
        }
        if (offLine && !bites)
            out.push_back({"PIN_NO_BITE", k + ": the " + pieceName(your->piece) + " can step off the line, the " +
                                              pieceName(pinner.type) + " then wins nothing taking the " + pieceName(t1->piece)});
    }
    // The pin holds something back: with the pinner off the board, the pinned piece has a legal move
    // off the line (a pawn on a file with nothing to take only pushes along it; it is not stuck).
    if (behind == t1->square) {
        const Position free = withoutPiece(p2, sq->square);
        const uint64_t line = squaresBetween(sq->square, t1->square) | squareBit(sq->square);
        bool offLine = false;
        for (const Move& x : free.legalMovesFrom(your->square))
            if (!(line & squareBit(x.to))) offLine = true;
        if (free.sideToMove() == c.human && !offLine)
            out.push_back({"PIN_GOES_NOWHERE", k + ": the " + pieceName(your->piece) + " has no move off the line, pin or not"});
    }
    const int at = takenAt(c, your->square, 1, 6);
    if (at < 0) {
        out.push_back({"PIN_NOT_WON", k + ": the line never takes the pinned " + pieceName(your->piece)});
        return;
    }
    if (keptAt(c, at) <= 0)
        out.push_back({"PIN_TRADED", k + ": " + c.refut[size_t(at)].san + " is taken back with " + c.refut[size_t(at + 1)].san});
    // The pinner taken before the pinned piece falls (Qf6+ Qxf6 exf6+ Kf7 fxg7): another piece wins it,
    // the pin did not.
    Square pinnerAt = sq->square;
    for (int i = 1; i < at; ++i) {
        const LineStep& st = c.refut[size_t(i)];
        if (st.mover == c.coach && st.move.from == pinnerAt) pinnerAt = st.move.to;
        else if (st.mover == c.human && st.move.to == pinnerAt && st.captured != NoPiece) {
            out.push_back({"PIN_PINNER_TAKEN", k + ": " + st.san + " takes the pinner before " + c.refut[size_t(at)].san +
                                                   " wins the " + pieceName(your->piece)});
            break;
        }
    }
}

// "{my} moves away and {my2} attacks {t1}" / "{reply} uncovers an attack on {t1}" (ex.discovered.*):
// in p1, {my2} is a coach bishop, rook or queen on one line with {t1}, and {my} is the only piece
// between them; the reply moves {my}, and then {my2} attacks {t1}. A target other than the king is
// won: the refutation takes it within six plies after the reply.
void checkDiscovered(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("ex.discovered.", 0) != 0) return;
    const Arg* my = b.line.arg("my");
    const Arg* my2 = b.line.arg("my2");
    const Arg* t1 = b.line.arg("t1");
    const Move m = saidReply(c, b);
    if (!my || !my2 || !t1 || my->square == NoSquare || my2->square == NoSquare || t1->square == NoSquare || !m.valid()) {
        out.push_back({"DISCOVERED_ARGS", k});
        return;
    }
    const std::string reply = c.p1.toSAN(m);
    if (m.from != my->square)
        out.push_back({"DISCOVERED_MOVER", k + ": {my} on " + squareName(my->square) + ", " + reply + " moves " + squareName(m.from)});
    const Piece slider = c.p1.at(my2->square);
    if (slider.color != c.coach || !isLinePiece(slider.type)) {
        out.push_back({"DISCOVERED_NOT_LINE_PIECE", k + ": " + squareName(my2->square) + " holds no bishop, rook or queen of mine"});
        return;
    }
    // On one of the slider's lines (an empty board), {my} the only piece between.
    const uint64_t between = squaresBetween(my2->square, t1->square);
    if (!(attacksOf(slider.type, slider.color, my2->square, 0) & squareBit(t1->square)) ||
        (between & c.p1.occupancy()) != squareBit(my->square))
        out.push_back({"DISCOVERED_NOT_BLOCKED", k + ": in p1 " + squareName(my->square) + " is not the one piece between " +
                                                     squareName(my2->square) + " and " + squareName(t1->square)});
    Position p2 = c.p1;
    p2.makeMove(m);
    if (!(p2.attacksFrom(my2->square) & squareBit(t1->square)) || p2.at(t1->square).color != c.human ||
        p2.at(t1->square).type != t1->piece)
        out.push_back({"DISCOVERED_NO_ATTACK", k + ": after " + reply + " " + squareName(my2->square) + " does not attack the " +
                                                   pieceName(t1->piece) + " on " + squareName(t1->square)});
    if (t1->piece == King) return;
    Square by = NoSquare;
    const int at = takenAt(c, t1->square, 1, 6, &by);
    if (at < 0) {
        out.push_back({"DISCOVERED_NOT_TAKEN", k + ": the line never takes the " + pieceName(t1->piece) + " on " +
                                                   squareName(t1->square)});
        return;
    }
    // The uncovered piece taken before the target falls: another attack wins it (a warning).
    Square sliderAt = my2->square;
    for (int i = 1; i < at; ++i) {
        const LineStep& st = c.refut[size_t(i)];
        if (st.mover == c.coach && st.move.from == sliderAt) sliderAt = st.move.to;
        else if (st.mover == c.human && st.move.to == sliderAt && st.captured != NoPiece) {
            out.push_back({"DISCOVERED_SLIDER_TAKEN", k + ": " + st.san + " takes the " + pieceName(slider.type) + " before " +
                                                          c.refut[size_t(at)].san + " wins the " + pieceName(t1->piece), false});
            break;
        }
    }
}

// The squares in front of c's king standing on its first rank: next to it on the second rank.
uint64_t frontOfKing(const Position& p, Color c) {
    const Square ks = p.kingSquare(c);
    const int up = c == White ? 1 : -1;
    uint64_t s = 0;
    for (int df = -1; df <= 1; ++df) {
        const int f = fileOf(ks) + df, r = rankOf(ks) + up;
        if (f >= 0 && f < 8 && r >= 0 && r < 8) s |= squareBit(makeSquare(f, r));
    }
    return s;
}

// The back rank (ex.back_rank.*): in p1 the human's king stands on its first rank and every square
// in front of it holds its own piece or is attacked by the coach (no air); a coach rook or queen
// ({my}'s kind) lands on {sq}, on that rank, with check among the coach's first four moves of the
// refutation, and the king then has no legal move off its first rank. Level 1's "locked in by its
// pawns", "behind its pawns": the squares in front of the king hold its own pawns.
void checkBackRank(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("ex.back_rank.b", 0) != 0) return;
    const Arg* sq = b.line.arg("sq");
    const Arg* my = b.line.arg("my");
    const Square ks = c.p1.kingSquare(c.human);
    const int home = c.human == White ? 0 : 7;
    if (rankOf(ks) != home) {
        out.push_back({"BACK_RANK_KING_OFF", k + ": the king stands on " + squareName(ks)});
        return;
    }
    const uint64_t front = frontOfKing(c.p1, c.human);
    const uint64_t occ = c.p1.occupancy() & ~squareBit(ks);
    for (Square s : squaresOf(front)) {
        const Piece on = c.p1.at(s);
        if (!on.empty() && on.color == c.human) continue;
        if (c.p1.attackersTo(s, occ) & c.p1.pieces(c.coach)) continue;
        out.push_back({"BACK_RANK_AIR", k + ": the king can step to " + squareName(s)});
        break;
    }
    if (k.rfind("ex.back_rank.b1", 0) == 0 && (front & c.p1.pieces(c.human, Pawn)) != front)
        out.push_back({"BACK_RANK_PAWNS", k + ": the squares in front of the king do not all hold its pawns"});
    if (!sq || sq->square == NoSquare) return;
    if (rankOf(sq->square) != home) out.push_back({"BACK_RANK_SQ", k + ": " + squareName(sq->square) + " is off the king's first rank"});
    Position q = c.p1;
    for (size_t i = 0; i < c.refut.size() && i < 8; ++i) {
        const LineStep& st = c.refut[i];
        q.makeMove(st.move);
        if (st.mover != c.coach || st.move.to != sq->square || !(st.piece == Rook || st.piece == Queen) || !q.inCheck()) continue;
        if (my && my->piece != st.piece)
            out.push_back({"BACK_RANK_PIECE", k + ": {my} is a " + pieceName(my->piece) + ", " + st.san + " moves a " + pieceName(st.piece)});
        const Square kq = q.kingSquare(c.human);
        for (const Move& x : q.legalMovesFrom(kq))
            if (rankOf(x.to) != home) {
                out.push_back({"BACK_RANK_ESCAPES", k + ": after " + st.san + " the king steps out with " + q.toSAN(x)});
                break;
            }
        return;
    }
    out.push_back({"BACK_RANK_NO_CHECK", k + ": no rook or queen of mine checks on " + squareName(sq->square) + " in the line"});
}

// "Give your king a little window: move one pawn in front of it" (ex.back_rank.tip.*): the human's
// king stands on its first rank behind its own pawns (every square in front of it holds one), and one
// of them can step forward.
void checkBackRankTip(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("ex.back_rank.tip", 0) != 0) return;
    const Square ks = c.p1.kingSquare(c.human);
    const int home = c.human == White ? 0 : 7, up = c.human == White ? 8 : -8;
    const uint64_t front = frontOfKing(c.p1, c.human);
    if (rankOf(ks) != home || (front & c.p1.pieces(c.human, Pawn)) != front) {
        out.push_back({"BACK_RANK_TIP_PAWNS", k + ": the king on " + squareName(ks) + " is not behind a row of its pawns"});
        return;
    }
    bool push = false;
    for (Square s : squaresOf(front)) push = push || c.p1.at(Square(s + up)).empty();
    if (!push) out.push_back({"BACK_RANK_TIP_BLOCKED", k + ": no pawn in front of the king can step forward"});
}

// "{your} on {sq} is attacked by a cheaper piece" (ex.exchange_cheap.*): in p1 {my}, a coach piece
// worth fewer points than {your}, has a legal capture of {your} on {sq}; {n} and {n2} are their
// points; the {reply} the phrasings name is that capture.
void checkExchangeCheap(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("ex.exchange_cheap.", 0) != 0) return;
    const Arg* your = b.line.arg("your");
    const Arg* my = b.line.arg("my");
    const Arg* sq = b.line.arg("sq");
    if (!your || !my || !sq || my->square == NoSquare || sq->square == NoSquare) {
        out.push_back({"CHEAP_ARGS", k});
        return;
    }
    const Piece victim = c.p1.at(sq->square), attacker = c.p1.at(my->square);
    if (victim.empty() || victim.color != c.human || victim.type != your->piece || attacker.empty() || attacker.color != c.coach ||
        attacker.type != my->piece)
        out.push_back({"CHEAP_PIECES", k + ": the board has a " + pieceName(attacker.type) + " on " + squareName(my->square) +
                                           " and a " + pieceName(victim.type) + " on " + squareName(sq->square)});
    if (kPiecePoints[attacker.type] >= kPiecePoints[victim.type])
        out.push_back({"CHEAP_NOT_CHEAPER", k + ": a " + pieceName(attacker.type) + " takes a " + pieceName(victim.type)});
    if (!c.p1.findLegal(my->square, sq->square, Queen).valid())
        out.push_back({"CHEAP_NO_CAPTURE", k + ": " + squareName(my->square) + " cannot take on " + squareName(sq->square)});
    const Arg* n = b.line.arg("n");
    const Arg* n2 = b.line.arg("n2");
    if ((n && n->number != kPiecePoints[victim.type]) || (n2 && n2->number != kPiecePoints[attacker.type]))
        out.push_back({"CHEAP_NUMBERS", k + ": " + std::to_string(n ? n->number : -1) + " and " + std::to_string(n2 ? n2->number : -1)});
    const Move m = saidReply(c, b);
    if (m.valid() && (m.from != my->square || m.to != sq->square))
        out.push_back({"CHEAP_REPLY", k + ": the reply " + c.p1.toSAN(m) + " is not " + squareName(my->square) + "x" +
                                          squareName(sq->square)});
}

// "This trade leads to a pawn ending that I win" (ex.bad_trade.*): the {line} said (from the table)
// ends with kings and pawns alone; a phrasing without a line, the refutation reaches such an ending
// within its first ten plies. "That I win", "lost": the engine gives the human 25 % at most after
// the move (a warning). "This trade", "Trading into", "Wrong trade" (b3, b4, b5): the move itself
// takes something or is taken on its square by the reply.
void checkBadTrade(const Ctx& c, const PlyVerdict& v, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("ex.bad_trade.", 0) != 0) return;
    const auto piecesLeft = [](const Position& p) {
        return p.occupancy() & ~(p.pieces(Pawn) | p.pieces(King));
    };
    const Arg* ln = b.line.arg("line");
    Position end;
    int plies = 0;
    if (ln && !ln->san.empty() && replaySan(c.p1, ln->san, end, plies)) {
        if (piecesLeft(end))
            out.push_back({"BAD_TRADE_NOT_PAWN_ENDING", k + ": pieces are left after " + ln->san});
    } else {
        Position q = c.p1;
        bool reached = !piecesLeft(q);
        for (size_t i = 0; i < c.refut.size() && i < 10 && !reached; ++i) {
            q.makeMove(c.refut[i].move);
            reached = !piecesLeft(q);
        }
        if (!reached) out.push_back({"BAD_TRADE_NOT_PAWN_ENDING", k + ": the line reaches no pawn ending"});
    }
    if (v.wPlayed > 25.0)
        out.push_back({"BAD_TRADE_NOT_LOST", k + ": the human keeps " + std::to_string(int(v.wPlayed)) + " %", false});
    // The phrasings that call the move itself a trade ("This trade leads to...", "Trading into this
    // pawn ending loses", "Wrong trade"): b3, b4 and b5.
    const bool callsMoveTrade = k.rfind("ex.bad_trade.b3", 0) == 0 || k.rfind("ex.bad_trade.b4", 0) == 0 ||
                                k.rfind("ex.bad_trade.b5", 0) == 0;
    const Move pm = c.p0.parseUCI(c.rec->played);
    const bool takes = pm.valid() && !c.p0.at(pm.to).empty();
    const bool takenBack = !c.refut.empty() && c.refut[0].captured != NoPiece && c.refut[0].move.to == pm.to;
    if (callsMoveTrade && !takes && !takenBack)
        out.push_back({"BAD_TRADE_NO_TRADE", k + ": " + c.p0.toSAN(pm) + " takes nothing and is not taken; the line " +
                                                 (ln ? ln->san : std::string()) + " trades"});
}

// The mate pattern named (ex.mate_allowed.pattern, {text} = name.pattern.*), read on the mated
// position: the refutation played from the table until it mates. One checker always, and:
//   back rank: the king on its first rank, the checker a rook or a queen on that rank, every square
//     in front of the king held by its own pieces (PATTERN_BACK_RANK_OPEN when two or more of them
//     are open or one holds a piece of the mating side, _HELPED when one is open and another piece
//     of the mating side covers it, _CHECKER, a warning, when the checker alone covers the open ones);
//   smothered: the checker a knight, every square next to the king held by its own pieces;
//   support: the checker a queen next to the king, defended by another piece of the mating side;
//   ladder: the king on an edge rank or file, the checker a rook or a queen on that edge line, and
//     another rook or queen of the mating side on the next line inward guarding a square of it next
//     to the king;
//   epaulette: the checker on the king's file, the squares on both sides of the king on its rank
//     held by its own pieces.
void checkMatePattern(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    const Arg* text = b.line.arg("text");
    if (k.rfind("ex.mate_allowed.pattern", 0) != 0 || !text || text->text.rfind("name.pattern.", 0) != 0) return;
    const std::string name = text->text.substr(13);
    Position q = c.p1;
    bool mated = q.isCheckmate();
    for (size_t i = 0; i < c.refut.size() && !mated; ++i) {
        q.makeMove(c.refut[i].move);
        mated = q.isCheckmate();
    }
    if (!mated) {
        out.push_back({"PATTERN_NO_MATE", k + " " + name + ": the line never mates"});
        return;
    }
    const Color loser = q.sideToMove(), winner = opposite(loser);
    const Square ks = q.kingSquare(loser);
    const uint64_t checkers = q.checkers();
    if (squareCount(checkers) != 1) {
        out.push_back({"PATTERN_SHAPE", k + " " + name + ": a double check (" + q.fen() + ")"});
        return;
    }
    const Square cs = squaresOf(checkers)[0];
    const PieceType ct = q.at(cs).type;
    const uint64_t around = attacksOf(King, loser, ks, 0);
    const uint64_t own = q.pieces(loser);
    const int home = loser == White ? 0 : 7;
    bool ok = true;
    std::string why;
    bool soft = false;   // a back-rank mate the checker alone completes: a warning, not an error
    bool helped = false;
    if (name == "back_rank") {
        const uint64_t front = frontOfKing(q, loser);
        const bool shape = rankOf(ks) == home && (ct == Rook || ct == Queen) && rankOf(cs) == home;
        ok = shape && (front & own) == front;
        // The shape, but a square in front of the king shut by the mating side, not by its own piece:
        // by the checker alone (Qf8# against Kh8 and h7, the queen covering g7: often called so), or
        // by another piece of the mating side or one standing there (a mating net, no back-rank mate).
        // One open square held by another piece of the mating side (Qb8# against Kg8, f7 h7, the
        // bishop on h6 covering g7): helped, an error by the definition, the mildest case.
        if (shape && !ok) {
            soft = true;
            bool held = false;
            const uint64_t occ = q.occupancy() & ~squareBit(ks);
            for (Square s : squaresOf(front & ~own)) {
                const uint64_t by = q.attackersTo(s, occ) & q.pieces(winner);
                if (!q.at(s).empty() || (by & ~squareBit(cs))) soft = false;
                if (q.at(s).empty() && (by & ~squareBit(cs))) held = true;
            }
            helped = !soft && held && squareCount(front & ~own) == 1;
            why = soft     ? "the checker alone covers the open squares in front of the king"
                  : helped ? "one square in front of the king is open, held by another piece"
                           : "squares in front of the king are open or taken by the mating side";
        }
    } else if (name == "smothered") {
        ok = ct == Knight && (around & ~own) == 0;
    } else if (name == "support") {
        ok = ct == Queen && (around & squareBit(cs)) && (q.attackersTo(cs, winner) != 0);
    } else if (name == "ladder") {
        const int kf = fileOf(ks), kr = rankOf(ks);
        const uint64_t heavy = (q.pieces(winner, Rook) | q.pieces(winner, Queen)) & ~squareBit(cs);
        ok = false;
        if ((ct == Rook || ct == Queen) && (kr == 0 || kr == 7) && rankOf(cs) == kr) {
            const int inner = kr == 0 ? 1 : 6;
            for (Square s : squaresOf(heavy))
                if (rankOf(s) == inner && (q.attacksFrom(s) & around & (0xFFULL << (8 * inner)))) ok = true;
        }
        if ((ct == Rook || ct == Queen) && (kf == 0 || kf == 7) && fileOf(cs) == kf) {
            const int inner = kf == 0 ? 1 : 6;
            for (Square s : squaresOf(heavy))
                if (fileOf(s) == inner && (q.attacksFrom(s) & around & (0x0101010101010101ULL << inner))) ok = true;
        }
    } else if (name == "epaulette") {
        const int kf = fileOf(ks), kr = rankOf(ks);
        ok = kf > 0 && kf < 7 && fileOf(cs) == kf && q.at(makeSquare(kf - 1, kr)).color == loser &&
             !q.at(makeSquare(kf - 1, kr)).empty() && q.at(makeSquare(kf + 1, kr)).color == loser &&
             !q.at(makeSquare(kf + 1, kr)).empty();
    } else {
        out.push_back({"PATTERN_UNKNOWN", k + " " + name});
        return;
    }
    if (!ok)
        out.push_back({why.empty() ? "PATTERN_SHAPE" : soft ? "PATTERN_BACK_RANK_CHECKER"
                                                     : helped ? "PATTERN_BACK_RANK_HELPED" : "PATTERN_BACK_RANK_OPEN",
                       k + " " + name + ": " + pieceName(ct) + " on " + squareName(cs) + " mates the king on " + squareName(ks) +
                           (why.empty() ? "" : ", " + why) + " (" + q.fen() + ")",
                       !soft});
}

// The capture a move leaves for later: its mover could win a piece worth more than what the move
// takes, by 3 points more at least by exchange (the best such capture), else an invalid move.
Move biggerPrize(const Position& before, const Move& m) {
    const Piece victim = before.at((m.flags & MoveEnPassant) ? Square(m.to + (before.sideToMove() == White ? -8 : 8)) : m.to);
    const int took = victim.empty() ? 0 : kPiecePoints[victim.type];
    // A promotion wins the new piece (a8=Q gains more than the knight left standing).
    const int gain = victim.empty() && m.promotion == NoPiece ? 0 : seePoints(before, m);
    Move prize;
    int most = gain + 2;
    for (const Move& x : before.legalMoves()) {
        const Piece t = before.at(x.to);
        if (t.empty() || kPiecePoints[t.type] <= took) continue;
        const int g = seePoints(before, x);
        if (g > most) {
            most = g;
            prize = x;
        }
    }
    return prize;
}

// The side that played moves[i] (pos[i] before it) wins 'worth' points or more back along the line:
// it takes a piece worth that much within its next three moves, all checks before that capture,
// moves[i] itself a capture or a check; or with its very next move, made by the piece moves[i] moved
// (it attacked it).
bool winsBack(const std::vector<Position>& pos, const std::vector<Move>& moves, size_t i, int worth) {
    const bool forcing = pos[i + 1].inCheck() || !pos[i].at(moves[i].to).empty();
    for (size_t j = i + 2; j < moves.size() && j <= i + 6; j += 2) {
        const Piece victim = pos[j].at(moves[j].to);
        if (!victim.empty() && kPiecePoints[victim.type] >= worth &&
            (forcing || (j == i + 2 && moves[j].from == moves[i].to)))
            return true;
        if (!forcing || !pos[j + 1].inCheck()) return false;
    }
    return false;
}

// A line the coach shows (a demonstration from p1: 'keys' holds each move's narration) or says: no
// move may leave a bigger prize standing, which a player would see as the queen ignored whatever
// the engine's reason, unless a mate or a stalemate ends the line (material no longer counts), or
// its side wins as much back along the line (winsBack). The coach's in-between move (it takes that
// very piece with its next move) is said as such in a demonstration (demo.first, demo.my.first.*). 'skipFirst': the line starts with the engine's best move, said whatever it leaves.
void checkLinePrize(const Position& start, const std::vector<Move>& moves, const std::vector<std::string>* keys,
                    const std::string& where, bool skipFirst, std::vector<Finding>& out) {
    std::vector<Position> pos{start};
    for (const Move& m : moves) {
        Position q = pos.back();
        q.makeMove(m);
        pos.push_back(q);
    }
    if (pos.back().isCheckmate() || pos.back().isStalemate()) return;
    for (size_t i = skipFirst ? 1 : 0; i < moves.size(); ++i) {
        const Position& before = pos[i];
        const Move prize = biggerPrize(before, moves[i]);
        if (!prize.valid() || pos[i + 1].isCheckmate()) continue;
        const bool check = pos[i + 1].inCheck();
        const bool between = i + 2 < moves.size() && moves[i + 1].from != prize.to &&
                             moves[i + 2].to == prize.to && pos[i + 2].at(prize.to) == before.at(prize.to);
        const std::string what = before.toSAN(moves[i]) + (check ? " (check)" : "") + " while " + before.toSAN(prize) +
                                 " wins a " + pieceName(before.at(prize.to).type);
        if (!between && !winsBack(pos, moves, i, kPiecePoints[before.at(prize.to).type])) {
            out.push_back({"PRIZE_IGNORED", where + ": " + what});
        } else if (between && keys && i % 2 == 0 && i < keys->size() && (*keys)[i].rfind("demo.first", 0) != 0 &&
                   (*keys)[i].rfind("demo.my.first", 0) != 0) {
            out.push_back({"IN_BETWEEN_UNSAID", where + ": " + what + ", narrated " + (*keys)[i]});
        }
    }
}

// The {line} a Say beat says: where it starts (the table, p1, or the position before the move, p0;
// the better move's idea after that move) and its moves; 'fromBest': it starts with the engine's best
// move. False when the beat says no line.
bool saidLine(const Ctx& c, const Beat& b, Position& start, std::vector<Move>& moves, bool& fromBest) {
    const Arg* line = b.line.arg("line");
    if (!line || line->san.empty()) return false;
    bool said = false;
    for (const std::string& v : variantsOf(b.line.key)) said = said || usesPlaceholder(v, "line");
    if (!said) return false;
    // The better move's idea ({line} after {best}) starts after the better move; the other lines
    // from the table (p1), or the position before the move (p0).
    std::vector<Position> starts{c.p1, c.p0};
    if (b.line.key.rfind("ex.better.", 0) == 0) {
        if (c.bestLine.empty()) return false;
        starts = {c.p0};
        starts[0].makeMove(c.bestLine[0].move);
    }
    for (size_t k = 0; k < starts.size(); ++k) {
        Position end;
        int plies = 0;
        if (!replaySan(starts[k], line->san, end, plies)) continue;
        moves.clear();
        Position q = starts[k];
        std::istringstream in(line->san);
        std::string s;
        while (in >> s) {
            moves.push_back(q.parseSAN(s));
            q.makeMove(moves.back());
        }
        fromBest = k == 1 && !c.bestLine.empty() && moves[0] == c.bestLine[0].move;
        start = starts[k];
        return true;
    }
    return false;
}

// Every {line} said in a Say beat.
void checkLinePrizes(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    Position start;
    std::vector<Move> moves;
    bool fromBest = false;
    if (saidLine(c, b, start, moves, fromBest))
        checkLinePrize(start, moves, nullptr, b.line.key + " {line}", fromBest, out);
}

// Every move of a line shown or said is one its side would play: the engine at a fixed depth
// (MoveJudge) finds no move there better by 10 win percentage points or more (an error from 20). The
// engine's best move a line starts with ('skipFirst') is not judged.
void checkLinePlausible(MoveJudge& judge, const Position& start, const std::vector<Move>& moves,
                        const std::string& where, bool skipFirst, std::vector<Finding>& out) {
    Position q = start;
    for (size_t i = 0; i < moves.size(); ++i) {
        std::string best;
        const double cost = i == 0 && skipFirst ? 0.0 : judge.cost(q, moves[i], &best);
        if (cost >= 10.0) {
            const Move bm = q.parseUCI(best);
            char detail[256];
            std::snprintf(detail, sizeof detail, "%s: %s (ply %zu) gives away %.0f%%, best %s", where.c_str(),
                          q.toSAN(moves[i]).c_str(), i + 1, cost, bm.valid() ? q.toSAN(bm).c_str() : best.c_str());
            out.push_back({"UNLIKELY", detail, cost >= 20.0});
        }
        q.makeMove(moves[i]);
    }
}

// What the capture 'm' (legal in 'now') already won in 'before' with the same side to move: the
// same piece taking the same piece on the same squares (0 when it was no such move then).
int standingSee(const Position& before, const Position& now, const Move& m) {
    if (before.at(m.from) != now.at(m.from) || before.at(m.to) != now.at(m.to) ||
        before.sideToMove() != now.sideToMove())
        return 0;
    for (const Move& x : before.legalMoves())
        if (x.from == m.from && x.to == m.to && x.promotion == m.promotion) return std::max(0, seePoints(before, x));
    return 0;
}

// heldLossAt, where a capture the human could make before the move (p0) as well takes nothing back
// (the loss counts from p0, where it stood too), nor one after which the other side wins as much
// with a capture elsewhere.
int heldLossNet(const Ctx& c, const Position& end) {
    int lost = c.base - materialBalance(end, c.human);
    if (end.sideToMove() != c.human || !end.hasLegalMove()) return lost;
    int best = 0;
    for (const Move& m : end.legalMoves()) {
        if (!(m.flags & MoveCapture) || (m.promotion != NoPiece && m.promotion != Queen)) continue;
        const int won = seePoints(end, m) - standingSee(c.p0, end, m);
        int gain = won;
        if (gain <= best) continue;
        // No take-back while the other side then wins as much elsewhere (a queen left attacked).
        Position after = end;
        after.makeMove(m);
        for (const Move& x : after.legalMoves())
            if ((x.flags & MoveCapture) && x.to != m.to) gain = std::min(gain, won - seePoints(after, x));
        best = std::max(best, gain);
    }
    return lost - best;
}

// The retake line (ex.retake.*): where the sequence the cause names stops (its {line} or the
// demonstration, else the first prefix of the refutation where the move is legal, the human to move), {move} is a capture
// by {your2}, {reply} then a capture by {my} that takes {your}, after which the human is still down
// what the cause names (a point at least). Every other take-back worth a point there loses as much
// at once too. True when the line holds (LINE_SHORT then reads the table after both moves).
bool checkRetake(const Ctx& c, const Beat& b, int named, const Position* lineEnd, MoveJudge* judge,
                 std::vector<Finding>& out) {
    const Arg* mv = b.line.arg("move");
    const Arg* rp = b.line.arg("reply");
    if (!mv || !rp) {
        out.push_back({"RETAKE_ARGS", b.line.key});
        return false;
    }
    std::vector<Position> qs;
    if (lineEnd) qs.push_back(*lineEnd);
    Position q = c.p1;
    for (size_t i = 0; i < c.refut.size() && i < 12; ++i) {
        q.makeMove(c.refut[i].move);
        if (q.sideToMove() == c.human) qs.push_back(q);
    }
    for (const Position& at : qs) {
        const Move take = at.parseUCI(mv->uci);
        if (!take.valid() || at.sideToMove() != c.human) continue;
        if (!(take.flags & MoveCapture)) {
            out.push_back({"RETAKE_ILLEGAL", b.line.key + " " + mv->san + " takes nothing"});
            return false;
        }
        Position after = at;
        after.makeMove(take);
        const Move hit = after.parseUCI(rp->uci);
        if (!hit.valid() || !(hit.flags & MoveCapture)) {
            out.push_back({"RETAKE_ILLEGAL", b.line.key + " " + rp->san + " after " + mv->san});
            return false;
        }
        const Square lostOn = (hit.flags & MoveEnPassant) ? Square(hit.to + (c.human == White ? -8 : 8)) : hit.to;
        const Arg* your = b.line.arg("your");
        const Arg* your2 = b.line.arg("your2");
        const Arg* my = b.line.arg("my");
        if ((your && (your->piece != after.at(lostOn).type || after.at(lostOn).color != c.human)) ||
            (your2 && your2->piece != at.at(take.from).type) || (my && my->piece != after.at(hit.from).type))
            out.push_back({"RETAKE_PIECES", b.line.key + " " + mv->san + " " + rp->san});
        Position end = after;
        end.makeMove(hit);
        const int left = heldLossNet(c, end);
        if (left < std::max(1, named))
            out.push_back({"RETAKE_SHORT", b.line.key + " " + mv->san + " " + rp->san + " leaves the human " +
                                               std::to_string(left) + " down, " + std::to_string(named) + " named"});
        for (const Move& m : at.legalMoves()) {
            if (!(m.flags & MoveCapture) || m == take || (m.promotion != NoPiece && m.promotion != Queen)) continue;
            const int took = seePoints(at, m) - standingSee(c.p0, at, m);
            if (took < 1) continue;
            Position a2 = at;
            a2.makeMove(m);
            if (bestCapturePoints(a2, c.coach) < took)
                out.push_back({"RETAKE_OTHER", b.line.key + ": " + at.toSAN(m) + " takes back " + std::to_string(took)});
        }
        if (judge) {
            // The answer is real: the coach's capture is as good as its best move after the
            // take-back (one the engine refutes would make the take-back work). Where the take-back
            // is the engine's own choice there, every move loses as much: said, but counted.
            std::string best;
            const double cost = judge->cost(after, hit, &best);
            if (cost >= 5.0) {
                const Move bm = after.parseUCI(best);
                out.push_back({"RETAKE_WORKS", b.line.key + " " + mv->san + " " + rp->san + " costs the coach " +
                                                   std::to_string(int(cost)) + "%, best " +
                                                   (bm.valid() ? after.toSAN(bm) : best)});
            }
            std::string bestTake;
            if (judge->cost(at, take, &bestTake) == 0.0 && bestTake == mv->uci)
                out.push_back({"RETAKE_BEST", b.line.key + " " + mv->san + " is the best move there", false});
        }
        return left >= 1;
    }
    out.push_back({"RETAKE_ILLEGAL", b.line.key + " " + mv->san + " is no move where the sequence stops"});
    return false;
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

// ---- Endgame technique and draws ------------------------------------------------------------------

// Whether any English phrasing of the key holds 'word', or says the placeholder 'name'.
bool anyVariantHas(const std::string& key, const std::string& word) {
    for (const std::string& v : variantsOf(key))
        if (v.find(word) != std::string::npos) return true;
    return false;
}
bool anyVariantUses(const std::string& key, const std::string& name) {
    for (const std::string& v : variantsOf(key))
        if (usesPlaceholder(v, name)) return true;
    return false;
}

// Plays a space-separated SAN line from 'start': every position on the way (pos[0] = start) and every
// move; false when a move does not parse.
bool playLine(const Position& start, const std::string& line, std::vector<Position>& pos, std::vector<Move>& moves) {
    pos.assign(1, start);
    moves.clear();
    std::istringstream in(line);
    for (std::string s; in >> s;) {
        const Move m = pos.back().parseSAN(s);
        if (!m.valid()) return false;
        Position q = pos.back();
        q.makeMove(m);
        moves.push_back(m);
        pos.push_back(q);
    }
    return true;
}

// The coach is stalemated in 'p': the coach to move, not in check, without a single legal move.
bool coachStalemated(const Ctx& c, const Position& p) {
    return p.sideToMove() == c.coach && !p.inCheck() && p.legalMoves().empty();
}

// The human was winning before the move, as the board shows it: the best line mates, or the human
// is 3 points ahead before the move or where the best line ends.
bool humanWinning(const Ctx& c) {
    for (const LineStep& s : c.bestLine)
        if (s.mate && s.mover == c.human) return true;
    return c.base >= 3 || (!c.bestLine.empty() && materialBalance(c.bestEnd, c.human) >= 3);
}

// "Stalemate: my king has no legal move" (ex.stalemate, its tip too): the move played leaves the coach
// to move, not in check, with no legal move; {my} is the coach's king where it stands. A line that
// says a win was thrown away ("You were winning!", "a won game") needs the human winning before it.
void checkStalemate(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("ex.stalemate.", 0) != 0) return;
    if (!coachStalemated(c, c.p1))
        out.push_back({"STALEMATE_NONE", k + ": after " + c.rec->played + " the coach " +
                                             (c.p1.inCheck() ? "is in check" : "still has a legal move")});
    if (k.rfind("ex.stalemate.tip", 0) == 0) return;
    const Arg* my = b.line.arg("my");
    if (!my || my->piece != King || my->color != c.coach || my->square != c.p1.kingSquare(c.coach))
        out.push_back({"STALEMATE_NOT_KING", k + ": {my} is not the coach's king"});
    if ((anyVariantHas(k, "win") || anyVariantHas(k, "won ")) && !humanWinning(c))
        out.push_back({"STALEMATE_NOT_WINNING", k + ": the human was not winning on the board (" +
                                                    std::to_string(c.base) + " points, best line " +
                                                    sanLine(c.bestLine, 0, std::min<size_t>(6, c.bestLine.size())) + ")"});
}

// The stalemate trick (ex.stalemate_trick): its {line}, from the table, ends with the coach to move
// and stalemated, the human having taken material on the way (the coach gives its pieces away); {my}
// is the coach's king. The coach is the side that is losing (behind in material on the table), and
// the human cannot step aside: each of the human's moves along the line is its only legal move, an
// answer to a check, or takes the piece the coach just moved.
void checkStalemateTrick(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("ex.stalemate_trick.", 0) != 0) return;
    const Arg* my = b.line.arg("my");
    if (!my || my->piece != King || my->color != c.coach || my->square != c.p1.kingSquare(c.coach))
        out.push_back({"STALEMATE_TRICK_NOT_KING", k + ": {my} is not the coach's king"});
    const Arg* ln = b.line.arg("line");
    std::vector<Position> pos;
    std::vector<Move> moves;
    if (!ln || !playLine(c.p1, ln->san, pos, moves) || moves.empty()) {
        out.push_back({"STALEMATE_TRICK_LINE", k + ": {line} " + (ln ? ln->san : std::string("missing")) + " does not play"});
        return;
    }
    if (!coachStalemated(c, pos.back()))
        out.push_back({"STALEMATE_TRICK_NONE", k + ": " + ln->san + " ends without the coach stalemated"});
    bool took = false;
    for (size_t i = 0; i < moves.size(); ++i) {
        const Position& before = pos[i];
        if (before.sideToMove() != c.human) continue;
        const bool capture = !before.at(moves[i].to).empty() || (moves[i].flags & MoveEnPassant);
        took = took || capture;
        const bool only = before.legalMoves().size() == 1;
        const bool takesMover = capture && i > 0 && moves[i].to == moves[i - 1].to;
        if (!only && !before.inCheck() && !takesMover)
            out.push_back({"STALEMATE_TRICK_UNFORCED", k + ": in " + ln->san + ", " + before.toSAN(moves[i]) +
                                                           " is a free choice of the human's"});
    }
    if (!took) out.push_back({"STALEMATE_TRICK_NOTHING_GIVEN", k + ": the human takes nothing along " + ln->san});
    if (materialBalance(c.p1, c.human) < 1)
        out.push_back({"STALEMATE_TRICK_NOT_BEHIND", k + ": the coach is not behind in material on the table (" +
                                                         std::to_string(materialBalance(c.p1, c.human)) + ")"});
}

// Moves a pawn on 'from' needs to reach the last rank of its colour: the rank distance, one less from
// its first square when the double step is open.
int pawnMovesTo(const Position& p, Square from, Color side) {
    const int last = side == White ? 7 : 0, start = side == White ? 1 : 6, up = side == White ? 1 : -1;
    int n = std::abs(last - rankOf(from));
    if (rankOf(from) == start && p.at(makeSquare(fileOf(from), start + up)).empty() &&
        p.at(makeSquare(fileOf(from), start + 2 * up)).empty())
        n -= 1;
    return n;
}

int kingSteps(Square a, Square b) { return std::max(std::abs(fileOf(a) - fileOf(b)), std::abs(rankOf(a) - rankOf(b))); }

// The coach's pawn race (ex.promotion): {sq} is on the coach's last rank, and the coach promotes there
// along the refutation, with the very pawn {my} names (followed from its square on the table, not
// taken on the way), and the human does not take the new piece at once. {your}, the human's king, is
// outside the square: more king steps to {sq} than moves the pawn needs, the coach to move ({n} says
// those moves, or the squares when a phrasing says "squares"; {n2} the king's steps). A {line} said
// reaches the promotion.
void checkPromotion(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("ex.promotion.", 0) != 0) return;
    const Arg* sq = b.line.arg("sq");
    const Arg* my = b.line.arg("my");
    const Arg* your = b.line.arg("your");
    if (!sq || sq->square == NoSquare || !my || my->square == NoSquare || !your || your->square == NoSquare) {
        out.push_back({"PROMOTION_ARGS", k});
        return;
    }
    const Square to = sq->square;
    if (rankOf(to) != (c.coach == White ? 7 : 0))
        out.push_back({"PROMOTION_SQ", k + ": " + squareName(to) + " is not on the coach's last rank"});
    if (c.p1.at(my->square) != Piece{Pawn, c.coach})
        out.push_back({"PROMOTION_PAWN", k + ": no pawn of the coach's on " + squareName(my->square)});
    // The pawn followed along the refutation.
    Square at = my->square;
    int promotedAt = -1;
    bool samePawn = false;
    for (size_t i = 0; i < c.refut.size(); ++i) {
        const LineStep& st = c.refut[i];
        const bool mine = st.mover == c.coach && st.move.from == at;
        if (mine) at = st.move.to;
        else if (st.mover == c.human && st.move.to == at) at = NoSquare;
        if (st.mover == c.coach && st.promotion != NoPiece && st.move.to == to) {
            promotedAt = int(i);
            samePawn = mine;
            break;
        }
    }
    if (promotedAt < 0) {
        out.push_back({"PROMOTION_NONE", k + ": the coach never promotes on " + squareName(to) + " along " +
                                             sanLine(c.refut, 0, c.refut.size())});
        return;
    }
    if (!samePawn)
        out.push_back({"PROMOTION_OTHER_PAWN", k + ": the pawn on " + squareName(my->square) + " is not the one that queens on " +
                                                   squareName(to)});
    // A line that under-promotes while a phrasing says "queen": queening was there too (counted).
    if (c.refut[size_t(promotedAt)].promotion != Queen && anyVariantHas(k, "queen"))
        out.push_back({"PROMOTION_UNDER", k + ": the line promotes to a " + pieceName(c.refut[size_t(promotedAt)].promotion) +
                                              " (" + c.refut[size_t(promotedAt)].san + "), a phrasing says queen", false});
    if (size_t(promotedAt + 1) < c.refut.size() && c.refut[size_t(promotedAt + 1)].move.to == to &&
        c.refut[size_t(promotedAt + 1)].mover == c.human)
        out.push_back({"PROMOTION_CAUGHT", k + ": " + c.refut[size_t(promotedAt + 1)].san + " takes the new piece"});
    // The rule of the square, the coach to move on the table.
    const Square hk = c.p1.kingSquare(c.human);
    if (your->square != hk) out.push_back({"PROMOTION_KING", k + ": {your} is not the human's king"});
    const int need = pawnMovesTo(c.p1, my->square, c.coach), steps = kingSteps(hk, to);
    if (steps <= need)
        out.push_back({"PROMOTION_IN_SQUARE", k + ": the king on " + squareName(hk) + " is " + std::to_string(steps) +
                                                  " steps from " + squareName(to) + ", the pawn needs " + std::to_string(need)});
    for (const std::string& v : variantsOf(k)) {
        const Arg* n = b.line.arg("n");
        const Arg* n2 = b.line.arg("n2");
        const int squares = std::abs(rankOf(to) - rankOf(my->square));
        if (usesPlaceholder(v, "n") && n) {
            const bool saysSquares = v.find("squares from") != std::string::npos;
            if (n->number != (saysSquares ? squares : need))
                out.push_back({"PROMOTION_COUNT", k + ": {n} = " + std::to_string(n->number) + ", the pawn is " +
                                                      std::to_string(squares) + " squares and " + std::to_string(need) +
                                                      " moves away: \"" + v + "\""});
        }
        if (usesPlaceholder(v, "n2") && n2 && n2->number != steps)
            out.push_back({"PROMOTION_COUNT", k + ": {n2} = " + std::to_string(n2->number) + ", the king needs " +
                                                  std::to_string(steps)});
    }
    if (anyVariantUses(k, "line")) {
        const Arg* ln = b.line.arg("line");
        std::vector<Position> pos;
        std::vector<Move> moves;
        bool reached = false;
        if (ln && playLine(c.p1, ln->san, pos, moves))
            for (size_t i = 0; i < moves.size(); ++i)
                reached = reached || (pos[i].sideToMove() == c.coach && moves[i].promotion != NoPiece && moves[i].to == to);
        if (!reached)
            out.push_back({"PROMOTION_LINE_SHORT", k + ": {line} " + (ln ? ln->san : std::string("missing")) +
                                                       " stops before the promotion on " + squareName(to)});
    }
}

// "Your pawn could have run to {sq}" (ex.promotion_missed): the best line from before the move has a
// pawn of the human's promote on {sq}; where a phrasing says it queens or wins a race ("queen",
// "race", "first", "yours"), the new piece stays there (taken at once, it won no race: lost, or a
// decoy the human takes back at once). The played line does not promote there as soon and keep it.
void checkPromotionMissed(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("ex.promotion_missed.", 0) != 0) return;
    const Arg* sq = b.line.arg("sq");
    if (!sq || sq->square == NoSquare) {
        out.push_back({"PROMOTION_ARGS", k});
        return;
    }
    // The first promotion of the human's on {sq} along a line; 'kept': the next ply does not take the
    // new piece there.
    const auto promotesAt = [&](const std::vector<LineStep>& line, bool& kept) {
        for (size_t i = 0; i < line.size(); ++i)
            if (line[i].mover == c.human && line[i].promotion != NoPiece && line[i].move.to == sq->square) {
                kept = !(i + 1 < line.size() && line[i + 1].move.to == sq->square && line[i + 1].captured != NoPiece);
                return int(i);
            }
        return -1;
    };
    bool bestKept = false, playedKept = false;
    const int best = promotesAt(c.bestLine, bestKept), played = promotesAt(c.playedLine, playedKept);
    if (best < 0)
        out.push_back({"PROMOTION_MISSED_NONE", k + ": the best line " + sanLine(c.bestLine, 0, c.bestLine.size()) +
                                                    " never promotes on " + squareName(sq->square)});
    else if (!bestKept && (anyVariantHas(k, "queen") || anyVariantHas(k, "race") || anyVariantHas(k, "first") ||
                           anyVariantHas(k, "yours"))) {
        const bool back = size_t(best) + 2 < c.bestLine.size() && c.bestLine[size_t(best) + 2].move.to == sq->square &&
                          c.bestLine[size_t(best) + 2].captured != NoPiece;
        out.push_back({back ? "PROMOTION_MISSED_DEFLECTION" : "PROMOTION_MISSED_TAKEN",
                       k + ": " + sanLine(c.bestLine, size_t(best), back ? 3 : 2) + " along the best line: the new piece is taken at once" +
                           (back ? " and taken back" : "")});
    }
    else if (played >= 0 && playedKept && played <= best)
        out.push_back({"PROMOTION_MISSED_PLAYED", k + ": the played line promotes on " + squareName(sq->square) +
                                                      " as soon (" + sanLine(c.playedLine, 0, size_t(played) + 1) + ")"});
}

// "Push the passed pawn" (ex.push_passed): {best}, legal before the move, moves a pawn of the human's
// straight up its file; that pawn is passed (no pawn of the coach's ahead of it on its file or the two
// next to it); {sq} is the last-rank square of its file.
void checkPushPassed(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("ex.push_passed.", 0) != 0) return;
    const Arg* best = b.line.arg("best");
    const Move m = best ? c.p0.parseUCI(best->uci) : Move();
    if (!m.valid() || c.p0.at(m.from) != Piece{Pawn, c.human}) {
        out.push_back({"PUSH_NOT_PAWN", k + ": {best} " + (best ? best->san : std::string("missing")) + " moves no pawn of the human's"});
        return;
    }
    if (fileOf(m.to) != fileOf(m.from))
        out.push_back({"PUSH_CAPTURE", k + ": " + best->san + " leaves the pawn's file"});
    const int up = c.human == White ? 1 : -1;
    for (Square s : squaresOf(c.p0.pieces(c.coach, Pawn)))
        if (std::abs(fileOf(s) - fileOf(m.from)) <= 1 && (rankOf(s) - rankOf(m.from)) * up > 0) {
            out.push_back({"PUSH_NOT_PASSED", k + ": the pawn on " + squareName(m.from) + " has the coach's pawn on " +
                                                  squareName(s) + " ahead"});
            break;
        }
    const Arg* sq = b.line.arg("sq");
    const Square last = makeSquare(fileOf(m.to), c.human == White ? 7 : 0);
    if (anyVariantUses(k, "sq") && (!sq || sq->square != last))
        out.push_back({"PUSH_SQ", k + ": {sq} " + (sq ? squareName(sq->square) : std::string("missing")) + ", the pawn queens on " +
                                      squareName(last)});
}

// "Bring your king to the centre / forward" (ex.king_active): {best}, legal before the move, is a king
// move of the human's that ends nearer the middle of the board than it starts; where a phrasing says
// "forward", the king also steps toward the coach's side.
void checkKingActive(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("ex.king_active.", 0) != 0) return;
    const Arg* best = b.line.arg("best");
    const Move m = best ? c.p0.parseUCI(best->uci) : Move();
    if (!m.valid() || c.p0.at(m.from) != Piece{King, c.human}) {
        out.push_back({"KING_ACTIVE_NOT_KING", k + ": {best} " + (best ? best->san : std::string("missing")) + " is no king move"});
        return;
    }
    // Twice the distance to the board's middle point, squared.
    auto far = [](Square s) {
        const int x = 2 * fileOf(s) - 7, y = 2 * rankOf(s) - 7;
        return x * x + y * y;
    };
    if (far(m.to) >= far(m.from))
        out.push_back({"KING_ACTIVE_NOT_CENTRE", k + ": " + best->san + " goes no nearer the centre"});
    // "Forward": a step back toward the human's own side contradicts it; a step along the rank only
    // stretches it (counted, not an error).
    const int up = c.human == White ? 1 : -1, ahead = (rankOf(m.to) - rankOf(m.from)) * up;
    if (anyVariantHas(k, "forward") && ahead < 0)
        out.push_back({"KING_ACTIVE_BACKWARD", k + ": " + best->san + " steps back, a phrasing says \"forward\""});
    else if (anyVariantHas(k, "forward") && ahead == 0)
        out.push_back({"KING_ACTIVE_SIDEWAYS", k + ": " + best->san + " steps sideways, a phrasing says \"forward\"", false});
}

// "Kd5 takes the opposition" (ex.opposition): {best}, legal before the move, is a king move of the
// human's after which the two kings stand on one file or one rank with one square between them, the
// coach to move; the move played did not leave the kings so (it would hold the opposition too).
void checkOpposition(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("ex.opposition.", 0) != 0) return;
    const Arg* best = b.line.arg("best");
    const Move m = best ? c.p0.parseUCI(best->uci) : Move();
    if (!m.valid() || c.p0.at(m.from) != Piece{King, c.human}) {
        out.push_back({"OPPOSITION_NOT_KING", k + ": {best} " + (best ? best->san : std::string("missing")) + " is no king move"});
        return;
    }
    auto facing = [&](const Position& p) {
        const Square a = p.kingSquare(White), d = p.kingSquare(Black);
        const int df = std::abs(fileOf(a) - fileOf(d)), dr = std::abs(rankOf(a) - rankOf(d));
        return p.sideToMove() == c.coach && ((df == 0 && dr == 2) || (dr == 0 && df == 2));
    };
    Position q = c.p0;
    q.makeMove(m);
    if (!facing(q)) out.push_back({"OPPOSITION_NONE", k + ": after " + best->san + " the kings do not face each other"});
    if (facing(c.p1)) out.push_back({"OPPOSITION_PLAYED_TOO", k + ": the move played leaves the kings facing too"});
}

// "Careful: fifty moves without a capture or a pawn move is a draw" (tip.fifty): the plies since the
// last capture or pawn move, counted on the game's own moves (from the start position's clock), are
// near the limit: 80 at least (ten moves left at most), and fewer than 100 (else the draw is there).
void checkFiftyTip(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("tip.fifty.", 0) != 0) return;
    int clock = c.g.positionAt(0).halfmoveClock();
    for (size_t i = 0; i < c.g.moves().size(); ++i) {
        const Position& p = c.g.positionAt(i);
        const Move& m = c.g.moves()[i];
        const bool reset = p.at(m.from).type == Pawn || !p.at(m.to).empty();
        clock = reset ? 0 : clock + 1;
    }
    if (clock < 80 || clock >= 100)
        out.push_back({"FIFTY_CLOCK", k + ": " + std::to_string(clock) + " plies without a capture or a pawn move"});
}

// "If the same position comes a third time, it's a draw" (tip.repetition): the position after the
// move stood on the board exactly once before in the game (placement, side to move, castling rights,
// en passant square: the first four fields of the FEN).
void checkRepetitionTip(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("tip.repetition.", 0) != 0) return;
    auto id = [](const Position& p) {
        const std::string f = p.fen();
        size_t at = 0;
        for (int i = 0; i < 4 && at != std::string::npos; ++i) at = f.find(' ', at + 1);
        return f.substr(0, at);
    };
    const std::string now = id(c.p1);
    int seen = 0;
    for (size_t i = 0; i <= c.g.moves().size(); ++i) seen += id(c.g.positionAt(i)) == now;
    if (seen != 2) out.push_back({"REPETITION_COUNT", k + ": the position stands " + std::to_string(seen) + " times in the game"});
}

// "With a lone king left, push it to the edge, then checkmate" (tip.mate_technique): after the move
// the coach has its king alone, and the human has the material to force a mate (a queen, a rook, a
// pawn, a bishop and a knight, or bishops on both colours).
void checkMateTechnique(const Ctx& c, const Beat& b, std::vector<Finding>& out) {
    const std::string& k = b.line.key;
    if (k.rfind("tip.mate_technique.", 0) != 0) return;
    if (c.p1.pieces(c.coach) != c.p1.pieces(c.coach, King))
        out.push_back({"MATE_TECH_NOT_LONE", k + ": the coach has more than its king"});
    const Color h = c.human;
    bool light = false, dark = false;
    for (Square s : squaresOf(c.p1.pieces(h, Bishop))) ((fileOf(s) + rankOf(s)) % 2 ? light : dark) = true;
    const bool enough = c.p1.pieces(h, Queen) || c.p1.pieces(h, Rook) || c.p1.pieces(h, Pawn) || (light && dark) ||
                        ((light || dark) && c.p1.pieces(h, Knight));
    if (!enough) out.push_back({"MATE_TECH_NO_MATERIAL", k + ": the human has no material to force a mate"});
}

}  // namespace

// ---- One move at one level ---------------------------------------------------------------------------

namespace {

struct Stats {
    std::map<std::string, int> codes;
    std::map<std::string, int> types;   // voiced explanation types
    int reviews = 0, scripts = 0;
    // --vcache: the reviews whose reason was checked, kept as found, made smaller, dropped (and of
    // those, for want of searches: the budget spent), the searches they asked for.
    int checked = 0, kept = 0, reduced = 0, dropped = 0, budgetOut = 0, searches = 0;
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
    std::unique_ptr<MoveJudge> judge;
    if (o.verify > 0) {
        judge.reset(new MoveJudge(o.verify, o.threads, o.cache));
        if (!judge->ok()) std::fprintf(stderr, "engine unavailable: moves judged from the cache only\n");
    }
    std::unique_ptr<SequenceSearches> searches;
    if (!o.vcache.empty()) {
        searches.reset(new SequenceSearches(1, o.vcache));
        if (!searches->ok()) std::fprintf(stderr, "engine unavailable: sequences confirmed from the cache only\n");
    }
    Stats st;
    // --sequence: the Reviewers of the game under way, per level and side.
    std::map<std::pair<int, int>, Reviewer> seq;
    std::string seqStart;
    std::vector<std::string> seqMoves;   // the last record's moves and its move played
    auto printScript = [&](const char* kind, const Script& s) {
        for (const Beat& b : s)
            if (!b.line.key.empty()) std::fprintf(out, "  %s [%s] %s\n", kind, b.line.key.c_str(), render(b.line).c_str());
    };
    for (size_t ri = 0; ri < recs.size(); ++ri) {
        const Record& r = recs[ri];
        if (o.sequence) {
            // A new game unless this record continues the last one's moves.
            const bool goesOn = r.startFen == seqStart && seqMoves.size() <= r.moves.size() &&
                                std::equal(seqMoves.begin(), seqMoves.end(), r.moves.begin());
            if (!goesOn) seq.clear();
            seqStart = r.startFen;
            seqMoves = r.moves;
            seqMoves.push_back(r.played);
        }
        for (int level = 1; level <= 6; ++level) {
            if (o.level && level != o.level) continue;
            Ctx c;
            if (!buildCtx(r, level, c)) continue;
            Reviewer single;
            Reviewer* rvp = &single;
            Script pre;   // --sequence: the other side's move, as the coach's
            std::vector<Finding> preF;
            if (o.sequence) {
                const auto key = std::make_pair(level, int(c.human));
                auto it = seq.find(key);
                if (it == seq.end()) {
                    it = seq.emplace(key, Reviewer()).first;
                    it->second.reset(level, c.human);
                }
                rvp = &it->second;
                Game before = c.g;
                before.undo(1);
                if (!before.moves().empty()) {
                    for (const Beat& b : rvp->announce(before)) pre.push_back(b);
                    // --vcache: the threat search a Coach game runs after the coach's move (levels 1-2).
                    const std::string threatKey = searches ? rvp->threatKey(before) : std::string();
                    const ai::Analysis* threat = threatKey.empty() ? nullptr : &searches->get(threatKey);
                    for (const Beat& b : rvp->coachMoved(before, &r.a0, threat && threat->ok ? threat : nullptr)) {
                        pre.push_back(b);
                        // "My knight is attacking your queen": it does, on the board the player sees.
                        if (b.line.key.rfind("threat.piece", 0) == 0) {
                            const Arg* my = b.line.arg("my");
                            const Arg* your = b.line.arg("your");
                            if (!my || !your || c.p0.at(your->square).color != c.human ||
                                c.p0.at(your->square).type != your->piece || c.p0.at(my->square).type != my->piece ||
                                !(c.p0.attackersTo(your->square, c.coach) & squareBit(my->square)))
                                preF.push_back({"THREAT_PIECE", b.line.key + ": no such attack on the board"});
                            // A capture the coach could make next (the turn passed): not by a pinned piece.
                            Position q = c.p0;
                            bool legal = false;
                            if (my && your && q.passTurn())
                                for (const Move& m : q.legalMoves()) legal = legal || (m.from == my->square && m.to == your->square);
                            else if (my && your)
                                legal = !(c.p0.pinned(c.coach) & squareBit(my->square));
                            if (my && your && !legal)
                                preF.push_back({"THREAT_PINNED", b.line.key + ": " + squareName(my->square) + " cannot take"});
                        }
                        // "I'm threatening checkmate on g1": with the turn passed, the piece named mates
                        // there at once.
                        if (b.line.key.rfind("threat.mate", 0) == 0) {
                            const Arg* sq = b.line.arg("sq");
                            const Arg* my = b.line.arg("my");
                            Position q = c.p0;
                            if (sq && q.passTurn()) {
                                bool mates = false;
                                for (const Move& m : q.legalMoves()) {
                                    if (m.to != sq->square || (my && my->square != NoSquare && m.from != my->square)) continue;
                                    Position r2 = q;
                                    r2.makeMove(m);
                                    mates = mates || r2.isCheckmate();
                                }
                                if (!mates) preF.push_back({"MATE_THREAT_NONE", b.line.key + ": no mate on " + squareName(sq->square)});
                            }
                        }
                        // "My last move was a mistake": the human's best line keeps 2 points more
                        // than before the coach's move, four plies on (the capture answered).
                        if (b.line.key != "threat.punish.b1" || !r.a0.ok || r.a0.lines.empty()) continue;
                        const int start = materialBalance(before.positionAt(before.moves().size() - 1), c.human);
                        const std::vector<LineStep> l = replayLine(c.p0, r.a0.lines[0].pv, c.human, 16);
                        const int kept = l.empty() ? start : l[std::min<size_t>(3, l.size() - 1)].balance;
                        if (kept - start < 2)
                            preF.push_back({"PUNISH_NONE", "threat.punish.b1: the best line keeps " +
                                                               std::to_string(kept - start) + " points from before my move"});
                    }
                }
                for (const Beat& b : rvp->announce(c.g)) pre.push_back(b);
            } else {
                single.reset(level, c.human);
            }
            Reviewer& rv = *rvp;
            ReviewInput ri2;
            ri2.game = &c.g;
            ri2.before = &r.a0;
            ri2.played = r.hasA1 ? &r.a1 : nullptr;
            ri2.after = level >= 4 && r.hasA2 ? &r.a2 : nullptr;
            ri2.shallow = level == 3 || level == 4 ? &r.a3 : nullptr;
            ri2.inBook = OpeningBook::instance().lookup(c.p1.hash());
            // --vcache: as a Coach game does (coach/session.cpp), a reason the review would say has its
            // sequence confirmed first, the positions it asks for searched one after another.
            std::map<std::string, ai::Analysis> confirmed;
            int asked = 0;
            bool budgetOut = false;
            if (searches && rv.probe(ri2).claim.type != ExType::None) {
                ri2.verified = &confirmed;
                for (;;) {
                    const std::string next = rv.probe(ri2).verifyNext;
                    if (next.empty()) break;
                    if (asked >= Reviewer::kVerifyMax) {
                        budgetOut = true;
                        break;
                    }
                    confirmed[next] = searches->get(next);
                    ++asked;
                }
            }
            const Review rev = rv.review(ri2);
            if (ri2.verified) {
                ++st.checked;
                st.searches += asked;
                if (rev.dropped.type == ExType::None) ++st.kept;
                else if (rev.claim.type != ExType::None) ++st.reduced;
                else ++st.dropped;
                if (budgetOut) ++st.budgetOut;
            }
            // The oracles read the line the reason was found on, and the better move the review named
            // (the most natural of the moves as good as the best).
            if (!rev.refutation.empty()) c.refut = replayLine(c.p1, rev.refutation, c.human, 16);
            if (!rev.verdict.bestUci.empty() && !c.bestLine.empty() && c.bestLine[0].uci != rev.verdict.bestUci)
                if (const ai::PvLine* l = c.rec->a0.line(rev.verdict.bestUci)) {
                    c.bestLine = replayLine(c.p0, l->pv, c.human, 16);
                    c.bestEnd = detail::lineEnd(c.p0, c.bestLine);
                }
            ++st.reviews;
            Script declined;   // --sequence: the game goes on as played
            if (o.sequence)
                for (const Beat& b : rev.script)
                    if (b.kind == BeatKind::OfferTakeback) {
                        declined = rv.takebackDeclined();
                        break;
                    }
            const PlyVerdict& v = rev.verdict;
            std::vector<Finding> f = preF;

            // The demonstration's moves, none of them leaving a bigger prize standing unexplained.
            {
                Position q = c.p1;
                std::vector<Move> moves;
                std::vector<std::string> keys;
                for (const Beat& b : rev.script) {
                    if (b.kind == BeatKind::Rewind) break;
                    if (b.kind != BeatKind::DemoMove) continue;
                    const Move m = q.parseUCI(b.uci);
                    if (!m.valid()) break;
                    moves.push_back(m);
                    keys.push_back(b.line.key);
                    q.makeMove(m);
                }
                checkLinePrize(c.p1, moves, &keys, "demo", false, f);
                if (judge) checkLinePlausible(*judge, c.p1, moves, "demo", false, f);
            }

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
                    checkWon(c, b, size_t(demoPlies), f);
                    checkFree(c, b, f);
                    checkTrapped(c, b, f);
                    checkSacrifice(c, b, f);
                    checkSaved(c, b, f);
                    checkPraiseFork(c, b, f);
                    checkPraiseCapture(c, b, f);
                    checkPraiseTrade(c, b, f);
                    checkPraiseLost(c, v, b, f);
                    checkLineRepeats(c, b, f);
                    checkKingLine(c, b, f);
                    checkGivenAway(c, v, b, demoPlies, f);
                    checkBishopPair(c, v, b, f);
                    checkFork(c, b, f);
                    checkSkewer(c, b, f);
                    checkPin(c, b, f);
                    checkDiscovered(c, b, f);
                    checkBackRank(c, b, f);
                    checkBackRankTip(c, b, f);
                    checkExchangeCheap(c, b, f);
                    checkBadTrade(c, v, b, f);
                    checkMatePattern(c, b, f);
                    checkMateCount(c, b, f);
                    checkStalemate(c, b, f);
                    checkStalemateTrick(c, b, f);
                    checkPromotion(c, b, f);
                    checkPromotionMissed(c, b, f);
                    checkPushPassed(c, b, f);
                    checkKingActive(c, b, f);
                    checkOpposition(c, b, f);
                    checkFiftyTip(c, b, f);
                    checkRepetitionTip(c, b, f);
                    checkMateTechnique(c, b, f);
                    checkLinePrizes(c, b, f);
                    if (judge) {
                        Position start;
                        std::vector<Move> moves;
                        bool fromBest = false;
                        if (saidLine(c, b, start, moves, fromBest)) {
                            // A line from p0 may start with the move played (the fault itself) or the
                            // better move: neither is judged.
                            const bool fromPlayed = start.fen() == c.p0.fen() && moves[0] == c.p0.parseUCI(r.played);
                            checkLinePlausible(*judge, start, moves, b.line.key + " {line}", fromBest || fromPlayed, f);
                        }
                    }
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
                    k.rfind("ex.better.", 0) != 0 && k.rfind("ex.point.", 0) != 0 && k.rfind("ex.result.", 0) != 0 &&
                    k.rfind("ex.hint.", 0) != 0 && k.rfind("ex.reveal.", 0) != 0)
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
            // A mistake (not a blunder) is said in words by design (review.cpp): only blunders.
            if (explained && !demo && level <= 2 && (isMaterialType(t) || t == ExType::MateAllowed) &&
                v.cls == MoveClass::Blunder)
                f.push_back({"NO_DEMO_BEGINNER", exTypeName(t), false});

            // Policy: small slips are not dramatised.
            if (explained && v.cls == MoveClass::Inaccuracy && demo &&
                worstLoss(c.playedLine, c.p0, c.human, c.base, 9) < 3)
                f.push_back({"INACCURACY_DEMO", exTypeName(t)});
            // A dubious move (an inaccuracy) gets a remark and nothing more (review.cpp): the slip that
            // names the better move, "that move looks dubious", a tip, or at levels 1-2 "maybe you can
            // do better?" said with the takeback card. Levels 1-2 keep their word and offer for a free
            // piece or a mate missed.
            const bool missedKept = level <= 2 && (t == ExType::MissedCapture || t == ExType::MateMissed);
            bool dubiousOffer = false;
            for (const Beat& b : rev.script)
                if (b.kind == BeatKind::OfferTakeback && b.line.key.rfind("ex.dubious.", 0) == 0) dubiousOffer = true;
            if (dubiousOffer && (v.cls != MoveClass::Inaccuracy || level > 2))
                f.push_back({"DUBIOUS_OFFER", std::string(className(v.cls)) + " at level " + std::to_string(level)});
            if (v.cls == MoveClass::Inaccuracy && !missedKept)
                for (const Beat& b : rev.script) {
                    const std::string& k = b.line.key;
                    const bool remark = k.rfind("ex.verdict.inaccuracy.", 0) == 0 || k == "ex.verdict.dubious" ||
                                        k.rfind("tip.", 0) == 0 ||
                                        (level <= 2 && b.kind == BeatKind::OfferTakeback && k.rfind("ex.dubious.", 0) == 0);
                    if (!remark) {
                        f.push_back({"DUBIOUS_MORE", k.empty() ? "beat kind " + std::to_string(int(b.kind)) : k});
                        break;
                    }
                }
            // Levels 1-2 offer a free piece or a mate missed back whatever it costs (review.cpp).
            if (offer && !dubiousOffer && v.cls == MoveClass::Inaccuracy && !missedKept)
                f.push_back({"OFFER_INACCURACY", exTypeName(t)});
            if (offer && !dubiousOffer && v.cls != MoveClass::Blunder && t != ExType::MateAllowed &&
                t != ExType::MateMissed && t != ExType::Stalemate && t != ExType::MissedCapture)
                f.push_back({"OFFER_NOT_BLUNDER", className(v.cls)});
            if (offer && !dubiousOffer && v.wPlayed >= 75.0)
                f.push_back({"OFFER_STILL_WINNING", std::to_string(int(v.wPlayed)), false});
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

            // The hint names the piece the better move moves (where it stands now), and the reveal
            // that move itself; neither is said without a better move.
            for (const Beat& b : rev.script) {
                const bool hint = b.line.key.rfind("ex.hint.", 0) == 0, reveal = b.line.key.rfind("ex.reveal.", 0) == 0;
                if (!hint && !reveal) continue;
                if (c.bestLine.empty() || v.cls != MoveClass::Blunder) {
                    f.push_back({"HINT_UNDUE", b.line.key});
                    continue;
                }
                const Move bm = c.bestLine[0].move;
                const Arg* your = b.line.arg("your");
                const Move pm = c.p0.parseUCI(c.rec->played);
                const Square want = hint && bm.from == pm.from ? pm.to : bm.from;
                if (your && (your->piece != c.bestLine[0].piece || your->square != want))
                    f.push_back({"HINT_PIECE", b.line.key + " names " + pieceName(your->piece) + " on " +
                                                   squareName(your->square) + ", the better move is " + c.bestLine[0].san});
                if (const Arg* sq = b.line.arg("sq"); reveal && sq && sq->square != bm.to)
                    f.push_back({"HINT_PIECE", b.line.key + " names " + squareName(sq->square) + ", the better move is " +
                                                   c.bestLine[0].san});
            }

            // A hint that says the better move wins material: its line still has the human ahead
            // where it settles (after 8 plies, the next four read too), and the move played wins
            // and loses nothing for good along its own line.
            for (const Beat& b : rev.script) {
                if (b.line.key.rfind("ex.hint.gain.", 0) != 0) continue;
                const int bestGain = settledLoss(c.bestLine, c.p0, c.coach, -c.base, 8);
                const int playedGain = lastingLoss(c.playedLine, c.p0, c.coach, -c.base, 12);
                const int playedLoss = lastingLoss(c.playedLine, c.p0, c.human, c.base, 12);
                if (bestGain < 1 || playedGain > 0 || playedLoss > 0)
                    f.push_back({"HINT_GAIN", b.line.key + ": the best line wins " + std::to_string(bestGain) +
                                                  ", the played line wins " + std::to_string(playedGain) +
                                                  " and loses " + std::to_string(playedLoss)});
            }

            // The loss the explanation names is one the best move avoids: where the best line settles
            // (what is traded back is no loss), and the named piece not given up by the best line too.
            if (explained && fault && (isMaterialType(t) || isGenericType(t)) && c.lp) {
                const int bestLoss = lastingLoss(c.bestLine, c.p0, c.human, c.base, 8);
                const int refLoss = lastingLoss(c.playedLine, c.p0, c.human, c.base, 9);
                // A loss the best line wins back later than the windows read (Nxd4 Bh5+ Bg6 Bxg6+
                // hxg6 Bxd4; Bxc8 ... Bxa1 Rxa1 Qxc8) is none: the best line is not down where it ends.
                const int bestEndLoss = endLoss(c.bestLine, c.p0, c.human, c.base);
                // Nor one it takes back after in-between checks ("fxg8=Q Qg3+ Kc2 d3+ Kd1 Rxg8"):
                // the least of the two is what the best line loses.
                if (isMaterialType(t) && bestLoss >= 2 && bestEndLoss >= 2 && refLoss - std::min(bestLoss, bestEndLoss) < 2)
                    f.push_back({"AVOIDABLE?", "best line loses " + std::to_string(bestLoss) + ", played line " +
                                                   std::to_string(refLoss)});
                if (isMaterialType(t) && bestLoss >= 1)
                    for (const Beat& b : rev.script)
                        for (const auto& a : b.line.args) {
                            const Arg& x = a.second;
                            if (b.kind != BeatKind::Say || x.kind != Arg::Kind::Piece || x.color != c.human ||
                                x.square == NoSquare || x.piece == King || b.line.key.rfind("ex.better.", 0) == 0 ||
                                b.line.key.rfind("ex.hint.", 0) == 0 || b.line.key.rfind("ex.reveal.", 0) == 0 ||
                                (b.line.key.rfind("ex.retake.", 0) == 0 && a.first == "your2") ||   // the taker
                                // A bad capture says what the capturing piece is traded for ("your queen
                                // for 3"), whatever the better move trades it for; a count names the
                                // piece on the square the exchange is about (the loss is the exchange's,
                                // checked above), whatever the better move does with it later.
                                (b.line.key.rfind("ex.exchange_capture.", 0) == 0 && a.first == "your") ||
                                (b.line.key.rfind("ex.exchange_count.", 0) == 0 && a.first == "your"))
                                continue;
                            const Move pm = c.p0.parseUCI(c.rec->played);
                            const Square onP0 = x.square == pm.to && c.p0.at(pm.from).type == x.piece ? pm.from : x.square;
                            if (c.p0.at(onP0).type != x.piece || c.p0.at(onP0).color != c.human) continue;
                            // A piece traded for as much elsewhere ("Rxe5 Bxh8") is not given up: the
                            // best line must lose about what the piece is worth (a pawn's point, else two).
                            const int anyway = lostAnyway(c.bestLine, c.p0, c.human, onP0);
                            if (anyway >= 1 && std::min(bestLoss, bestEndLoss) >= std::min(anyway, 2))
                                f.push_back({"AVOIDABLE?", b.line.key + " {" + a.first + "} " + pieceName(x.piece) +
                                                               " on " + squareName(onP0) + " is given up by the best line too"});
                        }
                bool mated = false;
                for (const LineStep& s : c.playedLine)
                    if (s.mate && s.mover == c.coach) mated = true;
                // King safety is about mates: only a material loss makes it the wrong reason, one the
                // coach's own refutation shows too (where it shows the take-back, the king exposed
                // after it is what that line says).
                int coachLoss = refLoss;
                if (t == ExType::KingSafety) {
                    std::vector<LineStep> line;
                    if (!c.playedLine.empty()) line.push_back(c.playedLine[0]);
                    line.insert(line.end(), c.refut.begin(), c.refut.end());
                    coachLoss = lastingLoss(line, c.p0, c.human, c.base, 9);
                }
                if (isGenericType(t) && (v.cls != MoveClass::Inaccuracy) &&
                    (std::min(refLoss, coachLoss) - bestLoss >= 2 || (mated && t != ExType::KingSafety)))
                    f.push_back({"GENERIC_REASON", std::string(exTypeName(t)) + " while the played line loses " +
                                                       std::to_string(refLoss) + (mated ? " (mate)" : "")});
            }

            // What a king-safety line says, read on the board: a shield pawn moved ("that pawn
            // protected your king": the pawn stood next to the king, one or two ranks up); a king said
            // open (exposed, no shelter, too open) just walked out with the queens on, or has at most
            // one pawn of its own in front of it (the three files, two ranks), or stands off its first
            // two ranks, or faces two checks within eight plies of the line; checks said ("many
            // checks", "with checks") are two at least.
            for (const Beat& b : rev.script) {
                const std::string& k = b.line.key;
                const bool shieldKey = k.rfind("ex.king_shield.", 0) == 0, exposedKey = k.rfind("ex.king_exposed.", 0) == 0;
                if (b.kind != BeatKind::Say || (!shieldKey && !exposedKey)) continue;
                const Square ks = c.p1.kingSquare(c.human);
                const int up = c.human == White ? 1 : -1, home = c.human == White ? 0 : 7;
                int shelter = 0;
                for (int df = -1; df <= 1; ++df)
                    for (int dr = 1; dr <= 2; ++dr) {
                        const int f = fileOf(ks) + df, r = rankOf(ks) + dr * up;
                        if (f < 0 || f > 7 || r < 0 || r > 7) continue;
                        if (c.p1.at(makeSquare(f, r)) == Piece{Pawn, c.human}) ++shelter;
                    }
                const bool offHome = std::abs(rankOf(ks) - home) >= 2;
                int checks = 0;
                for (int i = 0; i < int(c.refut.size()) && i < 8; ++i) checks += c.refut[size_t(i)].mover == c.coach && c.refut[size_t(i)].check;
                const Move pm = c.p0.parseUCI(c.rec->played);
                const Square k0 = c.p0.kingSquare(c.human);
                if (shieldKey && !(c.p0.at(pm.from).type == Pawn && std::abs(fileOf(pm.from) - fileOf(k0)) <= 1 &&
                                   (rankOf(pm.from) - rankOf(k0)) * up >= 1 && (rankOf(pm.from) - rankOf(k0)) * up <= 2))
                    f.push_back({"KING_CLAIM", k + ": the move is no shield pawn's"});
                const bool walk = c.p0.at(pm.from).type == King && std::abs(fileOf(pm.to) - fileOf(pm.from)) <= 1 &&
                                  c.p0.pieces(c.human, Queen) && c.p0.pieces(c.coach, Queen);
                if (exposedKey && shelter >= 2 && !offHome && checks < 2 && !walk)
                    f.push_back({"KING_CLAIM", k + ": the king has " + std::to_string(shelter) + " pawns in front, " +
                                                   std::to_string(checks) + " checks follow"});
                if ((k.rfind("ex.king_exposed.b2", 0) == 0 || k.rfind("ex.king_exposed.b3", 0) == 0) && checks < 2 &&
                    render(b.line).find("check") != std::string::npos)
                    f.push_back({"KING_CLAIM", k + ": checks said, " + std::to_string(checks) + " in the line"});
            }

            // An explanation where one is due: a blunder the player can still repair (not lost before
            // it, not still winning after it) after which the engine's line wins material for good or
            // mates (or that misses such a win), and no line of the coach's says why. Levels 1-2 stay
            // silent by design when the reason lies beyond the level's reach: counted, not an error.
            if (v.cls == MoveClass::Blunder && v.wBest > 10.0 && v.wPlayed < 75.0 && !explained && c.lp) {
                const int bestLoss = lastingLoss(c.bestLine, c.p0, c.human, c.base, 8);
                const int refLoss = lastingLoss(c.playedLine, c.p0, c.human, c.base, 9);
                const int bestGain = worstLoss(c.bestLine, c.p0, c.coach, -c.base, 8);
                bool mated = false, mates = false;
                for (const LineStep& s : c.playedLine)
                    if (s.mate && s.mover == c.coach) mated = true;
                for (const LineStep& s : c.bestLine)
                    if (s.mate && s.mover == c.human) mates = true;
                std::string what = mated ? "the played line mates" :
                                   refLoss - bestLoss >= 2 ? "the played line loses " + std::to_string(refLoss - bestLoss) :
                                   mates ? "the best line mates" :
                                   bestGain >= 3 ? "the best line wins " + std::to_string(bestGain) : "";
                if (!what.empty())
                    f.push_back({"UNEXPLAINED", what + (rev.script.empty() ? ", nothing said" : ", no reason said"), false});
            }

            // What a {line} names goes as far as the point it makes: a material reason's line ends
            // with the human down (for good) or mated, or the retake line says why the take-back
            // the table shows there fails (checkRetake).
            {
                bool lineSaid = false;
                Position lineEnd;
                int named = 0;
                for (const Beat& b : rev.script) {
                    if (const Arg* p = b.line.arg("pts"); p && b.line.key.rfind("ex.retake.", 0) != 0) named = std::max(named, p->number);
                    Position start;
                    std::vector<Move> moves;
                    bool fromBest = false;
                    if (b.kind != BeatKind::Say || b.line.key.rfind("ex.better.", 0) == 0 ||
                        !saidLine(c, b, start, moves, fromBest) || fromBest || lineSaid)
                        continue;
                    lineSaid = true;
                    lineEnd = start;
                    for (const Move& m : moves) lineEnd.makeMove(m);
                }
                bool retakeHolds = false, retakeSaid = false;
                for (const Beat& b : rev.script)
                    if (b.kind == BeatKind::Say && b.line.key.rfind("ex.retake.", 0) == 0) {
                        retakeSaid = true;
                        // Where the sequence stops: the line said, else the demonstration's last move.
                        Position shownEnd = c.p1;
                        for (int i = 0; i < demoPlies && i < int(c.refut.size()); ++i) shownEnd.makeMove(c.refut[size_t(i)].move);
                        retakeHolds = checkRetake(c, b, named, lineSaid ? &lineEnd : demoPlies > 0 ? &shownEnd : nullptr,
                                                  judge.get(), f);
                    }
                if (retakeSaid && !(explained && isMaterialType(t)))
                    f.push_back({"RETAKE_UNDUE", std::string(exTypeName(t)) + (explained ? "" : ", no reason said")});
                if (explained && isMaterialType(t) && lineSaid && !retakeHolds && !lineEnd.isCheckmate() &&
                    heldLossNet(c, lineEnd) < 1)
                    f.push_back({"LINE_SHORT", "{line} ends with nothing lost"});
            }

            // A reason that names a target (a motif; a level 1-2 "I can win your pawn") while the
            // coach's reply itself takes another piece, as much or more, for good: that capture is the story ("Nxb3"
            // took the queen; "Qxc6" took a loose bishop, the fork on two pawns came after).
            if (explained && fault && (isMaterialType(t) && t != ExType::Hanging && t != ExType::Exchange) &&
                !c.refut.empty() && c.refut[0].mover == c.coach && c.refut[0].captured != NoPiece) {
                bool saysPts = false, named = false, kingNamed = false;
                int target = 0;
                const Square took = (c.refut[0].move.flags & MoveEnPassant)
                                        ? Square(c.refut[0].move.to + (c.human == White ? 8 : -8))
                                        : c.refut[0].move.to;
                for (const Beat& b : rev.script) {
                    const std::string& k = b.line.key;
                    if (b.kind != BeatKind::Say || k.rfind("ex.", 0) != 0 || k.rfind("ex.verdict.", 0) == 0 ||
                        k.rfind("ex.better.", 0) == 0 || k.rfind("ex.retake.", 0) == 0 || k.rfind("ex.result", 0) == 0)
                        continue;
                    for (const std::string& vt : variantsOf(k)) saysPts = saysPts || usesPlaceholder(vt, "pts");
                    // "It cannot protect f3": the square a pinned defender no longer guards.
                    if (const Arg* a = b.line.arg("sq"); a && k.rfind("ex.pin_defender.", 0) == 0 && a->square == took)
                        named = true;
                    for (const char* n : {"t1", "t2", "your"})
                        if (const Arg* a = b.line.arg(n); a && a->kind == Arg::Kind::Piece && a->color == c.human) {
                            if (a->piece == King) {
                                kingNamed = true;
                                continue;
                            }
                            target = std::max(target, int(kPiecePoints[a->piece]));
                            named = named || a->square == took;
                        }
                }
                // What the human is down then (from p0: the move's own capture counts), after the
                // human's capture right after it (anywhere: "gxf4 cxb4" trades the queens).
                Position q = c.p1;
                q.makeMove(c.refut[0].move);
                if (c.refut.size() >= 2 && c.refut[1].mover == c.human && c.refut[1].captured != NoPiece)
                    q.makeMove(c.refut[1].move);
                const int lostAtOnce = c.base - materialBalance(q, c.human);
                // A check the motif names (a discovered check, a fork with check, a back-rank check): its prize is what
                // the line wins after the reply's own capture.
                if (kingNamed && (t == ExType::Discovered || t == ExType::Fork || t == ExType::Skewer || t == ExType::BackRank))
                    target = std::max(target, lastingLoss(c.playedLine, c.p0, c.human, c.base, 9) - lostAtOnce);
                if (lostAtOnce >= 1 && lostAtOnce >= target && !named && !(t == ExType::Material && saysPts))
                    f.push_back({"MOTIF_BEHIND_CAPTURE", std::string(exTypeName(t)) + " names " + std::to_string(target) +
                                                              " points, " + c.refut[0].san + " takes " +
                                                              std::to_string(lostAtOnce) + " at once"});
            }

            // A material reason that names much less than the engine's drop understates the problem.
            if (explained && fault && isMaterialType(t) && c.lp) {
                const int sign = c.human == White ? 1 : -1;
                const int drop = sign * (v.cpWhiteBefore - v.cpWhiteAfter);
                int named = 0;
                if (demo) named = shownLoss(c, demoPlies);
                // The points a reason says (where its phrasings say {pts}), else the pieces it names
                // as lost or attacked.
                for (const Beat& b : rev.script) {
                    const std::string& k = b.line.key;
                    if (b.kind != BeatKind::Say || k.rfind("ex.", 0) != 0 || k.rfind("ex.better.", 0) == 0 ||
                        k.rfind("ex.hint.", 0) == 0 || k.rfind("ex.reveal.", 0) == 0 || k.rfind("ex.retake.", 0) == 0)
                        continue;
                    bool saysPts = false;
                    for (const std::string& v : variantsOf(k)) saysPts = saysPts || usesPlaceholder(v, "pts");
                    if (const Arg* p = b.line.arg("pts"); p && saysPts) named = std::max(named, p->number);
                    // The piece a pinned defender no longer guards ("so f3 is not really defended").
                    if (const Arg* a = b.line.arg("sq"); a && k.rfind("ex.pin_defender.", 0) == 0 && a->square != NoSquare &&
                                                         c.p1.at(a->square).color == c.human)
                        named = std::max(named, kPiecePoints[c.p1.at(a->square).type]);
                    // A line said from the table: what it leaves the human down where it stops (the
                    // human, to move then, taking back what it can at once).
                    Position start, end;
                    std::vector<Move> moves;
                    bool fromBest = false;
                    if (b.line.key.rfind("ex.better.", 0) != 0 && saidLine(c, b, start, moves, fromBest) && !fromBest &&
                        start.fen() == c.p1.fen()) {
                        end = start;
                        for (const Move& m : moves) end.makeMove(m);
                        named = std::max(named, heldLossAt(end, c.human, c.base));
                    }
                    for (const char* t : {"t1", "t2", "your"})
                        if (const Arg* a = b.line.arg(t); a && a->kind == Arg::Kind::Piece && a->color == c.human &&
                                                          a->piece != King)
                            named = std::max(named, kPiecePoints[a->piece]);
                }
                // The same measure as the coach's (Ctx::lossExplains): a pawn for 2.5 pawns of the
                // drop, a piece for a pawn (or the exchange) for 8.
                if (named < 3 && named * (named >= 2 ? 400 : 250) < drop)
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
            if (rev.script.empty() && pre.empty() && declined.empty() && !o.all && f.empty()) continue;

            // The transcript.
            std::fprintf(out, "=== #%zu L%d %s %s  %s\n", ri, level, c.human == White ? "W" : "B",
                         c.p0.fen().c_str(), anyError ? "ERROR" : (f.empty() ? "" : "warn"));
            if (!c.rec->tag.empty()) std::fprintf(out, "  tag %s\n", c.rec->tag.c_str());
            std::fprintf(out, "  move %s  class %s  W%% %.1f -> %.1f (d %.1f)  best %s  ex %s%s%s\n", v.san.c_str(),
                         className(v.cls), v.wBest, v.wPlayed, v.delta, v.bestSan.c_str(), exTypeName(t),
                         v.voiced ? " voiced" : "", v.offered ? " offer" : "");
            if (rev.unsettled) std::fprintf(out, "  unsettled (the search's last iterations disagree)\n");
            if (rev.costBefore[0] >= 0.0 || rev.costBefore[1] >= 0.0)
                std::fprintf(out, "  settle d-1 %.1f d-2 %.1f\n", rev.costBefore[0], rev.costBefore[1]);
            if (rev.assessment.wdl) {
                const Assessment& A = rev.assessment;
                std::fprintf(out, "  wdl best %d/%d/%d played %d/%d/%d  E %.2f -> %.2f  lead %.2f%s\n", A.best.win, A.best.draw,
                             A.best.loss, A.played.win, A.played.draw, A.played.loss, A.best.e, A.played.e, A.eLead(),
                             A.changesResult() ? "  result" : "");
            }
            if (rev.claim.type != ExType::None) {
                std::string ln;
                for (const std::string& m : rev.claim.line) ln += " " + m;
                const Claim& cl = rev.claim;
                std::fprintf(out, "  claim %s lost %d plies %d%s%s line%s\n", exTypeName(cl.type), cl.lost, cl.plies,
                             cl.mate ? " mate" : "", cl.guessed ? " guessed" : "", ln.c_str());
                std::fprintf(out, "  checks quiet %d comp %d own %d bestLoss %d playedLoss %d drop %d held %d\n", cl.quiet,
                             cl.compensation, int(cl.ownLine), cl.bestLoss, cl.playedLoss, cl.dropCp, cl.held);
            }
            if (ri2.verified) {
                const Claim& cl = rev.claim.type != ExType::None ? rev.claim : rev.dropped;
                std::fprintf(out, "  vcheck plies %d changed %d reduced %d searches %d%s\n", cl.verifiedPlies,
                             int(cl.lineChanged), rev.dropped.type == ExType::None ? 0 : rev.dropped.reduced, asked,
                             budgetOut ? " budget" : "");
            }
            for (const std::string& l : rev.verifyLog) std::fprintf(out, "  vlog %s\n", l.c_str());
            if (rev.dropped.type != ExType::None) {
                std::string ln;
                for (const std::string& m : rev.dropped.line) ln += " " + m;
                std::fprintf(out, "  dropped %s lost %d plies %d line%s\n", exTypeName(rev.dropped.type), rev.dropped.lost,
                             rev.dropped.plies, ln.c_str());
            }
            std::fprintf(out, "  best line: %s\n", sanLine(c.bestLine, 0, std::min<size_t>(10, c.bestLine.size())).c_str());
            std::fprintf(out, "  played line: %s\n",
                         sanLine(c.playedLine, 0, std::min<size_t>(10, c.playedLine.size())).c_str());
            printScript("PRE ", pre);
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
            printScript("DECL", declined);
            for (const Finding& x : f)
                std::fprintf(out, "  %s %s: %s\n", x.error ? "!!" : "..", x.code.c_str(), x.detail.c_str());
        }
    }
    std::fprintf(out, "\n==== %d reviews, %d with a script\n", st.reviews, st.scripts);
    if (searches)
        std::fprintf(out, "  sequences checked %d: kept %d, smaller %d, dropped %d (budget spent %d); %d searches asked, %d run\n",
                     st.checked, st.kept, st.reduced, st.dropped, st.budgetOut, st.searches, searches->searched());
    for (const auto& k : st.codes) std::fprintf(out, "  %-24s %d\n", k.first.c_str(), k.second);
    std::fprintf(out, "  voiced explanations:\n");
    for (const auto& k : st.types) std::fprintf(out, "    %-16s %d\n", k.first.c_str(), k.second);
    if (out != stdout) std::fclose(out);
    return 0;
}

}  // namespace audit
