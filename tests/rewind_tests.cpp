// Taking moves back on the physical board by hand (src/coach/rewind.h): the trips planRewind gives,
// carried out one at a time, leave the table exactly as PhysicalBoard::syncTo would, never set a
// piece down where another stands, and keep a demonstration's pieces within the coach's reach.
#include "test.h"
#include "chess/chess.h"
#include "coach/rewind.h"
#include "game/layout.h"
#include "game/physical_board.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace m;
using namespace chess;
using coach::PieceTrip;
using coach::RestKind;
using game::PhysicalBoard;
using game::PieceObject;

namespace {

constexpr float kClear = layout::CAPTURE_PITCH - 0.001f;

float flat(vec3 a, vec3 b) { return length(vec2(a.x - b.x, a.z - b.z)); }

// A move as GameScene plays it on the table (capture_layout_tests.cpp): the slots are handed out
// when the move is planned, the pieces set down when the hand releases them. coachHalf < 0: the
// game's rules (a victim beside its capturer, a promoted pawn beside its owner). Otherwise a
// demonstration by the coach of that colour: every piece leaving the board goes to its half.
void playOnBoard(PhysicalBoard& b, const Position& before, const Move& mv, int coachHalf = -1) {
    Color side = before.sideToMove();
    int moverId = b.idAt(mv.from);
    int victimId = b.idAt(mv.to);
    if (mv.flags & MoveEnPassant) victimId = b.idAt(Square(mv.to + (side == White ? -8 : 8)));
    vec3 victimSlot, pawnSlot;
    int spareId = -1;
    if (victimId >= 0)
        victimSlot = coachHalf < 0 ? b.nextCaptureSlot(opposite(b.byId(victimId)->color), victimId)
                                   : coach::demoCaptureSlot(b, victimId, Color(coachHalf));
    if (mv.promotion != NoPiece) {
        pawnSlot = b.nextCaptureSlot(coachHalf < 0 ? side : Color(coachHalf), moverId);
        spareId = b.takeSpare(mv.promotion, side);
    }
    b.setOnSquare(moverId, mv.to);
    if (victimId >= 0) b.setCaptured(victimId, victimSlot);
    if (mv.flags & (MoveCastleKing | MoveCastleQueen)) {
        int rank = rankOf(mv.from);
        bool king = (mv.flags & MoveCastleKing) != 0;
        b.setOnSquare(b.idAt(makeSquare(king ? 7 : 0, rank)), makeSquare(king ? 5 : 3, rank));
    }
    if (mv.promotion != NoPiece) {
        b.setCaptured(moverId, pawnSlot);
        b.setOnSquare(spareId, mv.to);
    }
}

// Two tables in exactly the same state, piece by piece.
bool sameState(const PhysicalBoard& a, const PhysicalBoard& b) {
    if (a.pieces().size() != b.pieces().size()) return false;
    for (size_t i = 0; i < a.pieces().size(); ++i) {
        const PieceObject &p = a.pieces()[i], &q = b.pieces()[i];
        if (p.id != q.id || p.type != q.type || p.color != q.color || p.square != q.square || p.captured != q.captured ||
            p.inReserve != q.inReserve || p.spare != q.spare || p.held != q.held || p.slotReserved != q.slotReserved ||
            p.capturedBesideOwner != q.capturedBesideOwner || p.captureOrder != q.captureOrder ||
            length(p.basePos - q.basePos) > 1e-6f || length(p.reserveSpot - q.reserveSpot) > 1e-6f)
            return false;
    }
    return true;
}

// Starts rich in promotions (pawns about to queen on both sides) besides the standard position.
const char* const kPromotionStarts[] = {
    "r3k3/1PP2ppp/8/8/8/8/PPP2pp1/K6R w - - 0 1",
    "1n2k2r/P1P1p1P1/8/3pP3/8/8/p1p3p1/R3K1N1 w Q d6 0 1",
};

// A game from the standard position (two seeds in three) or from a promotion-rich one.
Game startOf(uint64_t seed) {
    Game g;
    if (seed % 3 == 1) CHECK(g.resetFromFEN(kPromotionStarts[(seed / 3) % 2]));
    return g;
}

// Where each piece stands (by id).
struct Spot {
    Square square;
    bool captured, inReserve;
    vec3 basePos;
};

std::vector<Spot> spotsOf(const PhysicalBoard& b) {
    std::vector<Spot> s;
    for (const PieceObject& p : b.pieces()) s.push_back({p.square, p.captured, p.inReserve, p.basePos});
    return s;
}

// Every piece of 'before' stands as it stood then; pieces added since wait in the reserve.
bool standsAsBefore(const PhysicalBoard& b, const std::vector<Spot>& before) {
    const auto& ps = b.pieces();
    if (ps.size() < before.size()) return false;
    for (size_t i = 0; i < ps.size(); ++i) {
        const PieceObject& p = ps[i];
        if (i >= before.size()) {
            if (!p.inReserve || !p.spare) return false;
            continue;
        }
        const Spot& s = before[i];
        if (p.square != s.square || p.captured != s.captured || p.inReserve != s.inReserve || length(p.basePos - s.basePos) > 1e-6f)
            return false;
    }
    return true;
}

// Pieces standing off the board whose footprints overlap.
int offBoardOverlaps(const PhysicalBoard& b) {
    int n = 0;
    const auto& ps = b.pieces();
    for (size_t i = 0; i < ps.size(); ++i)
        for (size_t j = i + 1; j < ps.size(); ++j) {
            const PieceObject &a = ps[i], &c = ps[j];
            if (!(a.captured || a.inReserve) || !(c.captured || c.inReserve)) continue;
            if (flat(a.basePos, c.basePos) < layout::PIECE_FOOTPRINT_RADIUS[a.type] + layout::PIECE_FOOTPRINT_RADIUS[c.type]) ++n;
        }
    return n;
}

bool atRest(const PieceObject& p, const coach::Rest& r) {
    switch (r.kind) {
    case RestKind::Square: return !p.captured && !p.inReserve && p.square == r.square;
    case RestKind::Captured: return p.captured && flat(p.basePos, r.pos) < 1e-5f;
    case RestKind::Reserve: return p.inReserve && flat(p.basePos, r.pos) < 1e-5f;
    }
    return false;
}

// Something other than 'self' stands at 'spot' on the table.
bool occupied(const PhysicalBoard& b, const coach::Rest& spot, int self) {
    for (const PieceObject& o : b.pieces()) {
        if (o.id == self) continue;
        if (spot.kind == RestKind::Square) {
            if (!o.captured && !o.inReserve && o.square == spot.square) return true;
        } else if ((o.captured || o.inReserve) && flat(o.basePos, spot.pos) < kClear) {
            return true;
        }
    }
    return false;
}

int stageOf(const PieceTrip& t) {
    if (t.to.kind != RestKind::Square) return 0;
    return t.from.kind == RestKind::Square ? 1 : 2;
}

struct Tally {
    int rewinds = 0, trips = 0, parks = 0, created = 0, bad = 0, stageOrder = 0;
};

// Carries the trips out one by one on 'b' as the hands would, counting the faults: a trip that does
// not start where its piece rests, lands where something stands, or goes nowhere.
int carryOut(PhysicalBoard& b, const std::vector<PieceTrip>& trips) {
    int bad = 0;
    for (const PieceTrip& t : trips) {
        const PieceObject* p = b.byId(t.pieceId);
        if (t.created) {
            if (p || occupied(b, t.from, t.pieceId)) ++bad;
        } else if (!p || !atRest(*p, t.from)) {
            ++bad;
            continue;
        }
        if (occupied(b, t.to, t.pieceId)) ++bad;
        if (!t.created && atRest(*p, t.to)) ++bad;
        if (p && (p->type != t.type || p->color != t.color)) ++bad;
        coach::applyTrip(b, t);
    }
    b.updateRestingTransforms();
    return bad;
}

// Rewinds 'b' to 'target' by hand and checks the result against the snap.
bool rewindTo(PhysicalBoard& b, const Position& target, Tally& tally, std::vector<PieceTrip>* out = nullptr) {
    PhysicalBoard want = b;
    want.syncTo(target);
    std::vector<PieceTrip> trips = coach::planRewind(b, target);
    int bad = carryOut(b, trips);
    tally.bad += bad;
    ++tally.rewinds;
    tally.trips += int(trips.size());
    for (size_t i = 0; i < trips.size(); ++i) {
        tally.parks += trips[i].park;
        tally.created += trips[i].created;
    }
    if (out) *out = trips;
    return bad == 0 && sameState(b, want) && coach::tableMatches(b, target) && coach::planRewind(b, target).empty();
}

// The trips of one move taken back: in stage order (off the board, on it, back onto it), none to
// the captured pieces (a move taken back only brings pieces from there), no stop on a free square.
bool likeOneMoveBack(const std::vector<PieceTrip>& trips) {
    for (size_t i = 0; i < trips.size(); ++i) {
        if (trips[i].to.kind == RestKind::Captured || trips[i].park || trips[i].created) return false;
        if (i > 0 && stageOf(trips[i]) < stageOf(trips[i - 1])) return false;
    }
    return true;
}

// Every off-board end of the trips lies in 'half' (a hand only reaches its own half).
bool withinHalf(const std::vector<PieceTrip>& trips, Color half) {
    const float side = half == White ? 1.0f : -1.0f;
    for (const PieceTrip& t : trips)
        for (const coach::Rest* r : {&t.from, &t.to})
            if (r->kind != RestKind::Square && side * r->pos.z <= 0.0f) return false;
    return true;
}

bool playSANs(Position& pos, PhysicalBoard& b, std::vector<Position>& seen, const std::vector<const char*>& sans,
              int coachHalf = -1) {
    for (const char* s : sans) {
        Move mv = pos.parseSAN(s);
        if (!mv.valid()) {
            std::fprintf(stderr, "  cannot play %s in %s\n", s, pos.fen().c_str());
            return false;
        }
        playOnBoard(b, pos, mv, coachHalf);
        seen.push_back(pos);
        pos.makeMove(mv);
    }
    return true;
}

// Deterministic random moves that capture, castle, push pawns two squares (en passant chances) and
// promote whenever they can.
Move pickMove(const Position& pos, Rng& rng) {
    std::vector<Move> moves = pos.legalMoves(), eager;
    for (const Move& m : moves)
        if (m.flags & (MoveCapture | MoveCastleKing | MoveCastleQueen | MoveDoublePush | MovePromotion)) eager.push_back(m);
    const std::vector<Move>& from = !eager.empty() && rng.next() % 4 != 0 ? eager : moves;
    return from[rng.next() % from.size()];
}

}  // namespace

