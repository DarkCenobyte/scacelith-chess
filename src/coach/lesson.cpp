// The rules lesson: chapters, exercises and the reactions to the player's moves.
//
// Integrator notes (the director in game_scene, W9), research-pedagogy §3.0:
// - The player is White, the coach Black. Legal-move hints are forced on, so illegal placements
//   are refused before the arbiter: report each refused (from, to) to explainIllegal().
// - Touch-move is relaxed: a piece put back on its square is released (Arbiter::reset(game)).
// - Nothing is recorded: Scorekeeper::setWriteLimit(0), no clock, no rating, and positions that
//   are over on load (8.1 stalemate, 8.3 two kings) or after a move (mates, the stalemate
//   exercise) never trigger the end-of-game flow; Game::undo works on a finished game.
// - SetPosition: Game::resetFromFEN + Arbiter::reset + PhysicalBoard::syncTo behind a fade.
//   PlayMove: the coach's hand plays the move and the game records it (no scoresheet).
// - WaitMove: after the player's move, judge(); perform 'before', take back 'undo' plies by
//   hand (Game::undo + the physical move back), perform 'after', and wait again unless accepted.
#include "lesson.h"
#include <algorithm>
#include <cstdlib>

namespace coach {
using namespace chess;

// ==== Why a move is illegal =======================================================================
namespace {

struct Board {
    Piece sq[64];
};

Board boardOf(const Position& p) {
    Board b;
    for (int s = 0; s < 64; ++s) b.sq[s] = p.at(Square(s));
    return b;
}

int sign(int v) { return (v > 0) - (v < 0); }

// Squares strictly between two aligned squares, from 'from' outwards.
std::vector<Square> between(Square from, Square to) {
    std::vector<Square> out;
    int df = fileOf(to) - fileOf(from), dr = rankOf(to) - rankOf(from);
    if (!(df == 0 || dr == 0 || std::abs(df) == std::abs(dr))) return out;
    int sf = sign(df), sr = sign(dr);
    int f = fileOf(from) + sf, r = rankOf(from) + sr;
    while (f != fileOf(to) || r != rankOf(to)) {
        out.push_back(makeSquare(f, r));
        f += sf;
        r += sr;
    }
    return out;
}

// Does the piece type move (or attack) like that on an empty board? Pawns: their capture.
bool geometry(PieceType t, Color c, Square from, Square to) {
    int df = fileOf(to) - fileOf(from), dr = rankOf(to) - rankOf(from);
    int adf = std::abs(df), adr = std::abs(dr);
    if (from == to) return false;
    switch (t) {
    case Pawn: return adf == 1 && dr == (c == White ? 1 : -1);
    case Knight: return (adf == 1 && adr == 2) || (adf == 2 && adr == 1);
    case Bishop: return adf == adr;
    case Rook: return df == 0 || dr == 0;
    case Queen: return adf == adr || df == 0 || dr == 0;
    case King: return adf <= 1 && adr <= 1;
    default: return false;
    }
}

bool attacks(const Board& b, Square from, Square to) {
    Piece p = b.sq[from];
    if (p.empty() || !geometry(p.type, p.color, from, to)) return false;
    if (p.type == Bishop || p.type == Rook || p.type == Queen)
        for (Square s : between(from, to))
            if (!b.sq[s].empty()) return false;
    return true;
}

std::vector<Square> attackersOf(const Board& b, Square target, Color by) {
    std::vector<Square> out;
    for (int s = 0; s < 64; ++s)
        if (!b.sq[s].empty() && b.sq[s].color == by && attacks(b, Square(s), target)) out.push_back(Square(s));
    return out;
}

Square kingOn(const Board& b, Color c) {
    for (int s = 0; s < 64; ++s)
        if (b.sq[s].type == King && b.sq[s].color == c) return Square(s);
    return NoSquare;
}

}  // namespace

const char* illegalReasonName(IllegalReason r) {
    switch (r) {
    case IllegalReason::None: return "None";
    case IllegalReason::NoPiece: return "NoPiece";
    case IllegalReason::NotYourTurn: return "NotYourTurn";
    case IllegalReason::OwnPieceOnTarget: return "OwnPieceOnTarget";
    case IllegalReason::WrongGeometry: return "WrongGeometry";
    case IllegalReason::PathBlocked: return "PathBlocked";
    case IllegalReason::PawnForwardBlocked: return "PawnForwardBlocked";
    case IllegalReason::PawnCaptureNeedsVictim: return "PawnCaptureNeedsVictim";
    case IllegalReason::EnPassantExpired: return "EnPassantExpired";
    case IllegalReason::LeavesKingInCheck: return "LeavesKingInCheck";
    case IllegalReason::MustAnswerCheck: return "MustAnswerCheck";
    case IllegalReason::KingIntoCheck: return "KingIntoCheck";
    case IllegalReason::CastlingNoRights: return "CastlingNoRights";
    case IllegalReason::CastlingBlocked: return "CastlingBlocked";
    case IllegalReason::CastlingOutOfCheck: return "CastlingOutOfCheck";
    case IllegalReason::CastlingThroughCheck: return "CastlingThroughCheck";
    case IllegalReason::NeedsPromotionPiece: return "NeedsPromotionPiece";
    }
    return "None";
}

IllegalInfo whyIllegal(const Position& p, Square from, Square to, PieceType promo) {
    IllegalInfo info;
    if (from < 0 || from > 63 || to < 0 || to > 63 || p.at(from).empty()) {
        info.reason = IllegalReason::NoPiece;
        return info;
    }
    Piece pc = p.at(from);
    Color us = pc.color, them = opposite(us);
    if (us != p.sideToMove()) {
        info.reason = IllegalReason::NotYourTurn;
        return info;
    }
    if (p.findLegal(from, to, promo).valid()) return info;   // legal
    Piece target = p.at(to);
    if (!target.empty() && target.color == us) {
        info.reason = IllegalReason::OwnPieceOnTarget;
        info.culprit = to;
        return info;
    }
    int lastRank = us == White ? 7 : 0;
    if (pc.type == Pawn && rankOf(to) == lastRank && promo == NoPiece && p.findLegal(from, to, Queen).valid()) {
        info.reason = IllegalReason::NeedsPromotionPiece;
        return info;
    }
    Board b = boardOf(p);
    Square ksq = p.kingSquare(us);
    std::vector<Square> checkers = attackersOf(b, ksq, them);

    // Castling: the king two files along its first rank from its initial square.
    Square home = makeSquare(4, us == White ? 0 : 7);
    if (pc.type == King && from == home && rankOf(to) == rankOf(from) && std::abs(fileOf(to) - fileOf(from)) == 2) {
        bool kingSide = fileOf(to) > fileOf(from);
        uint8_t right = us == White ? (kingSide ? WhiteKingSide : WhiteQueenSide) : (kingSide ? BlackKingSide : BlackQueenSide);
        Square rookSq = makeSquare(kingSide ? 7 : 0, rankOf(from));
        Piece rook = p.at(rookSq);
        if (!(p.castling() & right) || rook.type != Rook || rook.color != us) {
            info.reason = IllegalReason::CastlingNoRights;
            info.culprit = rook.type == Rook && rook.color == us ? rookSq : from;
            return info;
        }
        for (Square s : between(from, rookSq)) {
            if (!b.sq[s].empty()) {
                info.reason = IllegalReason::CastlingBlocked;
                info.culprit = s;
                return info;
            }
        }
        if (!checkers.empty()) {
            info.reason = IllegalReason::CastlingOutOfCheck;
            info.culprit = checkers.front();
            return info;
        }
        Square crossed = makeSquare(kingSide ? 5 : 3, rankOf(from));
        Board moved = b;   // the king alone on the crossed square (the rook has not moved yet)
        moved.sq[from] = Piece();
        moved.sq[crossed] = pc;
        std::vector<Square> a = attackersOf(moved, crossed, them);
        if (!a.empty()) {
            info.reason = IllegalReason::CastlingThroughCheck;
            info.square = crossed;
            info.culprit = a.front();
            return info;
        }
        moved.sq[crossed] = Piece();
        moved.sq[to] = pc;
        a = attackersOf(moved, to, them);
        info.reason = a.empty() ? IllegalReason::WrongGeometry : IllegalReason::KingIntoCheck;
        info.square = to;
        info.culprit = a.empty() ? NoSquare : a.front();
        return info;
    }

    // Pawns: pushes, captures, en passant.
    int dir = us == White ? 1 : -1;
    int df = fileOf(to) - fileOf(from), dr = rankOf(to) - rankOf(from);
    bool epCapture = false;
    if (pc.type == Pawn) {
        int startRank = us == White ? 1 : 6;
        if (df == 0 && (dr == dir || (dr == 2 * dir && rankOf(from) == startRank))) {
            Square blocker = NoSquare;
            if (dr == 2 * dir && !b.sq[makeSquare(fileOf(from), rankOf(from) + dir)].empty())
                blocker = makeSquare(fileOf(from), rankOf(from) + dir);
            else if (!target.empty())
                blocker = to;
            if (blocker != NoSquare) {
                info.reason = IllegalReason::PawnForwardBlocked;
                info.culprit = blocker;
                return info;
            }
        } else if (std::abs(df) == 1 && dr == dir) {
            if (target.empty()) {
                if (to == p.epSquare()) {
                    epCapture = true;
                } else {
                    Square beside = makeSquare(fileOf(to), rankOf(from));
                    Piece pb = b.sq[beside];
                    bool epRank = rankOf(from) == (us == White ? 4 : 3);
                    info.reason = pb.type == Pawn && pb.color == them && epRank ? IllegalReason::EnPassantExpired
                                                                                  : IllegalReason::PawnCaptureNeedsVictim;
                    info.culprit = info.reason == IllegalReason::EnPassantExpired ? beside : NoSquare;
                    info.square = to;
                    return info;
                }
            }
        } else {
            info.reason = IllegalReason::WrongGeometry;
            return info;
        }
    } else {
        if (!geometry(pc.type, us, from, to)) {
            info.reason = IllegalReason::WrongGeometry;
            return info;
        }
        if (pc.type == Bishop || pc.type == Rook || pc.type == Queen) {
            for (Square s : between(from, to)) {
                if (!b.sq[s].empty()) {
                    info.reason = IllegalReason::PathBlocked;
                    info.culprit = s;
                    return info;
                }
            }
        }
    }

    // A move the piece can make that leaves (or puts) its own king in check.
    Board after = b;
    after.sq[from] = Piece();
    after.sq[to] = pc;
    if (epCapture) after.sq[makeSquare(fileOf(to), rankOf(from))] = Piece();
    Square king = pc.type == King ? to : kingOn(after, us);
    std::vector<Square> a = king == NoSquare ? std::vector<Square>() : attackersOf(after, king, them);
    if (pc.type == King) {
        info.reason = IllegalReason::KingIntoCheck;
        info.square = to;
        info.culprit = a.empty() ? NoSquare : a.front();
        return info;
    }
    if (!checkers.empty()) {
        // A checker still attacks: the check is not answered. Only new attackers: the piece was
        // pinned (it answered the check but opened another line).
        for (Square s : a) {
            if (std::find(checkers.begin(), checkers.end(), s) != checkers.end()) {
                info.reason = IllegalReason::MustAnswerCheck;
                info.culprit = s;
                info.square = ksq;
                return info;
            }
        }
        info.reason = IllegalReason::LeavesKingInCheck;
        info.culprit = a.empty() ? NoSquare : a.front();
        info.square = king;
        return info;
    }
    info.reason = IllegalReason::LeavesKingInCheck;
    info.culprit = a.empty() ? NoSquare : a.front();
    info.square = king;
    return info;
}

// ==== Building the chapters ========================================================================
namespace {

Square sq(const char* s) { return parseSquare(s); }
Arg yours(PieceType t, const char* s) { return Arg::ofPiece(t, White, true, sq(s)); }
Arg mine(PieceType t, const char* s) { return Arg::ofPiece(t, Black, false, sq(s)); }
Arg square(const char* s) { return Arg::ofSquare(sq(s)); }

Line line(const char* key) {
    Line l;
    l.key = key;
    return l;
}
Line line(const char* key, const char* n1, const Arg& a1) { Line l = line(key); l.with(n1, a1); return l; }
Line line(const char* key, const char* n1, const Arg& a1, const char* n2, const Arg& a2) {
    Line l = line(key, n1, a1);
    l.with(n2, a2);
    return l;
}

Gesture gesture(GestureKind k, const char* s, const char* anchor = "", float at = 0.0f) {
    Gesture g;
    g.kind = k;
    g.square = s ? sq(s) : NoSquare;
    g.anchor = anchor;
    g.at = at;
    return g;
}
Gesture pointPiece(const char* s, const char* anchor = "", float at = 0.0f) { return gesture(GestureKind::PointPiece, s, anchor, at); }
Gesture pointSquare(const char* s, const char* anchor = "", float at = 0.0f) { return gesture(GestureKind::PointSquare, s, anchor, at); }
Gesture present(const char* s, const char* anchor = "", float at = 0.0f) { return gesture(GestureKind::Present, s, anchor, at); }
Gesture beatG(const char* anchor = "", float at = 0.0f) { return gesture(GestureKind::Beat, nullptr, anchor, at); }
Gesture nod() { return gesture(GestureKind::Nod, nullptr); }
Gesture openPalm() { return gesture(GestureKind::Open, nullptr); }
Gesture trace(std::initializer_list<const char*> path, const char* anchor = "", float at = 0.0f) {
    Gesture g = gesture(GestureKind::Trace, nullptr, anchor, at);
    for (const char* s : path) g.path.push_back(sq(s));
    g.square = g.path.empty() ? NoSquare : g.path.back();
    return g;
}
Gesture pointObject(TableObject o, const char* anchor = "", float at = 0.0f) {
    Gesture g = gesture(GestureKind::PointObject, nullptr, anchor, at);
    g.object = o;
    return g;
}

Mark markSquare(const char* s, const char* anchor = "") {
    Mark m;
    m.kind = Mark::Kind::Square;
    m.square = sq(s);
    m.anchor = anchor;
    return m;
}
Mark markPiece(const char* s, const char* anchor = "") {
    Mark m = markSquare(s, anchor);
    m.kind = Mark::Kind::Piece;
    return m;
}
Mark markArrow(const char* from, const char* to, const char* via = nullptr, const char* anchor = "") {
    Mark m;
    m.kind = Mark::Kind::Arrow;
    m.from = sq(from);
    m.to = sq(to);
    m.via = via ? sq(via) : NoSquare;
    m.anchor = anchor;
    return m;
}

bool pointsAtBoard(const std::vector<Gesture>& g, const std::vector<Mark>& m) {
    for (auto& x : g)
        if (x.square != NoSquare || !x.path.empty() || x.kind == GestureKind::PointObject) return true;
    return !m.empty();
}

Beat sayBeat(const Line& l, std::vector<Gesture> g = {}, std::vector<Mark> m = {}) {
    Beat b;
    b.kind = BeatKind::Say;
    b.line = l;
    // The gaze rule: at what the line is about, else at the player.
    b.look = pointsAtBoard(g, m) ? Look::Target : Look::Player;
    b.gestures = std::move(g);
    b.marks = std::move(m);
    return b;
}

LessonReply reply(LessonReply::When w, std::vector<std::string> moves, const Line& l) {
    LessonReply r;
    r.when = w;
    r.moves = std::move(moves);
    r.line = l;
    return r;
}
LessonReply replyMoves(std::vector<std::string> moves, const Line& l) { return reply(LessonReply::When::Moves, std::move(moves), l); }
LessonReply replyPiece(PieceType t, const Line& l) {
    LessonReply r = reply(LessonReply::When::Piece, {}, l);
    r.piece = t;
    return r;
}
LessonReply replyAny(const Line& l) { return reply(LessonReply::When::Any, {}, l); }
LessonReply replyCheckEscape() {
    LessonReply r = reply(LessonReply::When::Check, {}, Line());
    r.demo = "escape";
    return r;
}
LessonReply replyStalemate() { return reply(LessonReply::When::Stalemate, {}, line("lesson.stalemate")); }

}  // namespace

// Builds the chapters while following the position the player will see, so that every exercise
// knows its FEN (the tests replay them).
struct LessonBuilder {
    Lesson& lesson;
    LessonChapter* chapter = nullptr;
    Position pos;

