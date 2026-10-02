// Captured pieces beside the board: the slot grid (layout.h) and PhysicalBoard's slot allocator,
// promotions and synchronisation.
#include "test.h"
#include "chess/chess.h"
#include "game/layout.h"
#include "game/physical_board.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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

// What the table shows, whichever piece is which: type, colour, state and place of every piece.
std::vector<std::string> layoutOf(const PhysicalBoard& b) {
    std::vector<std::string> out;
    for (const PieceObject& p : b.pieces()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%d %d %d %d %d %.4f %.4f", int(p.type), int(p.color), int(p.square), int(p.captured),
                      int(p.inReserve), p.basePos.x, p.basePos.z);
        out.push_back(buf);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Pawns of colour c standing off the board: [0] beside Black, [1] beside White.
void pawnsBeside(const PhysicalBoard& b, Color c, int out[2]) {
    out[0] = out[1] = 0;
    for (const PieceObject& p : b.pieces())
        if (p.captured && p.type == Pawn && p.color == c) ++out[p.basePos.z > 0.0f ? 1 : 0];
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

TEST(capture_slots_promotion_taken_back_returns_the_spare) {
    // A promotion completed on the board, then taken back (syncTo the position before it: an
    // illegal move under hints off, an online move never confirmed): the new piece goes back to
    // the reserve, the pawn to its square, a victim to the board, and the table is as it was. The
    // promotion played again takes the same spare: no piece is added.
    struct Case {
        const char* fen;
        int from, to;
        PieceType promotion;
    };
    const Case cases[] = {
        {"k7/4P3/8/8/8/8/8/3QK3 w - - 0 1", 52, 60, Queen},    // e8=Q: the spare queen
        {"k2r4/4P3/8/8/8/8/8/3QK3 w - - 0 1", 52, 59, Queen},  // exd8=Q
        {"k7/4P3/8/8/8/8/8/3QK3 w - - 0 1", 52, 60, Knight},   // e8=N: a knight the arbiter brings
        {"4k3/8/8/8/8/8/4p3/K7 b - - 0 1", 12, 4, Queen},      // e1=Q
    };
    for (bool clockPosX : {true, false}) {
        for (const Case& c : cases) {
            Position pos;
            CHECK(pos.setFEN(c.fen));
            Move mv = pos.findLegal(Square(c.from), Square(c.to), c.promotion);
            CHECK(mv.valid());
            PhysicalBoard b;
            b.reset(clockPosX);
            b.syncTo(pos);
            std::vector<Spot> before = spotsOf(b);
            int pawn = b.idAt(mv.from);
            playOnBoard(b, pos, mv);
            size_t played = b.pieces().size();
            int spare = b.idAt(mv.to);
            b.syncTo(pos);
            CHECK(boardMatches(b, pos));
            CHECK_EQ(b.idAt(mv.from), pawn);
            CHECK(standsAsBefore(b, before));
            CHECK(b.byId(spare)->inReserve && !b.byId(spare)->captured);
            CHECK_EQ(offBoardOverlaps(b), 0);
            CHECK(!anyReserved(b));
            for (int again = 0; again < 2; ++again) {
                playOnBoard(b, pos, mv);
                CHECK_EQ(b.idAt(mv.to), spare);
                CHECK_EQ(b.pieces().size(), played);
                b.syncTo(pos);
                CHECK(standsAsBefore(b, before));
            }
        }
    }
}

TEST(capture_slots_promotion_taken_back_brings_its_pawn_back) {
    // White's a-pawn was captured earlier and stands beside Black. Taking back e8=Q brings back
    // the e-pawn set down beside White, not the a-pawn: the table then shows what a fresh
    // synchronisation of the position shows, with no promoted pawn beside White.
    Position start, pos;
    CHECK(start.setFEN("k7/8/8/8/8/8/1PPPPPPP/3QK3 w - - 0 1"));
    CHECK(pos.setFEN("k7/4P3/8/8/8/8/1PPP1PPP/3QK3 w - - 0 1"));
    PhysicalBoard b;
    b.reset(true);
    b.syncTo(start);
    b.syncTo(pos);   // the e-pawn goes to e7
    std::vector<Spot> before = spotsOf(b);
    int pawn = b.idAt(Square(52));
    Move mv = pos.findLegal(Square(52), Square(60), Queen);
    CHECK(mv.valid());
    playOnBoard(b, pos, mv);
    b.syncTo(pos);
    CHECK_EQ(b.idAt(Square(52)), pawn);
    CHECK(standsAsBefore(b, before));
    int beside[2];
    pawnsBeside(b, White, beside);
    CHECK_EQ(beside[0], 1);
    CHECK_EQ(beside[1], 0);
    PhysicalBoard fresh;
    fresh.reset(true);
    fresh.syncTo(pos);
    CHECK(layoutOf(b) == layoutOf(fresh));
}

TEST(capture_slots_promoted_piece_captured_in_a_resync) {
    // b8=Q played on the board, then a resync to the position after Rxb8 (an online reconnection):
    // the promoted queen stands beside Black as if the capture had been played, not back in the
    // reserve, and the pawn stays beside White.
    Position pos;
    CHECK(pos.setFEN("4k3/1P6/8/8/8/8/8/1r1QK3 w - - 0 1"));
    Move promo = pos.findLegal(Square(49), Square(57), Queen);
    CHECK(promo.valid());
    Position after = pos;
    after.makeMove(promo);
    Move rxb8 = after.findLegal(Square(1), Square(57));
    CHECK(rxb8.valid());
    Position end = after;
    end.makeMove(rxb8);
    PhysicalBoard synced, played;
    for (PhysicalBoard* b : {&synced, &played}) {
        b->reset(true);
        b->syncTo(pos);
        playOnBoard(*b, pos, promo);
    }
    int queen = synced.idAt(Square(57));
    synced.syncTo(end);
    playOnBoard(played, after, rxb8);
    CHECK(boardMatches(synced, end));
    CHECK(synced.byId(queen)->captured && !synced.byId(queen)->inReserve && synced.byId(queen)->basePos.z < 0.0f);
    CHECK(layoutOf(synced) == layoutOf(played));
    int beside[2];
    pawnsBeside(synced, White, beside);
    CHECK_EQ(beside[1], 1);
    // Taking the capture back brings the queen back on b8, and the promotion too: the queen back
    // in the reserve.
    synced.syncTo(after);
    CHECK(boardMatches(synced, after));
    CHECK_EQ(synced.idAt(Square(57)), queen);
    synced.syncTo(pos);
    CHECK(synced.byId(queen)->inReserve && length(synced.byId(queen)->basePos - synced.reserveSlot(White)) < 1e-6f);
    pawnsBeside(synced, White, beside);
    CHECK_EQ(beside[1], 0);
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

TEST(capture_slots_moves_taken_back) {
    // Capture-hungry random games in which the arbiter takes moves back (syncTo the position
    // before), every promotion and one move in five, before they are played again: each time the
    // table is exactly as it was (a piece the arbiter brought waits in the reserve), and playing
    // the move again adds no piece.
    int takenBack = 0, promotions = 0, captures = 0;
    for (uint64_t seed = 1; seed <= 60; ++seed) {
        Rng rng(seed * 7919);
        Game g;
        PhysicalBoard b;
        b.reset(seed % 2 == 1);
        bool ok = true;
        for (int ply = 0; ply < 300 && !g.isOver() && ok; ++ply) {
            Position before = g.position();
            Move mv = pickMove(before, rng);
            if (mv.promotion != NoPiece || rng.next() % 5 == 0) {
                std::vector<Spot> spots = spotsOf(b);
                playOnBoard(b, before, mv);
                size_t played = b.pieces().size();
                b.syncTo(before);
                ok = boardMatches(b, before) && standsAsBefore(b, spots) && offBoardOverlaps(b) == 0 && !anyReserved(b);
                ++takenBack;
                if (mv.promotion != NoPiece) ++promotions;
                if (!before.at(mv.to).empty() || (mv.flags & MoveEnPassant)) ++captures;
                playOnBoard(b, before, mv);
                ok = ok && b.pieces().size() == played;
            } else {
                playOnBoard(b, before, mv);
            }
            g.play(mv);
            ok = ok && boardMatches(b, g.position()) && offBoardOverlaps(b) == 0;
        }
        CHECK(ok);
    }
    CHECK(takenBack > 1500);
    CHECK(promotions > 40);
    CHECK(captures > 250);
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

// ---- Explicit operations --------------------------------------------------------------------------

namespace {

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

// 'b' brought to the state of 'want' (b after a syncTo) with the explicit operations only: the new
// pieces first, then each piece that changed, those set down among the captured in id order (the
// order syncTo sets them down in).
void rebuildWithCalls(PhysicalBoard& b, const PhysicalBoard& want) {
    for (size_t i = b.pieces().size(); i < want.pieces().size(); ++i) {
        const PieceObject& w = want.pieces()[i];
        CHECK_EQ(b.addSpare(w.type, w.color, w.reserveSpot), w.id);
    }
    for (const PieceObject& w : want.pieces()) {
        const PieceObject& p = *b.byId(w.id);
        bool same = p.square == w.square && p.captured == w.captured && p.inReserve == w.inReserve &&
                    length(p.basePos - w.basePos) < 1e-6f;
        if (same) continue;
        if (w.captured) b.setCaptured(w.id, w.basePos);
        else if (w.inReserve) b.setInReserve(w.id, w.basePos);
        else b.setOnSquare(w.id, w.square);
    }
}

}  // namespace

TEST(capture_slots_set_in_reserve_and_add_spare) {
    // e8=Q played, then the queen put back in the reserve and the pawn back on e7 by hand: the
    // table is as syncTo leaves it.
    Position pos;
    CHECK(pos.setFEN("k7/4P3/8/8/8/8/8/3QK3 w - - 0 1"));
    PhysicalBoard b;
    b.reset(true);
    b.syncTo(pos);
    Move mv = pos.findLegal(Square(52), Square(60), Queen);
    int pawn = b.idAt(Square(52));
    playOnBoard(b, pos, mv);
    int queen = b.idAt(Square(60));
    CHECK(b.byId(queen)->spare);
    PhysicalBoard snap = b;
    snap.syncTo(pos);
    b.setInReserve(queen, b.reserveSlot(White));
    b.setOnSquare(pawn, Square(52));
    const PieceObject* q = b.byId(queen);
    CHECK(q->inReserve && !q->captured && !q->held && q->square == NoSquare);
    CHECK(length(q->basePos - b.reserveSlot(White)) < 1e-6f && length(q->reserveSpot - b.reserveSlot(White)) < 1e-6f);
    CHECK(b.at(Square(60)) == nullptr);
    CHECK(boardMatches(b, pos));
    CHECK(sameState(b, snap));
    // The reserve piece is the one a promotion takes again.
    CHECK_EQ(b.takeSpare(Queen, White), queen);
    // A spare the arbiter brings.
    PhysicalBoard c = b;
    int n = c.takeSpare(Knight, White);
    CHECK_EQ(n, int(b.pieces().size()));
    CHECK_EQ(b.addSpare(Knight, White, c.byId(n)->reserveSpot), n);
    CHECK(sameState(b, c));
    CHECK_EQ(b.takeSpare(Knight, White), n);
    // setInReserve ends a plan and a hand's hold like the other operations.
    PhysicalBoard d = b;
    d.byId(queen)->held = true;
    d.nextCaptureSlot(Black, queen);
    d.setInReserve(queen, d.reserveSlot(White));
    CHECK(!d.byId(queen)->held && !d.byId(queen)->slotReserved);
}

TEST(capture_slots_every_synced_state_reachable_by_calls) {
    // Capture-hungry random games with jumps (syncTo to the position several moves later or
    // earlier, as online resyncs and takebacks do): each state syncTo leaves can be built with the
    // explicit operations (setOnSquare, setCaptured, setInReserve, addSpare), exactly.
    int jumps = 0, created = 0, toReserve = 0;
    for (uint64_t seed = 1; seed <= 40; ++seed) {
        Rng rng(seed * 104729);
        Game g;
        PhysicalBoard b;
        b.reset(seed % 2 == 0);
        std::vector<Position> seen{g.position()};
        for (int ply = 0; ply < 200 && !g.isOver(); ++ply) {
            Position before = g.position();
            Move mv = pickMove(before, rng);
            playOnBoard(b, before, mv);
            g.play(mv);
            seen.push_back(g.position());
            if (rng.next() % 5 != 0) continue;
            // An earlier position (a takeback) or a position ahead (a resync).
            Position to = seen[size_t(rng.next() % seen.size())];
            if (rng.next() % 3 == 0) {
                Game ahead = g;
                for (int k = 0; k < 4 && !ahead.isOver(); ++k) ahead.play(pickMove(ahead.position(), rng));
                to = ahead.position();
            }
            PhysicalBoard want = b, calls = b;
            want.syncTo(to);
            created += int(want.pieces().size() - b.pieces().size());
            for (const PieceObject& w : want.pieces())
                if (w.inReserve && w.id < int(b.pieces().size()) && !b.byId(w.id)->inReserve) ++toReserve;
            rebuildWithCalls(calls, want);
            CHECK(sameState(calls, want));
            ++jumps;
        }
    }
    // A position the table cannot show with its pieces: the arbiter brings a knight and a queen.
    Position odd;
    CHECK(odd.setFEN("1k6/8/8/8/8/8/8/NNNQQQK1 w - - 0 1"));
    for (bool clockPosX : {true, false}) {
        PhysicalBoard b;
        b.reset(clockPosX);
        PhysicalBoard want = b, calls = b;
        want.syncTo(odd);
        CHECK(boardMatches(want, odd));
        created += int(want.pieces().size() - b.pieces().size());
        rebuildWithCalls(calls, want);
        CHECK(sameState(calls, want));
    }
    std::fprintf(stderr, "  %d jumps, %d spares back in the reserve, %d pieces brought\n", jumps, toReserve, created);
    CHECK(jumps > 800);
    CHECK(created >= 4);
    CHECK(toReserve > 20);
}

TEST(capture_slots_victim_beside_its_owner) {
    // A demonstration keeps its victims within the coach's reach: Black's pawn taken by White is set
    // down in Black's half (coach::demoCaptureSlot marks it). Black then promotes, the pawn set down
    // in Black's half too, the spare queen on b1. Taking the promotion back sends the queen back to
    // the reserve: the victim does not count as a pawn set down at a promotion.
    Position start;
    CHECK(start.setFEN("4k3/8/8/2p5/8/8/1p6/4K1B1 w - - 0 1"));
    for (bool clockPosX : {true, false}) {
        PhysicalBoard b;
        b.reset(clockPosX);
        b.syncTo(start);
        Move bxc5 = start.findLegal(Square(6), Square(34));
        CHECK(bxc5.valid());
        int victim = b.idAt(Square(34));
        vec3 slot = b.nextCaptureSlot(Black, victim);
        b.byId(victim)->capturedBesideOwner = true;
        b.setOnSquare(b.idAt(Square(6)), Square(34));
        b.setCaptured(victim, slot);
        CHECK(b.byId(victim)->captured && b.byId(victim)->capturedBesideOwner && b.byId(victim)->basePos.z < 0.0f);
        Position afterCapture = start;
        afterCapture.makeMove(bxc5);
        Move b1q = afterCapture.findLegal(Square(9), Square(1), Queen);
        CHECK(b1q.valid());
        int pawn = b.idAt(Square(9));
        playOnBoard(b, afterCapture, b1q);
        int queen = b.idAt(Square(1));
        CHECK(b.byId(queen)->spare);
        CHECK(!b.byId(pawn)->capturedBesideOwner);
        CHECK(b.byId(victim)->capturedBesideOwner);   // setCaptured of other pieces leaves it
        // Back one move: the queen in the reserve, the pawn on b2, the victim still waiting.
        PhysicalBoard back = b;
        back.syncTo(afterCapture);
        CHECK(boardMatches(back, afterCapture));
        CHECK(back.byId(queen)->inReserve && length(back.byId(queen)->basePos - back.reserveSlot(Black)) < 1e-6f);
        CHECK_EQ(back.idAt(Square(9)), pawn);
        CHECK(back.byId(victim)->captured && back.byId(victim)->capturedBesideOwner);
        // Unmarked, the victim would pass for the promoted pawn and the queen would stay captured.
        PhysicalBoard unmarked = b;
        unmarked.byId(victim)->capturedBesideOwner = false;
        unmarked.syncTo(afterCapture);
        CHECK(unmarked.byId(queen)->captured);
        // Back one more: the victim on c5, unmarked.
        back.syncTo(start);
        CHECK(boardMatches(back, start));
        CHECK_EQ(back.idAt(Square(34)), victim);
        CHECK(!back.byId(victim)->capturedBesideOwner);
    }
    // A marked piece is not one to promote to, though it stands in its owner's half.
    Position pos;
    CHECK(pos.setFEN("4k3/1n6/8/8/8/8/1p6/4K3 b - - 0 1"));
    PhysicalBoard b;
    b.reset(true);
    b.syncTo(pos);
    int knight = b.idAt(Square(49));
    vec3 slot = b.nextCaptureSlot(Black, knight);
    b.byId(knight)->capturedBesideOwner = true;
    b.setCaptured(knight, slot);
    int spare = b.takeSpare(Knight, Black);
    CHECK(spare != knight);
    CHECK(b.byId(spare)->inReserve && b.byId(spare)->spare);
    // The plan of a normal capture, a square, the reserve: each clears the mark.
    b.nextCaptureSlot(White, knight);
    CHECK(!b.byId(knight)->capturedBesideOwner);
    b.byId(knight)->capturedBesideOwner = true;
    b.setOnSquare(knight, Square(49));
    CHECK(!b.byId(knight)->capturedBesideOwner);
    b.byId(knight)->capturedBesideOwner = true;
    b.setInReserve(knight, b.reserveSlot(Black));
    CHECK(!b.byId(knight)->capturedBesideOwner);
}

// A resync teleports pieces (an illegal move put back, the 7.5.2 correction, a replay jump): it is
// not motion, so each piece's previous transform is its new one and the motion blur draws no
// streak for that frame (as after reset()).
TEST(capture_slots_sync_snaps_without_motion) {
    Position before, after;
    CHECK(before.setFEN("rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e6 0 2"));
    CHECK(after.setFEN("rnbqkbnr/pppp1ppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2"));  // e5 gone
    PhysicalBoard b;
    b.reset(true);
    b.syncTo(before);
    b.beginFrame();
    int pawn = b.idAt(Square(36));  // e5
    vec3 was = b.byId(pawn)->basePos;
    b.syncTo(after);
    const PieceObject& p = *b.byId(pawn);
    CHECK(p.captured);
    CHECK(length(p.basePos - was) > 0.1f);
    int moving = 0;
    for (const PieceObject& q : b.pieces())
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (q.prevTransform.c[c][r] != q.transform.c[c][r]) ++moving;
    CHECK_EQ(moving, 0);
}