TEST(rewind_single_moves) {
    // One move of each kind taken back by hand: the trips it takes, their order, and the table as
    // it stood before the move.
    struct Case {
        const char* fen;
        const char* uci;
        size_t trips;
        PieceType first;   // the piece carried first (NoPiece: any)
    };
    const char* kStart = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    const char* kCastle = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";
    const char* kCastleB = "r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1";
    const Case cases[] = {
        {kStart, "e2e4", 1, Pawn},
        {kStart, "g1f3", 1, Knight},
        {"4k3/8/8/3n4/4P3/8/8/4K3 w - - 0 1", "e4d5", 2, Pawn},       // the capturer back, then the victim
        {"4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2", "e5d6", 2, Pawn},       // en passant
        {"4k3/8/8/8/3Pp3/8/8/4K3 b - d3 0 2", "e4d3", 2, Pawn},
        {kCastle, "e1g1", 2, Rook},                                   // the rook back before the king
        {kCastle, "e1c1", 2, Rook},
        {kCastleB, "e8g8", 2, Rook},
        {kCastleB, "e8c8", 2, Rook},
        {"k7/4P3/8/8/8/8/8/3QK3 w - - 0 1", "e7e8q", 2, Queen},        // the spare back to the reserve first
        {"k7/4P3/8/8/8/8/8/3QK3 w - - 0 1", "e7e8n", 2, Knight},       // a knight the arbiter brought
        {"k2r4/4P3/8/8/8/8/8/3QK3 w - - 0 1", "e7d8q", 3, Queen},      // capture and promotion
        {"k2r4/4P3/8/8/8/8/8/3QK3 w - - 0 1", "e7d8r", 3, Rook},
        {"4k3/8/8/8/8/8/4p3/K7 b - - 0 1", "e2e1q", 2, Queen},
        {"4k3/8/8/8/8/8/4p3/K2R4 b - - 0 1", "e2d1n", 3, Knight},
        {"4k3/8/8/8/8/8/4p3/K2R4 b - - 0 1", "e2d1b", 3, Bishop},
    };
    Tally tally;
    for (bool clockPosX : {true, false}) {
        for (const Case& c : cases) {
            Position pos;
            CHECK(pos.setFEN(c.fen));
            Move mv = pos.parseUCI(c.uci);
            CHECK(mv.valid());
            PhysicalBoard b;
            b.reset(clockPosX);
            b.syncTo(pos);
            std::vector<Spot> before = spotsOf(b);
            playOnBoard(b, pos, mv);
            std::vector<PieceTrip> trips;
            CHECK(rewindTo(b, pos, tally, &trips));
            CHECK_EQ(trips.size(), c.trips);
            CHECK(!trips.empty() && trips.front().type == c.first);
            CHECK(likeOneMoveBack(trips));
            CHECK(standsAsBefore(b, before));
            // The mover of a capture goes back before its victim comes back; a promotion's pawn
            // comes back from beside its owner.
            if (!pos.at(mv.to).empty() || (mv.flags & MoveEnPassant)) CHECK(trips.back().from.kind == RestKind::Captured);
            if (mv.promotion != NoPiece) CHECK(trips.front().to.kind == RestKind::Reserve);
            for (const PieceTrip& t : trips) CHECK(!t.park && !t.created);
            // The mover's side reaches every trip: the move is its own.
            CHECK(withinHalf(trips, pos.sideToMove()));
            // Nothing left to do.
            CHECK(coach::planRewind(b, pos).empty());
        }
    }
    CHECK_EQ(tally.bad, 0);
}