    explicit LessonBuilder(Lesson& l) : lesson(l) {}

    void begin(const char* id) {
        lesson.chapters_.push_back(LessonChapter());
        chapter = &lesson.chapters_.back();
        chapter->id = id;
        chapter->title = line(("lesson.title." + std::string(id)).c_str());
    }
    void set(const char* fen) {
        Beat b;
        b.kind = BeatKind::SetPosition;
        b.fen = fen;
        b.skippable = false;
        chapter->beats.push_back(b);
        pos.setFEN(fen);
    }
    Beat& say(const Line& l, std::vector<Gesture> g = {}, std::vector<Mark> m = {}) {
        chapter->beats.push_back(sayBeat(l, std::move(g), std::move(m)));
        return chapter->beats.back();
    }
    Beat& sayBoard(const Line& l, std::vector<Gesture> g = {}, std::vector<Mark> m = {}) {
        Beat& b = say(l, std::move(g), std::move(m));
        b.look = Look::Board;
        return b;
    }
    void play(const char* uci) {
        Beat b;
        b.kind = BeatKind::PlayMove;
        b.uci = uci;
        b.look = Look::Board;
        b.skippable = false;
        chapter->beats.push_back(b);
        pos.makeMove(pos.parseUCI(uci));
    }
    void demo(const char* uci, const Line& narration = Line()) {
        Beat b;
        b.kind = BeatKind::DemoMove;
        b.uci = uci;
        b.line = narration;
        b.look = Look::Board;
        chapter->beats.push_back(b);
    }
    void rewind(int n) {
        Beat b;
        b.kind = BeatKind::Rewind;
        b.count = n;
        b.look = Look::Board;
        b.skippable = false;
        chapter->beats.push_back(b);
    }
    void pause(float s) {
        Beat b;
        b.kind = BeatKind::Pause;
        b.seconds = s;
        chapter->beats.push_back(b);
    }
    // Asks (the 'ask' line with its gestures and marks), then waits; the player's model move
    // (the first accepted one, or the solution) continues the followed position.
    Expectation& wait(Expectation e, const std::vector<Gesture>& askGestures = {}, const std::vector<Mark>& askMarks = {}) {
        if (!e.ask.empty()) say(e.ask, askGestures, askMarks);
        e.fen = pos.fen();
        int index = int(lesson.expectations_.size());
        lesson.expectations_.push_back(e);
        lesson.chapterOfExpectation_.push_back(int(lesson.chapters_.size()) - 1);
        Beat b;
        b.kind = BeatKind::WaitMove;
        b.expect = index;
        b.look = Look::Player;
        b.skippable = false;
        chapter->beats.push_back(b);
        std::string model = !e.solution.empty() ? e.solution : e.accept.empty() ? std::string() : e.accept.front();
        Move m = pos.parseUCI(model);
        if (m.valid()) pos.makeMove(m);
        return lesson.expectations_.back();
    }
};

namespace {

Expectation anyOf(std::vector<std::string> accept, const Line& ask, const Line& success) {
    Expectation e;
    e.kind = Expectation::Kind::AnyOf;
    e.accept = std::move(accept);
    e.solution = e.accept.front();
    e.ask = ask;
    e.success = success;
    return e;
}

}  // namespace

Lesson::Lesson() {
    LessonBuilder b(*this);
    const char* kStart = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

    // ---- Chapter 0: welcome and the board (§3.1) ----
    b.begin("welcome");
    b.set("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    b.say(line("lesson.welcome.hello"), {nod()});
    b.sayBoard(line("lesson.welcome.board"), {present(nullptr)});
    b.say(line("lesson.welcome.light"), {pointSquare("h1", "@")}, {markSquare("h1", "@")});
    b.say(line("lesson.welcome.files"), {trace({"a1", "h1"}, "@")}, {markArrow("a1", "h1")});
    b.say(line("lesson.welcome.ranks"), {trace({"a1", "a8"}, "@")}, {markArrow("a1", "a8")});
    b.say(line("lesson.welcome.square", "sq", square("e4")), {pointSquare("e4", "sq")}, {markSquare("e4", "sq")});
    b.say(line("lesson.welcome.square2", "sq", square("d5")), {pointSquare("d5", "sq")}, {markSquare("d5", "sq")});
    b.say(line("lesson.welcome.kings", "sq", square("e1"), "sq2", square("e8")),
          {pointPiece("e1", "sq"), pointPiece("e8", "sq2")});
    {
        Expectation e = anyOf({"e1e2"}, line("lesson.welcome.ask", "sq", square("e2")),
                              line("lesson.welcome.ok", "sq", square("e2")));
        e.replies.push_back(replyAny(line("lesson.welcome.wrong", "sq", square("e2"))));
        e.hint2 = e.ask;
        e.hint2Gestures = {pointSquare("e2", "sq")};
        e.hint2Marks = {markSquare("e2")};
        Beat& ask = b.say(e.ask, {}, {markSquare("e2")});
        ask.look = Look::Player;
        Line askLine = e.ask;
        e.ask = Line();   // said above with the player in view
        Expectation& w = b.wait(e);
        w.ask = askLine;
    }

    // ---- Chapter 1: how the pieces move and capture (§3.2) ----
    b.begin("pieces");
    // 1.1 Rook
    b.set("7k/8/n7/8/8/8/8/R3K3 w - - 0 1");
    b.say(line("lesson.rook.intro", "your", yours(Rook, "a1")), {pointPiece("a1", "your")});
    b.say(line("lesson.rook.lines"), {trace({"a1", "a5"}), trace({"a1", "d1"}, "@")});
    b.say(line("lesson.rook.capture"), {pointPiece("a6", "@")});
    {
        Arg my = mine(Knight, "a6"), your = yours(Rook, "a1");
        Expectation e = anyOf({"a1a6"}, line("lesson.rook.ask", "my", my), line("lesson.rook.ok", "my", my));
        e.replies.push_back(replyMoves({"a1a2", "a1a3", "a1a4", "a1a5"}, line("lesson.rook.short", "my", my)));
        e.replies.push_back(replyMoves({"a1b1", "a1c1", "a1d1"}, line("lesson.rook.rank", "my", my)));
        e.replies.push_back(replyPiece(King, line("lesson.rook.king", "your", your)));
        e.hint2Gestures = {pointPiece("a1"), pointPiece("a6")};
        e.hint2Marks = {markArrow("a1", "a6")};
        b.wait(e, {}, {markPiece("a6")});
    }
    // 1.2 Pieces don't jump (demonstration only)
    b.set("7k/8/n7/8/P7/8/8/R3K3 w - - 0 1");
    b.say(line("lesson.jump.blocked", "sq", square("a4")), {trace({"a1", "a3"}), pointPiece("a4", "sq")},
          {markSquare("a4", "sq")});
    b.say(line("lesson.jump.knight"));
    // 1.3 Bishop
    b.set("7k/8/8/1r6/8/8/8/4KB2 w - - 0 1");
    b.say(line("lesson.bishop.intro", "your", yours(Bishop, "f1")), {pointPiece("f1", "your"), trace({"f1", "c4"}, "", 0.6f)});
    b.say(line("lesson.bishop.colour"), {present("d3")});
    {
        Arg my = mine(Rook, "b5"), your = yours(Bishop, "f1");
        Expectation e = anyOf({"f1b5"}, line("lesson.bishop.ask", "my", my), line("lesson.bishop.ok"));
        e.replies.push_back(replyMoves({"f1e2", "f1d3", "f1c4"}, line("lesson.bishop.short", "my", my)));
        e.replies.push_back(replyMoves({"f1g2", "f1h3"}, line("lesson.bishop.other", "my", my)));
        e.replies.push_back(replyPiece(King, line("lesson.use_your", "your", your)));
        e.hint2Gestures = {pointPiece("f1"), pointPiece("b5")};
        e.hint2Marks = {markArrow("f1", "b5")};
        b.wait(e, {pointPiece("b5", "my")}, {markPiece("b5", "my")});
    }
    // 1.4 Queen (the black king on a8, so that Qxh5 gives no check)
    b.set("k7/8/8/7n/8/8/8/3QK3 w - - 0 1");
    b.say(line("lesson.queen.intro", "your", yours(Queen, "d1")),
          {pointPiece("d1", "your"), trace({"d1", "d6"}, "", 0.45f), trace({"d1", "g4"}, "", 0.75f)});
    {
        Arg my = mine(Knight, "h5"), your = yours(Queen, "d1");
        Expectation e = anyOf({"d1h5"}, line("lesson.queen.ask", "my", my), line("lesson.queen.ok"));
        e.replies.push_back(replyPiece(Queen, line("lesson.queen.wrong", "my", my)));
        e.replies.push_back(replyPiece(King, line("lesson.use_your", "your", your)));
        e.hint2 = line("lesson.queen.wrong", "my", my);
        e.hint2Gestures = {pointSquare("g4")};
        e.hint2Marks = {markArrow("d1", "h5")};
        b.wait(e, {}, {markPiece("h5")});
    }
    // 1.5 King
    b.set("k7/8/8/8/8/8/4p3/4K3 w - - 0 1");
    b.say(line("lesson.king.intro"), {pointPiece("e1")},
          {markSquare("d1"), markSquare("d2"), markSquare("e2"), markSquare("f2"), markSquare("f1")});
    b.say(line("lesson.king.important"));
    {
        Arg my = mine(Pawn, "e2");
        Expectation e = anyOf({"e1e2"}, line("lesson.king.ask", "my", my), line("lesson.king.ok"));
        e.replies.push_back(replyAny(line("lesson.king.wrong", "my", my)));
        e.hint2Gestures = {pointPiece("e2")};
        e.hint2Marks = {markPiece("e2")};
        b.wait(e, {pointPiece("e2", "my")});
    }
    // 1.6 Knight
    b.set("7k/8/8/8/8/5r2/8/4K1N1 w - - 0 1");
    b.say(line("lesson.knight.intro", "your", yours(Knight, "g1")), {pointPiece("g1", "your")});
    b.say(line("lesson.knight.l"), {trace({"g1", "g3", "f3"}, "@")}, {markArrow("g1", "f3", "g3", "@")});
    b.say(line("lesson.knight.jump"), {beatG()});
    {
        Arg my = mine(Rook, "f3"), your = yours(Knight, "g1");
        Expectation e = anyOf({"g1f3"}, line("lesson.knight.ask", "my", my), line("lesson.knight.ok"));
        e.replies.push_back(replyMoves({"g1e2", "g1h3"}, line("lesson.knight.wrong", "my", my)));
        e.replies.push_back(replyPiece(King, line("lesson.use_your", "your", your)));
        e.hint2Gestures = {trace({"g1", "g3", "f3"})};
        e.hint2Marks = {markArrow("g1", "f3", "g3")};
        b.wait(e, {}, {markPiece("f3")});
    }
    // 1.7 Pawn: moving
    b.set("4k3/8/8/8/8/8/4P3/4K3 w - - 0 1");
    b.say(line("lesson.pawn.intro", "your", yours(Pawn, "e2")), {pointPiece("e2", "your")});
    b.say(line("lesson.pawn.first"), {}, {markSquare("e3"), markSquare("e4")});
    {
        Expectation e = anyOf({"e2e4"}, line("lesson.pawn.ask"), line("lesson.pawn.ok"));
        e.replies.push_back(replyMoves({"e2e3"}, line("lesson.pawn.one")));
        e.replies.push_back(replyPiece(King, line("lesson.pawn.use")));
        e.hint2Gestures = {pointPiece("e2"), pointSquare("e4")};
        e.hint2Marks = {markSquare("e4")};
        b.wait(e);
    }
    // 1.8 Pawn: capturing
    b.set("7k/8/8/3np3/4P3/8/8/4K3 w - - 0 1");
    b.say(line("lesson.pawncap.blocked"), {pointPiece("e5")});
    b.say(line("lesson.pawncap.diagonal"), {trace({"e4", "d5"})}, {markArrow("e4", "d5")});
    {
        Arg my = mine(Knight, "d5"), your = yours(Pawn, "e4");
        Expectation e = anyOf({"e4d5"}, line("lesson.pawncap.ask", "my", my), line("lesson.pawncap.ok"));
        e.replies.push_back(replyAny(line("lesson.pawncap.use", "your", your)));
        e.hint2Gestures = {pointPiece("e4"), pointPiece("d5")};
        e.hint2Marks = {markArrow("e4", "d5")};
        b.wait(e, {}, {markPiece("d5")});
    }
    // 1.9 Values, set-up and the first moves
    b.set(kStart);
    b.say(line("lesson.values.pawn"), {pointPiece("e2")});
    b.say(line("lesson.values.pieces"),
          {pointPiece("g1"), pointPiece("f1", "@"), pointPiece("a1", "@2"), pointPiece("d1", "@3")});
    b.say(line("lesson.values.king"), {pointPiece("e1")});
    b.say(line("lesson.values.trade"));
    b.say(line("lesson.setup.corners"), {pointPiece("a1", "@"), pointPiece("b1", "@2"), pointPiece("c1", "@3")});
    b.say(line("lesson.setup.queen"), {pointPiece("d1")});
    b.say(line("lesson.setup.king"), {pointPiece("e1")});
    {
        Expectation e = anyOf({"e2e4"}, line("lesson.first.ask", "sq", square("e4")), line("lesson.first.ok"));
        e.replies.push_back(replyAny(line("lesson.first.wrong", "sq", square("e4"))));
        e.hint2Gestures = {pointPiece("e2"), pointSquare("e4")};
        e.hint2Marks = {markSquare("e4")};
        b.wait(e, {}, {markSquare("e4", "sq")});
    }
    b.play("e7e5");
    {
        Line reply = line("lesson.first.reply", "sq", square("g1"), "sq2", square("f3"));
        Expectation e = anyOf({"g1f3"}, reply, line("lesson.first.knight_ok"));
        e.replies.push_back(replyAny(line("lesson.first.knight_wrong", "sq", square("g1"), "sq2", square("f3"))));
        e.hint2Gestures = {pointPiece("g1"), pointSquare("f3")};
        e.hint2Marks = {markSquare("f3")};
        b.wait(e, {pointPiece("g1", "sq"), pointSquare("f3", "sq2")}, {markSquare("f3", "sq2")});
    }

    // ---- Chapter 2: safe and unsafe captures (§3.3) ----
    b.begin("safety");
    b.set("7k/8/4p3/3n4/b7/8/8/3QK3 w - - 0 1");
    b.say(line("lesson.safety.defended"), {beatG()});
    b.say(line("lesson.safety.pawn", "my", mine(Knight, "d5")), {pointPiece("e6", "@"), pointPiece("d5", "my")},
          {markArrow("e6", "d5", nullptr, "@")});
    b.say(line("lesson.safety.free", "my", mine(Bishop, "a4")), {pointPiece("a4", "my")}, {markPiece("a4", "my")});
    {
        Expectation e = anyOf({"d1a4"}, line("lesson.safety.ask"), line("lesson.safety.ok"));
        LessonReply gave = replyMoves({"d1d5"}, line("lesson.safety.watch"));
        gave.demo = "e6d5";
        gave.demoLine = line("lesson.safety.gave");
        gave.after = line("lesson.safety.other");
        e.replies.push_back(gave);
        e.replies.push_back(replyAny(line("lesson.safety.look")));
        e.hint2 = line("lesson.safety.look");
        e.hint2Gestures = {pointPiece("a4")};
        e.hint2Marks = {markPiece("a4")};
        b.wait(e);
    }

    // ---- Chapter 3: check and getting out of check (§3.4) ----
    b.begin("check");
    // 3.1 The king never steps into danger
    b.set("7k/8/8/8/8/8/r7/4K3 w - - 0 1");
    b.say(line("lesson.danger.rank"), {pointPiece("a2"), trace({"a2", "h2"}, "", 0.35f)}, {markArrow("a2", "h2")});
    b.say(line("lesson.danger.rule"));
    {
        Expectation e;
        e.kind = Expectation::Kind::AnyLegal;
        e.solution = "e1d1";
        e.ask = line("lesson.danger.ask");
        e.success = line("lesson.danger.ok");
        e.hint2Gestures = {pointSquare("d1"), pointSquare("f1")};
        e.hint2Marks = {markSquare("d1"), markSquare("f1")};
        b.wait(e);
    }
    // 3.2 Check: move the king
    b.set("4r2k/8/8/8/8/8/8/4K3 w - - 0 1");
    b.say(line("lesson.check.what"), {trace({"e8", "e1"})}, {markArrow("e8", "e1")});
    b.say(line("lesson.check.must"));
    b.say(line("lesson.check.ways"), {beatG("@"), beatG("@2"), beatG("@3")});
    {
        Expectation e;
        e.kind = Expectation::Kind::EscapesCheck;
        e.solution = "e1d1";
        e.ask = line("lesson.check.ask");
        e.success = line("lesson.check.ok");
        LessonReply file = replyMoves({"e1e2"}, line("lesson.check.file"));
        e.illegal.push_back(file);
        e.hint2Gestures = {pointSquare("d1"), pointSquare("f1")};
        e.hint2Marks = {markSquare("d1"), markSquare("d2"), markSquare("f1"), markSquare("f2")};
        b.wait(e);
    }
    // 3.3 Check: block
    b.set("4r2k/8/8/8/8/8/3P1P2/3QKB2 w - - 0 1");
    b.say(line("lesson.block.again"), {pointPiece("e8"), pointPiece("e1", "", 0.6f)});
    {
        Expectation e = anyOf({"f1e2"}, line("lesson.block.ask"), line("lesson.block.ok"));
        e.replies.push_back(replyMoves({"d1e2"}, line("lesson.block.queen")));
        e.acceptOnRetry.push_back(LessonRetry{"d1e2", 2, line("lesson.block.queen_ok")});
        e.hint2Gestures = {pointPiece("f1"), pointSquare("e2")};
        e.hint2Marks = {markSquare("e2")};
        b.wait(e, {}, {markSquare("e2")});
    }
    // 3.4 Check: capture the attacker
    b.set("7k/8/8/8/8/2N5/4q3/4K3 w - - 0 1");
    b.say(line("lesson.capture.check"), {pointPiece("e2")});
    {
        Expectation e = anyOf({"c3e2", "e1e2"}, line("lesson.capture.ask"), line("lesson.capture.knight_ok"));
        e.successFor.push_back({"e1e2", line("lesson.capture.king_ok")});
        e.successFor.push_back({"c3e2", line("lesson.capture.knight_ok")});
        e.hint2Gestures = {pointPiece("e2")};
        e.hint2Marks = {markPiece("e2")};
        b.wait(e);
    }

    // ---- Chapter 4: checkmate (§3.5) ----
    b.begin("mate");
    // 4.1 Back-rank mate
    b.set("6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1");
    b.say(line("lesson.mate1.what"));
    b.say(line("lesson.mate1.stuck"), {pointPiece("g8"), present("g7", "@")},
          {markSquare("f7", "@"), markSquare("g7", "@"), markSquare("h7", "@")});
    {
        Expectation e;
        e.kind = Expectation::Kind::Mates;
        e.solution = "a1a8";
        e.ask = line("lesson.mate1.ask");
        e.success = line("lesson.mate1.ok");
        e.replies.push_back(replyStalemate());
        e.replies.push_back(replyCheckEscape());
        e.replies.push_back(replyAny(line("lesson.mate1.wrong")));
        e.hint2 = line("lesson.mate1.wrong");
        e.hint2Gestures = {pointPiece("a1")};
        e.hint2Marks = {markSquare("a8")};
        b.wait(e);
    }
    // 4.2 King and queen together
    b.set("4k3/7Q/4K3/8/8/8/8/8 w - - 0 1");
    {
        Expectation e;
        e.kind = Expectation::Kind::Mates;
        e.solution = "h7e7";
        e.ask = line("lesson.mate2.ask");
        e.success = line("lesson.mate2.ok");
        e.successFor.push_back({"h7g8", line("lesson.mate2.ok_rank")});
        e.successFor.push_back({"h7h8", line("lesson.mate2.ok_rank")});
        e.replies.push_back(replyStalemate());
        e.replies.push_back(replyCheckEscape());
        e.replies.push_back(replyAny(line("lesson.mate2.wrong")));
        e.hint2 = line("lesson.mate2.wrong");
        e.hint2Gestures = {pointPiece("h7")};
        e.hint2Marks = {markSquare("e7")};
        b.wait(e, {pointPiece("e6"), pointPiece("h7", "", 0.5f)});
    }
    // 4.3 Two-rook ladder
    b.set("4k3/R7/8/8/8/8/8/1R4K1 w - - 0 1");
    b.say(line("lesson.mate3.edge"), {trace({"a7", "h7"})}, {markArrow("a7", "h7")});
    {
        Expectation e;
        e.kind = Expectation::Kind::Mates;
        e.solution = "b1b8";
        e.ask = line("lesson.mate3.ask", "your", yours(Rook, "b1"));
        e.success = line("lesson.mate3.ok");
        LessonReply free = replyMoves({"a7a8"}, line("lesson.mate3.free"));
        free.demo = "e8e7";
        e.replies.push_back(free);
        LessonReply takes = replyMoves({"a7e7"}, line("lesson.mate3.takes"));
        takes.demo = "e8e7";
        e.replies.push_back(takes);
        e.replies.push_back(replyStalemate());
        e.replies.push_back(replyCheckEscape());
        e.replies.push_back(replyAny(line("lesson.mate3.hint")));
        e.hint1 = line("lesson.mate3.hint");
        e.hint2 = line("lesson.mate3.hint");
        e.hint2Gestures = {pointPiece("b1")};
        e.hint2Marks = {markSquare("b8")};
        b.wait(e, {pointPiece("b1", "your")});
    }

    // ---- Chapter 5: castling (§3.6) ----
    b.begin("castling");
    // 5.1 King-side castling
    b.set("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");
    b.say(line("lesson.castle.what"), {pointPiece("e1"), pointPiece("h1", "", 0.7f)});
    b.say(line("lesson.castle.how"), {trace({"e1", "g1"}, "@"), trace({"h1", "f1"}, "@2")});
    b.demo("e1g1", line("lesson.like_this"));
    b.say(line("lesson.and_back"));
    b.rewind(1);
    {
        Expectation e = anyOf({"e1g1"}, line("lesson.castle.ask"), line("lesson.castle.ok"));
        e.replies.push_back(replyMoves({"h1f1", "h1g1"}, line("lesson.castle.rook")));
        e.replies.push_back(replyMoves({"e1c1"}, line("lesson.castle.long")));
        e.replies.push_back(replyMoves({"e1f1"}, line("lesson.castle.one")));
        e.replies.push_back(replyAny(line("lesson.castle.other", "sq", square("h1"))));
        e.hint2Gestures = {pointPiece("e1"), pointSquare("g1")};
        e.hint2Marks = {markSquare("g1")};
        b.wait(e);
    }
    // 5.2 When castling is not allowed
    b.set("r3k2r/8/8/8/2b5/8/8/R3K2R w KQkq - 0 1");
    b.say(line("lesson.castle2.moved"));
    b.say(line("lesson.castle2.attacked"));
    b.say(line("lesson.castle2.bishop", "sq", square("f1")), {pointPiece("c4"), trace({"c4", "f1"}, "sq")},
          {markSquare("f1", "sq")});
    {
        Expectation e = anyOf({"e1c1"}, line("lesson.castle2.ask"), line("lesson.castle2.ok", "sq", square("c1")));
        e.replies.push_back(replyAny(line("lesson.castle2.other")));
        e.hint2Gestures = {pointPiece("e1"), pointSquare("c1")};
        e.hint2Marks = {markSquare("c1")};
        b.wait(e);
    }

    // ---- Chapter 6: en passant (§3.7) ----
    b.begin("en_passant");
    b.set("4k3/3p4/8/4P3/8/8/8/4K3 b - - 0 1");
    b.say(line("lesson.ep.what"), {pointPiece("e5")});
    b.say(line("lesson.ep.watch"), {pointPiece("d7")});
    b.play("d7d5");
    b.say(line("lesson.ep.how", "sq", square("d6")), {trace({"e5", "d6"}, "sq")}, {markSquare("d6", "sq")});
    {
        Expectation e = anyOf({"e5d6"}, line("lesson.ep.now"), line("lesson.ep.ok", "sq", square("d5")));
        e.replies.push_back(replyAny(line("lesson.ep.gone")));
        e.hint2Gestures = {pointPiece("e5"), pointSquare("d6")};
        e.hint2Marks = {markSquare("d6")};
        b.wait(e);
    }

    // ---- Chapter 7: promotion (§3.8) ----
    b.begin("promotion");
    b.set("8/4P2k/8/8/8/8/8/4K3 w - - 0 1");
    b.say(line("lesson.promo.what"), {pointPiece("e7")}, {markSquare("e8")});
    {
        Expectation e = anyOf({"e7e8q"}, line("lesson.promo.ask"), line("lesson.promo.ok"));
        e.kind = Expectation::Kind::Promotes;
        e.replies.push_back(replyMoves({"e7e8r", "e7e8b", "e7e8n"}, line("lesson.promo.under")));
        e.replies.push_back(replyAny(line("lesson.promo.push", "sq", square("e8"))));
        for (const char* u : {"e7e8r", "e7e8b", "e7e8n"}) e.acceptOnRetry.push_back(LessonRetry{u, 2, line("lesson.promo.under_ok")});
        e.hint2 = line("lesson.promo.push", "sq", square("e8"));
        e.hint2Gestures = {pointPiece("e7"), pointSquare("e8", "sq")};
        e.hint2Marks = {markSquare("e8")};
        b.wait(e);
    }

    // ---- Chapter 8: stalemate and draws (§3.9) ----
    b.begin("draws");
    // 8.1 Stalemate (demonstration; the game is drawn on load)
    b.set("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    b.say(line("lesson.stale.what"), {pointPiece("h8"), present("g7", "@")},
          {markSquare("g8", "@"), markSquare("g7", "@"), markSquare("h7", "@")});
    b.say(line("lesson.stale.draw"));
    // 8.2 Avoid stalemate
    b.set("7k/8/6K1/8/8/8/5Q2/8 w - - 0 1");
    {
        Expectation e;
        e.kind = Expectation::Kind::Mates;
        e.solution = "f2f8";
        e.ask = line("lesson.stale2.ask");
        e.success = line("lesson.stale2.ok");
        e.replies.push_back(replyStalemate());
        e.replies.push_back(replyCheckEscape());
        e.replies.push_back(replyAny(line("lesson.stale2.wrong")));
        e.hint2 = line("lesson.stale2.wrong");
        e.hint2Gestures = {pointSquare("f8")};
        e.hint2Marks = {markSquare("f8")};
        b.wait(e);
    }
    // 8.3 Other draws (talk; two kings: drawn on load)
    b.set("8/8/4k3/8/8/4K3/8/8 w - - 0 1");
    b.say(line("lesson.draws.kings"), {pointPiece("e3"), pointPiece("e6", "", 0.5f)});
    b.say(line("lesson.draws.agree"));
    b.say(line("lesson.draws.repeat"));

    // ---- Chapter 9: real games (§3.10; no moves, the start position as a backdrop) ----
    b.begin("etiquette");
    b.set(kStart);
    b.say(line("lesson.talk.rules"), {nod()});
    b.sayBoard(line("lesson.talk.real"), {present(nullptr)});
    b.say(line("lesson.talk.elo"), {beatG()});
    b.say(line("lesson.talk.elo_gap"));
    b.say(line("lesson.talk.elo_start"));
    b.say(line("lesson.talk.clock"), {pointObject(TableObject::Clock, "@")});
    b.say(line("lesson.talk.press"), {pointObject(TableObject::Clock)});
    b.say(line("lesson.talk.flag"));
    b.say(line("lesson.talk.increment"));
    b.say(line("lesson.talk.speeds"));
    b.say(line("lesson.talk.no_clock"), {openPalm()});
    b.say(line("lesson.talk.touch"), {pointPiece("e2")});
    b.say(line("lesson.talk.touch_mine"), {pointPiece("e7")});
    b.say(line("lesson.talk.adjust"));
    b.say(line("lesson.talk.release"));
    b.say(line("lesson.talk.sheet"), {pointObject(TableObject::CoachSheet, "@")});
    b.say(line("lesson.talk.notation", "move", Arg::ofMove("Nf3", "g1f3")));
    b.say(line("lesson.talk.robot"), {pointObject(TableObject::PlayerSheet)});
    b.say(line("lesson.talk.handshake"));
    b.say(line("lesson.talk.polite"));
    b.say(line("lesson.talk.tips"), {beatG("@"), beatG("@2"), beatG("@3")});
    b.say(line("lesson.talk.next"), {openPalm()});
    b.say(line("lesson.talk.welcome"), {nod()});
    // The handshake follows (anim::TaskType::Handshake, as at a game's end), then [coach]
    // rules_done = true and the coach page with First steps selected.
}

// ==== Judging moves ================================================================================
namespace {

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

Line withArg(Line l, const char* name, const Arg& a) {
    if (!l.empty() && !l.arg(name)) l.with(name, a);
    return l;
}

// The coach's answer to a check that is not mate: the king takes the checker, else steps away,
// else something blocks. 'line' gets the words, 'demo' the move.
void answerCheck(const Position& after, Line& line, std::string& demo) {
    std::vector<Move> moves = after.legalMoves();
    Square ksq = after.kingSquare(after.sideToMove());
    const Move* takes = nullptr;
    const Move* step = nullptr;
    for (const Move& m : moves) {
        if (m.from != ksq) continue;
        if (!after.at(m.to).empty() && !takes) takes = &m;
        else if (after.at(m.to).empty() && !step) step = &m;
    }
    if (takes) {
        Piece victim = after.at(takes->to);
        line = Line();
        line.key = "lesson.check_takes";
        line.with("your", Arg::ofPiece(victim.type, victim.color, true, takes->to));
        demo = after.toUCI(*takes);
    } else if (step) {
        line = Line();
        line.key = "lesson.check_escape";
        line.with("esc", Arg::ofSquare(step->to));
        demo = after.toUCI(*step);
    } else if (!moves.empty()) {
        line = Line();
        line.key = "lesson.check_block";
        demo = after.toUCI(moves.front());
    }
}

Beat say(const Line& l, Look look, std::vector<Gesture> g = {}, std::vector<Mark> m = {}) {
    Beat b = sayBeat(l, std::move(g), std::move(m));
    b.look = look;
    return b;
}

Beat demoBeat(const std::string& uci) {
    Beat b;
    b.kind = BeatKind::DemoMove;
    b.uci = uci;
    b.look = Look::Board;
    return b;
}

Beat rewindBeat(int n) {
    Beat b;
    b.kind = BeatKind::Rewind;
    b.count = n;
    b.look = Look::Board;
    b.skippable = false;
    return b;
}

// The second hint: the hint line with pointing at the solution when none is given.
Beat hint2Beat(const Expectation& e) {
    Line l = !e.hint2.empty() ? e.hint2 : !e.hint1.empty() ? e.hint1 : e.ask;
    std::vector<Gesture> g = e.hint2Gestures;
    std::vector<Mark> m = e.hint2Marks;
    if (g.empty() && e.solution.size() >= 4) {
        Gesture p;
        p.kind = GestureKind::PointPiece;
        p.square = parseSquare(e.solution.substr(0, 2));
        g.push_back(p);
        Mark mk;
        mk.kind = Mark::Kind::Square;
        mk.square = parseSquare(e.solution.substr(2, 2));
        m.push_back(mk);
    }
    return say(l, Look::Target, g, m);
}

}  // namespace

int Lesson::chapterOf(int expectation) const {
    if (expectation < 0 || size_t(expectation) >= chapterOfExpectation_.size()) return -1;
    return chapterOfExpectation_[size_t(expectation)];
}

LessonReaction Lesson::judge(int expect, const Position& before, const Move& move, int failures) const {
    LessonReaction r;
    if (expect < 0 || size_t(expect) >= expectations_.size()) {
        r.accepted = true;
        return r;
    }
    const Expectation& e = expectations_[size_t(expect)];
    Move m = before.findLegal(move.from, move.to, move.promotion);
    if (!m.valid()) {
        r.before = explainIllegal(expect, before, move.from, move.to, move.promotion);
        return r;
    }
    std::string uci = before.toUCI(m);
    Position after = before;
    after.makeMove(m);
    Arg played = Arg::ofSquare(m.to);

    bool ok = false;
    switch (e.kind) {
    case Expectation::Kind::AnyOf: ok = contains(e.accept, uci); break;
    case Expectation::Kind::AnyLegal: ok = true; break;
    case Expectation::Kind::EscapesCheck: ok = before.inCheck(); break;
    case Expectation::Kind::Mates: ok = after.isCheckmate(); break;
    case Expectation::Kind::Promotes: ok = m.promotion != NoPiece && contains(e.accept, uci); break;
    }
    Line praise = e.success;
    if (!ok) {
        for (const LessonRetry& rt : e.acceptOnRetry) {
            if (rt.uci == uci && failures + 1 >= rt.fromTry) {
                ok = true;
                praise = rt.line;
            }
        }
    } else {
        for (auto& s : e.successFor)
            if (s.first == uci) praise = s.second;
    }
    if (ok) {
        r.accepted = true;
        if (!praise.empty()) r.before.push_back(say(withArg(praise, "to", played), Look::Player, {nod()}));
        return r;
    }

    // Wrong: answer it with the move on the board, take it back, then help more each time.
    r.undo = 1;
    Piece moved = before.at(m.from);
    bool check = after.inCheck() && !after.isCheckmate();
    bool stalemate = after.isStalemate();
    const LessonReply* hit = nullptr;
    for (const LessonReply& rp : e.replies) {
        bool match = false;
        switch (rp.when) {
        case LessonReply::When::Moves: match = contains(rp.moves, uci); break;
        case LessonReply::When::Piece: match = moved.type == rp.piece; break;
        case LessonReply::When::Check: match = check; break;
        case LessonReply::When::Stalemate: match = stalemate; break;
        case LessonReply::When::Any: match = true; break;
        }
        if (match) { hit = &rp; break; }
    }
    bool solved = failures >= 3 && !e.solution.empty();   // the solution was demonstrated already
    if (solved) {
        r.before.push_back(say(line("lesson.almost"), Look::Player));
        r.after.push_back(hint2Beat(e));
        return r;
    }
    Line said = hit ? hit->line : line("lesson.try_again");
    std::string demo = hit ? hit->demo : std::string();
    if (demo == "escape") answerCheck(after, said, demo);
    if (said.empty()) said = line("lesson.try_again");
    r.before.push_back(say(withArg(said, "to", played), Look::Board));
    if (!demo.empty() && after.parseUCI(demo).valid()) {
        r.before.push_back(demoBeat(demo));
        if (hit && !hit->demoLine.empty()) r.before.push_back(say(hit->demoLine, Look::Player));
        r.before.push_back(rewindBeat(1));
    }
    if (hit && !hit->after.empty()) r.after.push_back(say(hit->after, Look::Player));
    if (failures == 1) {
        r.after.push_back(hint2Beat(e));
    } else if (failures == 2 && before.parseUCI(e.solution).valid()) {
        r.after.push_back(say(line("lesson.watch"), Look::Board));
        r.after.push_back(demoBeat(e.solution));
        r.after.push_back(rewindBeat(1));
        r.after.push_back(say(line("lesson.now_you"), Look::Player, {openPalm()}));
    }
    return r;
}

Script Lesson::explainIllegal(int expect, const Position& pos, Square from, Square to, PieceType promo) const {
    Script s;
    IllegalInfo info = whyIllegal(pos, from, to, promo);
    Piece moved = from >= 0 && from < 64 ? pos.at(from) : Piece();
    Line l;
    // The exercise's own line for this attempt.
    if (expect >= 0 && size_t(expect) < expectations_.size()) {
        std::string uci = squareName(from) + squareName(to);
        for (const LessonReply& rp : expectations_[size_t(expect)].illegal)
            if (rp.when == LessonReply::When::Any || contains(rp.moves, uci)) { l = rp.line; break; }
    }
    if (l.empty()) {
        switch (info.reason) {
        case IllegalReason::None:
        case IllegalReason::NoPiece:
        case IllegalReason::NeedsPromotionPiece:
            return s;   // nothing to explain (the promotion chooser is on screen)
        default: break;
        }
        l.key = std::string("why.") + illegalReasonName(info.reason);
    }
    if (info.square != NoSquare) l.with("sq", Arg::ofSquare(info.square));
    else l.with("sq", Arg::ofSquare(to));
    if (info.culprit != NoSquare && !pos.at(info.culprit).empty()) {
        Piece c = pos.at(info.culprit);
        l.with("my", Arg::ofPiece(c.type, c.color, c.color == pos.sideToMove(), info.culprit));
    } else {
        l.with("my", Arg::ofPiece(King, opposite(pos.sideToMove()), false, pos.kingSquare(opposite(pos.sideToMove()))));
    }
    if (!moved.empty()) l.with("your", Arg::ofPiece(moved.type, moved.color, true, from));
    l.with("to", Arg::ofSquare(to));

    std::vector<Gesture> g;
    std::vector<Mark> m;
    const char* anchor = "";
    switch (info.reason) {
    case IllegalReason::KingIntoCheck:
    case IllegalReason::CastlingThroughCheck:
    case IllegalReason::LeavesKingInCheck: anchor = "my"; break;
    case IllegalReason::PathBlocked:
    case IllegalReason::MustAnswerCheck:
    case IllegalReason::PawnForwardBlocked: anchor = "@"; break;
    default: break;
    }
    if (info.culprit != NoSquare) {
        Gesture p;
        p.kind = pos.at(info.culprit).empty() ? GestureKind::PointSquare : GestureKind::PointPiece;
        p.square = info.culprit;
        p.anchor = anchor;
        g.push_back(p);
        Mark mk;
        mk.kind = Mark::Kind::Piece;
        mk.square = info.culprit;
        mk.anchor = anchor;
        m.push_back(mk);
    }
    if (info.square != NoSquare) {
        Mark mk;
        mk.kind = Mark::Kind::Square;
        mk.square = info.square;
        m.push_back(mk);
    }
    Beat b = say(l, g.empty() && m.empty() ? Look::Player : Look::Target, g, m);
    b.priority = Priority::Urgent;   // research-pedagogy §6.5: the L0 illegal-move explanation is P1
    s.push_back(b);
    return s;
}

Script Lesson::idleHint(int expect, int stage) const {
    Script s;
    if (expect < 0 || size_t(expect) >= expectations_.size()) return s;
    const Expectation& e = expectations_[size_t(expect)];
    if (stage <= 1) {
        Line l = !e.hint1.empty() ? e.hint1 : e.ask;
        if (!l.empty()) s.push_back(say(l, Look::Player));
    } else {
        s.push_back(hint2Beat(e));
    }
    for (Beat& b : s) b.priority = Priority::Low;
    return s;
}

std::vector<Line> Lesson::chapterLines(int chapter) const {
    std::vector<Line> out;
    if (chapter < 0 || size_t(chapter) >= chapters_.size()) return out;
    auto add = [&](const Line& l) {
        if (!l.empty()) out.push_back(l);
    };
    for (const Beat& b : chapters_[size_t(chapter)].beats) {
        add(b.line);
        if (b.kind != BeatKind::WaitMove) continue;
        const Expectation& e = expectations_[size_t(b.expect)];
        add(e.success);
        for (auto& p : e.successFor) add(p.second);
        for (auto& p : e.acceptOnRetry) add(p.line);
        add(e.hint1);
        add(e.hint2);
    }
    return out;
}

}  // namespace coach
