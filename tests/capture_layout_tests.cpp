// Captured pieces beside the board: the slot grid (layout.h) and PhysicalBoard's slot allocator,
// promotions and synchronisation.
#include "test.h"
#include "chess/chess.h"
#include "game/layout.h"
#include "game/physical_board.h"
#include <algorithm>
#include <cmath>

using namespace m;
using namespace chess;
using game::PhysicalBoard;
using game::PieceObject;

namespace {

constexpr float kR = 0.019f;  // design footprint: a queen (the knight's nose reaches 18.7 mm)

float boxGap(vec2 p, float x0, float x1, float halfZ) {
    float dx = std::max({x0 - p.x, 0.0f, p.x - x1});
    float dz = std::max(0.0f, std::fabs(p.y) - halfZ);
    return std::sqrt(dx * dx + dz * dz);
}

// Pieces standing off the board (captured, spare) whose footprints overlap, or 0.
int offBoardOverlaps(const PhysicalBoard& b) {
    int n = 0;
    const auto& ps = b.pieces();
    for (size_t i = 0; i < ps.size(); ++i)
        for (size_t j = i + 1; j < ps.size(); ++j) {
            const PieceObject &a = ps[i], &c = ps[j];
            if (!(a.captured || a.inReserve) || !(c.captured || c.inReserve) || a.held || c.held) continue;
            float d = length(vec2(a.basePos.x - c.basePos.x, a.basePos.z - c.basePos.z));
            if (d < layout::PIECE_FOOTPRINT_RADIUS[a.type] + layout::PIECE_FOOTPRINT_RADIUS[c.type]) ++n;
        }
    return n;
}

bool boardMatches(const PhysicalBoard& b, const Position& pos) {
    for (int sq = 0; sq < 64; ++sq) {
        Piece want = pos.at(Square(sq));
        const PieceObject* p = b.at(Square(sq));
        if (want.empty() != (p == nullptr)) return false;
        if (p && (p->type != want.type || p->color != want.color)) return false;
    }
    return true;
}

bool anyReserved(const PhysicalBoard& b) {
    for (const PieceObject& p : b.pieces())
        if (p.slotReserved) return true;
    return false;
}

// A move as GameScene plays it on the physical board: the slots are handed out when the move is
// planned (planPlacement, planPromotionSwap), the pieces set down when the hand releases them.
void playOnBoard(PhysicalBoard& b, const Position& before, const Move& mv) {
    Color side = before.sideToMove();
    int moverId = b.idAt(mv.from);
    int victimId = b.idAt(mv.to);
    if (mv.flags & MoveEnPassant) victimId = b.idAt(Square(mv.to + (side == White ? -8 : 8)));
    vec3 victimSlot, pawnSlot;
    int spareId = -1;
    if (victimId >= 0) victimSlot = b.nextCaptureSlot(opposite(b.byId(victimId)->color), victimId);
    if (mv.promotion != NoPiece) {
        pawnSlot = b.nextCaptureSlot(side, moverId);
        spareId = b.takeSpare(mv.promotion, side);
    }
    // The hand lifts the victim off the board, then sets everything down.
    if (victimId >= 0) {
        b.byId(victimId)->held = true;
        b.byId(victimId)->square = NoSquare;
        b.setCaptured(victimId, victimSlot);
    }
    b.setOnSquare(moverId, mv.to);
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

// Deterministic random games that capture and promote whenever they can.
Move pickMove(const Position& pos, Rng& rng) {
    std::vector<Move> moves = pos.legalMoves(), eager;
    for (const Move& m : moves)
        if (m.promotion != NoPiece || !pos.at(m.to).empty() || (m.flags & MoveEnPassant)) eager.push_back(m);
    const std::vector<Move>& from = !eager.empty() && rng.next() % 4 != 0 ? eager : moves;
    return from[rng.next() % from.size()];
}

}  // namespace

// ---- The slot grid ------------------------------------------------------------------------------

TEST(capture_slots_capacity) {
    // A half holds at most 23 pieces at once: the 15 opponent pieces other than the king and the
    // player's 8 pawns set down at promotions.
    CHECK(layout::captureSlotCount() >= 23);
    CHECK(layout::captureSlotCount() <= layout::CAPTURE_MAX_SLOTS);
    // All the opponent's pieces fit before the late slots.
    CHECK(layout::captureSlotEarlyCount() >= 15);
    CHECK(layout::captureSlotEarlyCount() <= layout::captureSlotCount());
}

TEST(capture_slots_pairwise_pitch) {
    int n = layout::captureSlotCount();
    float closest = 1e9f;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) closest = std::min(closest, length(layout::captureSlot(i) - layout::captureSlot(j)));
    CHECK(closest >= layout::CAPTURE_PITCH - 1e-5f);
    // Two queens (or two knights nose to nose, 37.4 mm) keep 7 mm between them.
    CHECK(closest - 2.0f * kR >= 0.0069f);
    for (int t = Pawn; t <= King; ++t) CHECK(layout::PIECE_FOOTPRINT_RADIUS[t] >= layout::PIECE_BASE_RADIUS[t]);
    CHECK(layout::PIECE_FOOTPRINT_RADIUS[Queen] <= kR && layout::PIECE_FOOTPRINT_RADIUS[Knight] <= kR);
}