TEST(rewind_scripted_lines) {
    // Lines with captures, en passant both ways, castling on both sides for both colours,
    // promotions (under-promotions, captures that promote, both colours), played on the table then
    // taken back one move at a time: every step as the snap would set it, and the table in the end
    // exactly as it was. The whole line taken back at once also ends as the snap would set it.
    struct Line {
        const char* fen;
        std::vector<const char*> sans;
    };
    const Line lines[] = {
        {nullptr, {"e4", "d5", "e5", "f5", "exf6", "Nxf6", "Nf3", "Bg4", "Be2", "Qd6", "O-O", "Nc6", "d4", "O-O-O"}},
        {nullptr, {"Nf3", "d5", "Ng1", "d4", "e4", "dxe3", "fxe3", "e5", "Qh5", "Nc6", "Qxe5+", "Nxe5"}},
        {"1n2k3/P1P5/8/8/8/8/5p1p/K5N1 w - - 0 1", {"axb8=N", "Ke7", "c8=R", "hxg1=Q+", "Kb2", "f1=B", "Rc7+", "Kd6"}},
        {"3r3k/4P3/8/8/8/8/5p2/K7 w - - 0 1", {"exd8=Q+", "Kh7", "Kb2", "f1=Q", "Qd3+", "Qxd3"}},
    };
    Tally tally;
    int stepsChecked = 0;
    for (bool clockPosX : {true, false}) {
        for (const Line& line : lines) {
            Position pos;
            if (line.fen) CHECK(pos.setFEN(line.fen));
            PhysicalBoard b;
            b.reset(clockPosX);
            b.syncTo(pos);
            const Position start = pos;
            const std::vector<Spot> initial = spotsOf(b);
            std::vector<Position> seen;
            std::vector<std::vector<Spot>> spots;
            for (const char* s : line.sans) {
                spots.push_back(spotsOf(b));
                CHECK(playSANs(pos, b, seen, {s}));
            }
            PhysicalBoard atEnd = b;
            // One move at a time.
            for (size_t k = seen.size(); k-- > 0;) {
                std::vector<PieceTrip> trips;
                CHECK(rewindTo(b, seen[k], tally, &trips));
                CHECK(standsAsBefore(b, spots[k]));
                CHECK(likeOneMoveBack(trips));
                CHECK_EQ(offBoardOverlaps(b), 0);
                ++stepsChecked;
            }
            CHECK(standsAsBefore(b, initial));
            // The whole line at once, and the moves of the last two plies at once.
            PhysicalBoard all = atEnd;
            CHECK(rewindTo(all, start, tally));
            CHECK_EQ(offBoardOverlaps(all), 0);
            PhysicalBoard two = atEnd;
            CHECK(rewindTo(two, seen[seen.size() - 2], tally));
        }
    }
    CHECK_EQ(tally.bad, 0);
    CHECK_EQ(stepsChecked, 2 * (14 + 12 + 8 + 6));
}

