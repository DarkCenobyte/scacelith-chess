// Taking moves back on the physical board by hand, without a teleport: a takeback in Coach mode,
// a demonstration line undone move by move after the coach played it on the table.
//
// PhysicalBoard::syncTo already knows where every piece must stand to show a position the way the
// table showed it (victims back from the captured pieces, the last set down first; a promotion's
// spare back to the reserve). planRewind runs it on a copy of the board and turns the difference
// into trips: each one piece carried by one hand (Reach, Lift, Carry, Place) from where it rests
// to where the snap would have put it, in an order that never sets a piece down on an occupied
// square or spot. Executing every trip leaves the table exactly as syncTo(target) would, with no
// piece jumping.
//
// Typical use (one ply at a time, last first, so that each step shows one move going back):
//     std::vector<coach::PieceTrip> trips = coach::planRewind(board, positionBefore);
//     for each trip, in order: Reach(id), Lift, Carry(trip.to.pos), Place(trip.to.pos), and when
//     the piece is released coach::applyTrip(board, trip) (or the equivalent setOnSquare /
//     setCaptured / setInReserve); at the end coach::tableMatches(board, positionBefore).
// One ply taken back needs at most three trips (a capture that promotes: the new piece off, the
// pawn and its victim back) and never a stop on another square.
// Off-board spots lie in the half of the player on their side of the table: pos.z > 0 is White's
// half. A hand only reaches its own player's half, so a trip from or to the other half belongs to
// the other robot (a takeback of the human's capture is made by the human's robot).
#pragma once
#include "../chess/chess.h"
#include "../game/physical_board.h"
#include "../math/math.h"
#include <cstdint>
#include <vector>

namespace coach {

// Where a piece rests between two trips.
enum class RestKind : uint8_t {
    Square,     // on a square of the board
    Captured,   // among the captured pieces beside the board (a victim, the pawn of a promotion)
    Reserve     // a spare piece waiting for a promotion
};

struct Rest {
    RestKind kind = RestKind::Square;
    chess::Square square = chess::NoSquare;   // RestKind::Square
    m::vec3 pos{0, 0, 0};                     // resting base centre (world): square centre, slot, reserve spot
};

struct PieceTrip {
    int pieceId = -1;
    chess::PieceType type = chess::NoPiece;
    chess::Color color = chess::White;
    Rest from, to;
    // A stop on a free square: the pieces of a multi-move rewind can block one another in a cycle
    // (a knight and a bishop that swapped squares); one of them waits aside and travels on later.
    bool park = false;
    // The target needs a piece the table does not hold anywhere (never the case when rewinding
    // moves played on this table): a new spare the arbiter brings. Create it with
    // PhysicalBoard::addSpare(type, color, from.pos) (applyTrip does) before carrying it.
    bool created = false;
};

// Every trip that turns the table 'now' into 'target' exactly as now.syncTo(target) would (the
// same piece ids on the same squares, captured at the same slots, spares back on the same reserve
// spots), in a safe order: a trip lands only on a square or spot nothing stands on at that moment.
// Pieces leaving for spots off the board go first (a promotion's spare back to the reserve), then
// pieces moving on the board (a castling rook before its king), then pieces coming from beside
// the board (the last captured first). Empty when the table already stands so.
// Every piece must be at rest (none held). A piece already on its square stays there even when a
// hand set it down slightly off centre (syncTo would centre it).
std::vector<PieceTrip> planRewind(const game::PhysicalBoard& now, const chess::Position& target);

// The end of a trip on the board: the piece rests at trip.to (setOnSquare, setCaptured or
// setInReserve), created first when the trip says so.
void applyTrip(game::PhysicalBoard& board, const PieceTrip& trip);

// The table shows 'position': every square holds the piece it should (type and colour), no square
// holds two, and no piece is in a hand.
bool tableMatches(const game::PhysicalBoard& board, const chess::Position& position);

// Capture slot for the victim of a demonstration move, set down in 'half' (the coach's half, the
// only one its hand reaches) whoever captured it. Keeps the slot for the piece, like
// nextCaptureSlot. When that is the victim's own half, the piece is marked
// (PieceObject::capturedBesideOwner): syncTo, and so planRewind, does not count it as a pawn set
// down at a promotion, takeSpare does not use it for a promotion, and the rewind of the capture
// brings it back to its square like any victim. (A pawn promoted in a demonstration can simply be
// set down at board.nextCaptureSlot(half, pawnId): beside the opponent it counts as captured,
// which brings it back as well.)
m::vec3 demoCaptureSlot(game::PhysicalBoard& board, int victimId, chess::Color half);

}  // namespace coach