TEST(capture_slots_clearances) {
    const float clockX0 = layout::CLOCK_OFFSET_X - 0.5f * layout::CLOCK_DEPTH;
    const float clockX1 = layout::CLOCK_OFFSET_X + 0.5f * layout::CLOCK_DEPTH;
    const float tableFlatX = 0.5f * layout::TABLE_WIDTH - 0.016f, tableFlatZ = 0.5f * layout::TABLE_DEPTH - 0.016f;
    const vec2 reserve(layout::RESERVE_X, layout::RESERVE_Z);
    for (int k = 0; k < layout::captureSlotCount(); ++k) {
        vec2 s = layout::captureSlot(k);
        CHECK(s.x > 0.0f && s.y > 0.0f);
        CHECK(boxGap(s, clockX0, clockX1, 0.5f * layout::CLOCK_WIDTH) - kR >= 0.0059f);  // clock case
        CHECK(length(s - reserve) - 2.0f * kR >= 0.0059f);                                // spare queen
        CHECK(s.x - kR - 0.5f * layout::BOARD_SIZE >= 0.0099f);                           // board frame
        CHECK(tableFlatX - (s.x + kR) >= 0.0059f);                                        // table edge
        CHECK(tableFlatZ - (s.y + kR) >= 0.0059f);
        // The resting playing hand.
        bool underHand = s.x > layout::REST_HAND_MIN_X - layout::REST_HAND_CLEARANCE &&
                         s.x < layout::REST_HAND_MAX_X + layout::REST_HAND_CLEARANCE &&
                         s.y > layout::REST_HAND_MIN_Z - layout::REST_HAND_CLEARANCE;
        CHECK(!underHand);
        // No farther from the player's shoulder (about |x| 0.19, |z| 0.62) than the far rank.
        CHECK(length(s - vec2(0.19f, 0.62f)) < length(vec2(-3.5f * layout::SQUARE_SIZE - 0.19f, -3.5f * layout::SQUARE_SIZE - 0.62f)));
    }
}

TEST(capture_slots_fill_from_the_outside) {
    // The hand sets a captured piece down with the rest of the hand on the board side of it:
    // before the late slots, nothing stands on that side of the next slot within a hand's width
    // (earlier slots, the spare queen).
    const vec2 reserve(layout::RESERVE_X, layout::RESERVE_Z);
    auto onBoardSide = [](vec2 s, vec2 o) { return o.x < s.x - 1e-4f && s.x - o.x < 0.09f && std::fabs(o.y - s.y) < 0.05f; };
    for (int k = 0; k < layout::captureSlotEarlyCount(); ++k) {
        vec2 s = layout::captureSlot(k);
        for (int j = 0; j < k; ++j) CHECK(!onBoardSide(s, layout::captureSlot(j)));
        CHECK(!onBoardSide(s, reserve));
    }
}

// ---- The allocator ----------------------------------------------------------------------------

TEST(capture_slots_mirror_to_the_clock_side_and_the_capturer) {
    PhysicalBoard b;
    b.reset(true);
    vec3 w = b.captureSlot(White, 0), k = b.captureSlot(Black, 0);
    vec2 s = layout::captureSlot(0);
    CHECK(std::fabs(w.x - s.x) < 1e-6f && std::fabs(w.z - s.y) < 1e-6f);   // White sits at +Z
    CHECK(std::fabs(k.x - s.x) < 1e-6f && std::fabs(k.z + s.y) < 1e-6f);
    CHECK(std::fabs(w.y - layout::TABLE_TOP_Y) < 1e-6f);
    b.reset(false);
    w = b.captureSlot(White, 0);
    CHECK(std::fabs(w.x + s.x) < 1e-6f && std::fabs(w.z - s.y) < 1e-6f);
}