TEST(rewind_random_games) {
    // Capture-hungry random games in which moves are taken back now and then, 1 to 6 of them, one
    // at a time or all at once, the game record following (Game::undo) and the game going on from
    // there: each rewind ends as the snap would set it, one at a time exactly as the table stood.
    Tally single, several;
    int oneByOne = 0, atOnce = 0, promotions = 0, castlings = 0, enPassant = 0, captures = 0;
    for (uint64_t seed = 1; seed <= 80; ++seed) {
        Rng rng(seed * 6151);
        Game g = startOf(seed);
        PhysicalBoard b;
        b.reset(seed % 2 == 0);
        b.syncTo(g.position());
        // The table before each move, while it is known: a rewind of several moves at once may
        // swap two alike pieces (the snap's choice), after which the older records no longer hold.
        std::vector<std::vector<Spot>> spots;
        std::vector<bool> known;
        bool ok = true;
        for (int step = 0; step < 400 && ok; ++step) {
            const size_t n = g.moves().size();
            if (n > 0 && (g.isOver() || rng.next() % 10 == 0)) {
                const int k = 1 + int(rng.next() % std::min<size_t>(n, 6));
                for (size_t i = n - size_t(k); i < n; ++i) {
                    const Move& m = g.moves()[i];
                    promotions += m.promotion != NoPiece;
                    castlings += (m.flags & (MoveCastleKing | MoveCastleQueen)) != 0;
                    enPassant += (m.flags & MoveEnPassant) != 0;
                    captures += (m.flags & MoveCapture) != 0;
                }
                Game back = g;
                CHECK(back.undo(k));
                if (rng.next() % 2 == 0) {
                    for (int i = 1; i <= k && ok; ++i) {
                        Game to = g;
                        to.undo(i);
                        std::vector<PieceTrip> trips;
                        ok = rewindTo(b, to.position(), single, &trips) && offBoardOverlaps(b) == 0 &&
                             (!known[n - size_t(i)] || standsAsBefore(b, spots[n - size_t(i)]));
                        if (!likeOneMoveBack(trips)) ++single.stageOrder;
                        // The move taken back was played by one side: its hand reaches every trip.
                        if (!withinHalf(trips, to.position().sideToMove())) ++single.bad;
                    }
                    ++oneByOne;
                } else {
                    ok = rewindTo(b, back.position(), several) && offBoardOverlaps(b) == 0;
                    std::fill(known.begin(), known.end(), false);
                    ++atOnce;
                }
                g = back;
                spots.resize(g.moves().size());
                known.resize(g.moves().size());
                CHECK(g.position().samePosition(back.position()));
                continue;
            }
            spots.push_back(spotsOf(b));
            known.push_back(true);
            Position before = g.position();
            Move mv = pickMove(before, rng);
            playOnBoard(b, before, mv);
            g.play(mv);
        }
        CHECK(ok);
    }
    std::fprintf(stderr, "  %d rewinds one move at a time (%d trips), %d at once (%d trips, %d parked), %d promotions, "
                         "%d castlings, %d en passant, %d captures\n",
                         oneByOne, single.trips, atOnce, several.trips, several.parks, promotions, castlings, enPassant, captures);
    CHECK_EQ(single.bad, 0);
    CHECK_EQ(several.bad, 0);
    CHECK_EQ(single.stageOrder, 0);
    CHECK_EQ(single.parks, 0);
    CHECK(oneByOne > 1500 && atOnce > 1500);
    CHECK(several.parks > 0);   // pieces that swapped places now and then
    CHECK(promotions > 150 && castlings >= 10 && enPassant >= 3 && captures > 1500);
}

TEST(rewind_takeback_two_plies) {
    // The human asks for their move back on their turn: the coach's reply and the human's move go
    // back, each by the hand of the side that played it (the coach cannot reach the human's half,
    // where the human's captures stand), or both at once.
    int checked = 0;
    Tally tally;
    for (uint64_t seed = 1; seed <= 30; ++seed) {
        Rng rng(seed * 92821);
        Game g = startOf(seed);
        PhysicalBoard b;
        b.reset(seed % 2 == 1);
        b.syncTo(g.position());
        const Color human = (seed / 2) % 2 == 0 ? White : Black;
        for (int ply = 0; ply < 160 && !g.isOver(); ++ply) {
            Position before = g.position();
            Move mv = pickMove(before, rng);
            playOnBoard(b, before, mv);
            g.play(mv);
            if (g.moves().size() < 2 || g.position().sideToMove() != human || rng.next() % 3 != 0) continue;
            Game minus1 = g, minus2 = g;
            minus1.undo(1);
            minus2.undo(2);
            PhysicalBoard steps = b, once = b;
            std::vector<PieceTrip> coachTrips, humanTrips;
            CHECK(rewindTo(steps, minus1.position(), tally, &coachTrips));
            CHECK(withinHalf(coachTrips, opposite(human)));
            CHECK(rewindTo(steps, minus2.position(), tally, &humanTrips));
            CHECK(withinHalf(humanTrips, human));
            CHECK(rewindTo(once, minus2.position(), tally));
            CHECK(coach::tableMatches(steps, minus2.position()) && coach::tableMatches(once, minus2.position()));
            ++checked;
        }
    }
    CHECK_EQ(tally.bad, 0);
    CHECK(checked > 150);
}