TEST(capture_slots_reserved_and_reused) {
    PhysicalBoard b;
    b.reset(true);
    // Two slots asked for before either piece is set down (a promotion with a capture) differ.
    int victim = b.idAt(Square(57));   // b8 knight
    int pawn = b.idAt(Square(9));      // b2 pawn
    vec3 s0 = b.nextCaptureSlot(White, victim);
    vec3 s1 = b.nextCaptureSlot(White, pawn);
    CHECK(length(s0 - s1) > 0.04f);
    CHECK(length(s0 - b.captureSlot(White, 0)) < 1e-6f);
    CHECK(length(s1 - b.captureSlot(White, 1)) < 1e-6f);
    // Asking again for the same piece replaces its reservation instead of taking another slot.
    CHECK(length(b.nextCaptureSlot(White, pawn) - s1) < 1e-6f);
    b.setCaptured(victim, s0);
    b.setCaptured(pawn, s1);
    CHECK(!anyReserved(b));
    // The pawn goes back on the board (the arbiter restores the position): the next piece set
    // down beside White fills its hole rather than a new slot.
    CHECK_EQ(b.takeSpare(Pawn, White), pawn);
    b.setOnSquare(pawn, Square(9));
    int other = b.idAt(Square(62));   // g8 knight
    vec3 s2 = b.nextCaptureSlot(White, other);
    CHECK(length(s2 - s1) < 1e-6f);
    // A reset forgets reservations.
    b.reset(true);
    CHECK(!anyReserved(b));
    CHECK(length(b.nextCaptureSlot(White, b.idAt(Square(57))) - b.captureSlot(White, 0)) < 1e-6f);
}

TEST(capture_slots_a_full_half) {
    // The most a half ever holds: Black's 15 pieces but the king, then White's 8 pawns promoted,
    // beside White with the spare queen, each in its own slot.
    PhysicalBoard b;
    b.reset(true);
    int placed = 0;
    for (int sq = 0; sq < 64; ++sq) {
        const PieceObject* p = b.at(Square(sq));
        if (!p || p->type == King || (p->color == White && p->type != Pawn)) continue;
        int id = p->id;
        b.removeFromBoard(id);
        b.setCaptured(id, b.nextCaptureSlot(White, id));
        ++placed;
    }
    CHECK_EQ(placed, 23);
    CHECK_EQ(offBoardOverlaps(b), 0);
    std::vector<vec3> at;
    for (const PieceObject& p : b.pieces())
        if (p.captured) at.push_back(p.basePos);
    for (size_t i = 0; i < at.size(); ++i) {
        CHECK(at[i].z > 0.0f);
        for (size_t j = i + 1; j < at.size(); ++j) CHECK(length(at[i] - at[j]) >= layout::CAPTURE_PITCH - 1e-4f);
    }
}

TEST(capture_slots_spares_beside_the_player) {
    PhysicalBoard b;
    b.reset(true);
    const vec3 reserve = b.reserveSlot(White);
    // The spare queen first, then new pieces in free slots beside the player, never on the
    // spare queen's spot.
    int q = b.takeSpare(Queen, White);
    CHECK(b.byId(q)->inReserve);
    CHECK(length(b.byId(q)->basePos - reserve) < 1e-6f);
    int n = b.takeSpare(Knight, White);
    CHECK(n != q);
    CHECK(b.byId(n)->inReserve);
    CHECK(length(b.byId(n)->basePos - reserve) >= layout::CAPTURE_PITCH - 1e-4f);
    CHECK(b.byId(n)->basePos.z > 0.0f);   // White's half
    CHECK_EQ(offBoardOverlaps(b), 0);
    // A second one does not land on the first.
    int n2 = b.takeSpare(Rook, White);
    CHECK(length(b.byId(n2)->basePos - b.byId(n)->basePos) >= layout::CAPTURE_PITCH - 1e-4f);
    // White's pieces captured by Black stand beside Black, out of White's reach: a promotion does
    // not take them back.
    PhysicalBoard c;
    c.reset(true);
    int wq = c.idAt(Square(3));   // d1 queen
    c.setCaptured(wq, c.nextCaptureSlot(Black, wq));
    CHECK(c.takeSpare(Queen, White) != wq);
}

TEST(capture_slots_promoted_pawn_beside_its_owner) {
    // Live: planPromotionSwap sets the pawn down beside its owner.
    Position pos;
    CHECK(pos.setFEN("4k3/1P6/8/8/8/8/8/4K3 w - - 0 1"));
    PhysicalBoard b;
    b.reset(true);
    b.syncTo(pos);
    Move mv = pos.findLegal(Square(49), Square(57), Queen);   // b8=Q
    CHECK(mv.valid());
    int pawn = b.idAt(Square(49));
    playOnBoard(b, pos, mv);
    pos.makeMove(mv);
    CHECK(boardMatches(b, pos));
    CHECK(b.byId(pawn)->captured && b.byId(pawn)->basePos.z > 0.0f);
    // syncTo follows the same rule: a second queen proves a promotion.
    Position p2;
    CHECK(p2.setFEN("4k3/8/1Q6/8/8/8/8/3QK3 w - - 0 1"));
    PhysicalBoard c;
    c.reset(true);
    c.syncTo(p2);
    CHECK(boardMatches(c, p2));
    int beside[2] = {0, 0};   // White's pawns off the board: [0] beside Black, [1] beside White
    for (const PieceObject& p : c.pieces())
        if (p.captured && p.type == Pawn && p.color == White) ++beside[p.basePos.z > 0.0f ? 1 : 0];
    CHECK_EQ(beside[1], 1);
    CHECK_EQ(beside[0], 7);
    CHECK_EQ(offBoardOverlaps(c), 0);
    // Synchronising again changes nothing.
    std::vector<vec3> before;
    for (const PieceObject& p : c.pieces()) before.push_back(p.basePos);
    c.syncTo(p2);
    for (size_t i = 0; i < before.size(); ++i) CHECK(length(c.pieces()[i].basePos - before[i]) < 1e-6f);
}