TEST(rewind_demonstration_lines) {
    // The coach plays a line of up to eight moves for both sides on the table, then takes it back
    // one move at a time. Its victims and promoted pawns all stand in its own half (the only one
    // its hand reaches, coach::demoCaptureSlot), so every trip of the rewind stays there, except a
    // spare the human's side promoted to, which goes back to the human's reserve. In the end the
    // table stands exactly as before the line, and the game goes on.
    Tally tally;
    int lines = 0, plies = 0, victimsBesideOwner = 0, humanSpares = 0, coachPromotions = 0;
    for (uint64_t seed = 1; seed <= 40; ++seed) {
        Rng rng(seed * 31337);
        Game g = startOf(seed);
        PhysicalBoard b;
        b.reset(seed % 2 == 0);
        b.syncTo(g.position());
        const Color coachColor = seed % 4 < 2 ? Black : White;
        for (int ply = 0; ply < 200 && !g.isOver(); ++ply) {
            Position before = g.position();
            Move mv = pickMove(before, rng);
            playOnBoard(b, before, mv);
            g.play(mv);
            if (rng.next() % 4 != 0 || g.isOver()) continue;
            // A demonstration from here.
            const std::vector<Spot> spots = spotsOf(b);
            const size_t count = b.pieces().size();
            Game demo = g;
            std::vector<Position> seen;
            const int depth = 1 + int(rng.next() % 8);
            for (int d = 0; d < depth && !demo.isOver(); ++d) {
                Position p = demo.position();
                Move dm = pickMove(p, rng);
                if (p.sideToMove() != coachColor && dm.promotion != NoPiece) ++humanSpares;
                if (p.sideToMove() == coachColor && dm.promotion != NoPiece) ++coachPromotions;
                playOnBoard(b, p, dm, coachColor);
                seen.push_back(p);
                demo.play(dm);
                ++plies;
            }
            for (const PieceObject& p : b.pieces()) victimsBesideOwner += p.captured && p.capturedBesideOwner;
            for (size_t k = seen.size(); k-- > 0;) {
                std::vector<PieceTrip> trips;
                CHECK(rewindTo(b, seen[k], tally, &trips));
                CHECK(likeOneMoveBack(trips));
                for (const PieceTrip& t : trips) {
                    const float side = coachColor == White ? 1.0f : -1.0f;
                    for (const coach::Rest* r : {&t.from, &t.to}) {
                        if (r->kind == RestKind::Square) continue;
                        bool humanSpare = r->kind == RestKind::Reserve && t.color != coachColor;
                        CHECK(humanSpare || side * r->pos.z > 0.0f);
                    }
                }
            }
            CHECK(coach::tableMatches(b, g.position()));
            CHECK(standsAsBefore(b, spots));
            for (size_t i = count; i < b.pieces().size(); ++i) CHECK(b.pieces()[i].inReserve);
            CHECK_EQ(offBoardOverlaps(b), 0);
            ++lines;
        }
    }
    std::fprintf(stderr, "  %d demonstration lines, %d moves, %d victims beside their owner, %d promotions by the coach's side, "
                         "%d by the human's\n",
                         lines, plies, victimsBesideOwner, coachPromotions, humanSpares);
    CHECK_EQ(tally.bad, 0);
    CHECK(lines > 1000);
    CHECK(victimsBesideOwner > 200);
    CHECK(coachPromotions > 20 && humanSpares > 20);
}

TEST(rewind_demonstration_six_moves) {
    // Two six-move lines the coach shows on the table, then takes back one move at a time.
    // Coach Black after 1.e4 e5 2.Nf3 Nc6 3.Bc4 Nf6: 4.Ng5 d5 5.exd5 Nxd5 6.Nxf7 Kxf7, four
    // captures, two of Black's pawns set down in Black's half. Coach White in an endgame: both
    // sides castle, Black promotes (the human's side: its spare comes from Black's reserve and goes
    // back there), a rook takes the new queen, a capture promotes and White's new queen is taken,
    // set down in White's half.
    struct Line {
        const char* fen;
        std::vector<const char*> game, demo;
        Color coach;
        int besideOwner;   // the coach's pieces the line captures
    };
    const Line lines[] = {
        {nullptr, {"e4", "e5", "Nf3", "Nc6", "Bc4", "Nf6"}, {"Ng5", "d5", "exd5", "Nxd5", "Nxf7", "Kxf7"}, Black, 2},
        {"r3k2r/1P6/8/8/8/8/2p5/R3K2R w KQkq - 0 1", {}, {"O-O", "c1=Q", "Rfxc1", "O-O", "bxa8=Q", "Rxa8"}, White, 1},
    };
    for (bool clockPosX : {true, false}) {
        for (const Line& line : lines) {
            Position pos;
            if (line.fen) CHECK(pos.setFEN(line.fen));
            PhysicalBoard b;
            b.reset(clockPosX);
            b.syncTo(pos);
            std::vector<Position> seen;
            CHECK(playSANs(pos, b, seen, line.game));
            const Position real = pos;
            const std::vector<Spot> spots = spotsOf(b);
            seen.clear();
            CHECK(playSANs(pos, b, seen, line.demo, line.coach));
            CHECK_EQ(seen.size(), size_t(6));
            int besideOwner = 0;
            for (const PieceObject& p : b.pieces()) {
                if (!p.captured) continue;
                besideOwner += p.capturedBesideOwner;
                if (p.capturedBesideOwner) CHECK(p.color == line.coach);
            }
            CHECK_EQ(besideOwner, line.besideOwner);
            Tally tally;
            for (size_t k = seen.size(); k-- > 0;) {
                std::vector<PieceTrip> trips;
                CHECK(rewindTo(b, seen[k], tally, &trips));
                CHECK(!trips.empty() && trips.size() <= 3);
                CHECK(likeOneMoveBack(trips));
                const float side = line.coach == White ? 1.0f : -1.0f;
                for (const PieceTrip& t : trips)
                    for (const coach::Rest* r : {&t.from, &t.to})
                        if (r->kind == RestKind::Captured || (r->kind == RestKind::Reserve && t.color == line.coach))
                            CHECK(side * r->pos.z > 0.0f);
            }
            CHECK_EQ(tally.bad, 0);
            CHECK_EQ(tally.parks, 0);
            CHECK(coach::tableMatches(b, real));
            CHECK(standsAsBefore(b, spots));
        }
    }
}

TEST(rewind_demonstration_victim_and_promotion) {
    // Coach Black shows 1.Bxc5 b1=Q+ (Black's pawn set down in Black's half, then Black's pawn
    // promoted): taking b1=Q back sends the queen to Black's reserve, not beside White, and the
    // victim comes back last. All within Black's half.
    Position start;
    CHECK(start.setFEN("4k3/8/8/2p5/8/8/1p6/4K1B1 w - - 0 1"));
    for (bool clockPosX : {true, false}) {
        PhysicalBoard b;
        b.reset(clockPosX);
        b.syncTo(start);
        const std::vector<Spot> spots = spotsOf(b);
        Position pos = start;
        std::vector<Position> seen;
        CHECK(playSANs(pos, b, seen, {"Bxc5", "b1=Q+"}, Black));
        int victim = -1;
        for (const PieceObject& p : b.pieces())
            if (p.captured && p.capturedBesideOwner) victim = p.id;
        CHECK(victim >= 0 && b.byId(victim)->basePos.z < 0.0f);
        Tally tally;
        std::vector<PieceTrip> trips;
        CHECK(rewindTo(b, seen[1], tally, &trips));
        CHECK_EQ(trips.size(), size_t(2));
        CHECK(trips[0].type == Queen && trips[0].to.kind == RestKind::Reserve);
        CHECK(length(trips[0].to.pos - b.reserveSlot(Black)) < 1e-6f);
        CHECK(withinHalf(trips, Black));
        CHECK(rewindTo(b, seen[0], tally, &trips));
        CHECK_EQ(trips.size(), size_t(2));
        CHECK(trips[1].pieceId == victim && trips[1].to.square == Square(34));
        CHECK(withinHalf(trips, Black));
        CHECK(standsAsBefore(b, spots));
        CHECK_EQ(tally.bad, 0);
    }
    // demoCaptureSlot marks only a victim set down in its owner's half.
    PhysicalBoard b;
    b.reset(true);
    int blackPawn = b.idAt(Square(52)), whitePawn = b.idAt(Square(12));
    vec3 s1 = coach::demoCaptureSlot(b, blackPawn, Black);
    vec3 s2 = coach::demoCaptureSlot(b, whitePawn, Black);
    CHECK(b.byId(blackPawn)->capturedBesideOwner && !b.byId(whitePawn)->capturedBesideOwner);
    CHECK(s1.z < 0.0f && s2.z < 0.0f && flat(s1, s2) >= kClear);
    CHECK(b.byId(blackPawn)->slotReserved && b.byId(whitePawn)->slotReserved);
}