TEST(capture_slots_arbiter_restore_brings_the_victim_back) {
    // An illegal capture completed, then the position restored (syncTo): the captured piece
    // comes back from beside the capturer, no new piece appears.
    Position pos;
    CHECK(pos.setFEN("4k3/8/8/3n4/4P3/8/8/4K3 w - - 0 1"));
    PhysicalBoard b;
    b.reset(true);
    b.syncTo(pos);
    size_t count = b.pieces().size();
    Move mv = pos.findLegal(Square(28), Square(35));   // exd5
    CHECK(mv.valid());
    Position after = pos;
    playOnBoard(b, pos, mv);
    after.makeMove(mv);
    CHECK(boardMatches(b, after));
    b.syncTo(pos);
    CHECK(boardMatches(b, pos));
    CHECK_EQ(b.pieces().size(), count);
    CHECK_EQ(offBoardOverlaps(b), 0);
}

TEST(capture_slots_simulated_games) {
    // Capture-hungry random games played on the physical board as the game does, with online
    // resyncs (syncTo without a reset) now and then: no two pieces off the board ever overlap,
    // holes are filled again and the board always matches the position.
    int games = 0, captures = 0, promotions = 0, resyncs = 0, mostInAHalf = 0;
    for (uint64_t seed = 1; seed <= 60; ++seed) {
        Rng rng(seed);
        Game g;
        PhysicalBoard b;
        b.reset(seed % 2 == 0);
        bool ok = true;
        for (int ply = 0; ply < 300 && !g.isOver() && ok; ++ply) {
            Position before = g.position();
            Move mv = pickMove(before, rng);
            if (!before.at(mv.to).empty() || (mv.flags & MoveEnPassant)) ++captures;
            if (mv.promotion != NoPiece) ++promotions;
            playOnBoard(b, before, mv);
            g.play(mv);
            if (rng.next() % 7 == 0) {
                b.syncTo(g.position());
                ++resyncs;
            }
            int inHalf[2] = {0, 0};
            for (const PieceObject& p : b.pieces())
                if (p.captured) ++inHalf[p.basePos.z > 0.0f ? 0 : 1];
            mostInAHalf = std::max({mostInAHalf, inHalf[0], inHalf[1]});
            ok = boardMatches(b, g.position()) && offBoardOverlaps(b) == 0 && !anyReserved(b);
            // Every piece stands in a slot of its half, or on the spare queen's spot.
            for (const PieceObject& p : b.pieces()) {
                if (!(p.captured || p.inReserve)) continue;
                bool inSlot = false;
                for (Color c : {White, Black})
                    for (int k = 0; k < layout::captureSlotCount(); ++k)
                        if (length(p.basePos - b.captureSlot(c, k)) < 1e-5f) inSlot = true;
                if (!inSlot && !(p.inReserve && length(p.basePos - b.reserveSlot(p.color)) < 1e-5f)) ok = false;
            }
        }
        CHECK(ok);
        ++games;
    }
    CHECK_EQ(games, 60);
    CHECK(captures > 60 * 12);
    CHECK(promotions > 20);
    CHECK(resyncs > 100);
    CHECK(mostInAHalf >= 14);
}

TEST(capture_slots_hole_reuse_keeps_the_grid_compact) {
    // Pieces leave the grid and come back repeatedly (arbiter restores): the pieces off the board
    // always occupy the first slots of the filling order, whatever left.
    Position pos;
    CHECK(pos.setFEN("r1bqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));
    PhysicalBoard b;
    b.reset(true);
    b.syncTo(pos);   // the b8 knight beside White
    Position full;
    CHECK(full.setFEN("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));
    for (int i = 0; i < 5; ++i) {
        b.syncTo(full);   // back on b8
        b.syncTo(pos);    // captured again: the same first slot
        const PieceObject* n = nullptr;
        for (const PieceObject& p : b.pieces())
            if (p.captured) n = &p;
        CHECK(n && length(n->basePos - b.captureSlot(White, 0)) < 1e-6f);
    }
}