TEST(rewind_cycle_waits_on_a_free_square) {
    // Knight d2 and bishop e3 swapped places in five moves: taken back at once, one of them must
    // step aside first. It waits on a free square near it, then travels on.
    Position start;
    CHECK(start.setFEN("4k3/8/8/8/8/4B3/3N4/4K3 w - - 0 1"));
    PhysicalBoard b;
    b.reset(true);
    b.syncTo(start);
    Position pos = start;
    std::vector<Position> seen;
    CHECK(playSANs(pos, b, seen, {"Nf1", "Kd8", "Bd2", "Ke8", "Ne3"}));
    CHECK(b.at(Square(20))->type == Knight && b.at(Square(11))->type == Bishop);
    Tally tally;
    std::vector<PieceTrip> trips;
    CHECK(rewindTo(b, start, tally, &trips));
    CHECK_EQ(trips.size(), size_t(3));
    CHECK_EQ(tally.parks, 1);
    CHECK(trips[0].park && start.at(trips[0].to.square).empty() && pos.at(trips[0].to.square).empty());
    // The piece that waited goes on from there.
    CHECK(trips[2].pieceId == trips[0].pieceId && trips[2].from.square == trips[0].to.square);
    CHECK(std::max(std::abs(fileOf(trips[0].to.square) - fileOf(trips[0].from.square)),
                   std::abs(rankOf(trips[0].to.square) - rankOf(trips[0].from.square))) == 1);
    CHECK_EQ(tally.bad, 0);
}

TEST(rewind_to_a_position_the_table_cannot_show) {
    // Not a rewind of this table's moves: three knights and three queens for White. The snap
    // brings new pieces; the plan has them brought out on their spot, then carried.
    Position odd;
    CHECK(odd.setFEN("1k6/8/8/8/8/8/8/NNNQQQK1 w - - 0 1"));
    for (bool clockPosX : {true, false}) {
        PhysicalBoard b;
        b.reset(clockPosX);
        Tally tally;
        std::vector<PieceTrip> trips;
        CHECK(rewindTo(b, odd, tally, &trips));
        CHECK_EQ(tally.created, 2);
        CHECK_EQ(tally.bad, 0);
        CHECK_EQ(offBoardOverlaps(b), 0);
        // And back to the start: every piece comes back, the new ones wait in the reserve.
        Position start;
        CHECK(rewindTo(b, start, tally));
        CHECK_EQ(tally.bad, 0);
    }
}

TEST(rewind_table_matches) {
    PhysicalBoard b;
    b.reset(true);
    Position start, e4;
    CHECK(e4.setFEN("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1"));
    CHECK(coach::tableMatches(b, start));
    CHECK(!coach::tableMatches(b, e4));
    int pawn = b.idAt(Square(12));
    b.byId(pawn)->held = true;                     // in a hand: not on the table
    CHECK(!coach::tableMatches(b, start));
    b.setOnSquare(pawn, Square(28));
    CHECK(coach::tableMatches(b, e4));
    // A piece set down slightly off centre still stands on its square, and stays there.
    b.byId(pawn)->basePos += vec3(0.0015f, 0.0f, -0.0012f);
    CHECK(coach::tableMatches(b, e4));
    CHECK(coach::planRewind(b, e4).empty());
    std::vector<PieceTrip> trips = coach::planRewind(b, start);
    CHECK(trips.size() == 1 && trips[0].pieceId == pawn && trips[0].to.square == Square(12));
    // Two pieces on one square, a piece missing, a piece of the wrong colour.
    PhysicalBoard c;
    c.reset(true);
    c.setOnSquare(c.idAt(Square(1)), Square(8));
    CHECK(!coach::tableMatches(c, start));
    PhysicalBoard d;
    d.reset(true);
    d.setCaptured(d.idAt(Square(52)), d.captureSlot(White, 0));
    CHECK(!coach::tableMatches(d, start));
    Position swapped;
    CHECK(swapped.setFEN("r1bqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RnBQKBNR w KQkq - 0 1"));
    CHECK(!coach::tableMatches(b, swapped));
}
