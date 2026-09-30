// Physical chess set on the table: every piece is an object with a world transform, which may
// stand on a square, be held by a hand, sit among the captured pieces beside the board, or wait
// in the reserve (spare pieces used for promotions). The rules live in chess::Game; this class
// only mirrors what the players physically do.
#pragma once
#include "../chess/chess.h"
#include "../math/math.h"
#include <vector>

namespace game {

struct PieceObject {
    int id = -1;
    chess::PieceType type = chess::NoPiece;
    chess::Color color = chess::White;
    chess::Square square = chess::NoSquare;  // on-board square, NoSquare when off-board
    bool captured = false;                     // standing among the captured pieces
    bool inReserve = false;                    // spare piece not yet used
    bool spare = false;                        // not of the initial set: a reserve queen, one brought later
    m::vec3 reserveSpot{0, 0, 0};              // spare: where it stands while in the reserve
    int captureOrder = 0;                      // when it was last set down as captured: later is larger
    bool held = false;                         // following a hand
    // Captured, but set down in its owner's half rather than beside the capturer: a demonstration
    // keeps its victims within the coach's reach (coach::demoCaptureSlot). syncTo then does not
    // count it among the pawns set down at promotions, and takeSpare does not bring it back for a
    // promotion. Kept by setCaptured, cleared by nextCaptureSlot and whenever the piece goes back
    // on a square or into the reserve.
    bool capturedBesideOwner = false;
    bool slotReserved = false;                 // a capture slot waits for it (nextCaptureSlot)
    m::vec3 reservedSlot{0, 0, 0};
    m::vec3 basePos{0, 0, 0};                  // resting base centre (world)
    float yaw = 0.0f;                          // rotation about +Y (radians)
    m::mat4 transform;                         // current world transform (base centre origin)
    m::mat4 prevTransform;
};

class PhysicalBoard {
public:
    // clockOnPositiveX: side of the table holding the clock (captured and spare pieces go there too;
    // the scoresheets lie on the other side).
    void reset(bool clockOnPositiveX);
    std::vector<PieceObject>& pieces() { return pieces_; }
    const std::vector<PieceObject>& pieces() const { return pieces_; }
    PieceObject* at(chess::Square sq);
    const PieceObject* at(chess::Square sq) const;
    PieceObject* byId(int id);
    const PieceObject* byId(int id) const;
    int idAt(chess::Square sq) const;

    // Resting positions.
    m::vec3 squareBase(chess::Square sq) const;
    // Captured pieces and promoted pawns stand beside the board on the clock side, in the half of
    // the player 'beside' (who captured them, or who promoted the pawn). Returns the first free
    // capture slot of that half (layout::captureSlot) and keeps it for 'pieceId' until the piece
    // is set down (setCaptured, setOnSquare), the board is reset or synchronised (syncTo).
    m::vec3 nextCaptureSlot(chess::Color beside, int pieceId);
    m::vec3 captureSlot(chess::Color beside, int k) const;  // slot k of that half (world)
    m::vec3 reserveSlot(chess::Color c) const;
    float defaultYaw(chess::Color c) const { return c == chess::White ? m::PI : 0.0f; }

    // Physical operations (instantaneous; animations move the transforms in between). Together
    // they reach every state syncTo leaves a piece in: on a square, captured at a spot, back in
    // the reserve at a spot, and a new spare brought for a promotion.
    void setOnSquare(int id, chess::Square sq);
    void setCaptured(int id, m::vec3 pos);
    // A spare piece set back in the reserve, standing at 'pos' (its reserve spot from then on):
    // the new piece of a promotion taken back.
    void setInReserve(int id, m::vec3 pos);
    void removeFromBoard(int id);                 // picked up (square cleared)
    // Piece to bring in for a promotion: a captured one of that type/colour within the player's
    // reach, else a spare from the reserve (one queen per colour at start; created on demand in a
    // free capture slot beside the player). It stays where it is until setOnSquare().
    int takeSpare(chess::PieceType t, chess::Color c);
    // A new spare piece waiting in the reserve at 'spot' (the arbiter brings one when no piece of
    // that type is left: takeSpare, syncTo). Returns its id, the next one.
    int addSpare(chess::PieceType t, chess::Color c, m::vec3 spot);
    // Snap every piece to the given logical position (arbiter restoring the position, new game).
    // Pieces missing from the board come back from the captured ones, the last set down first, so
    // that taking a move back restores the table as it was: its victim, the pawn of a promotion.
    // The spare of a promotion taken back goes back to the reserve; the other pieces left over
    // are captured (syncTo in physical_board.cpp).
    void syncTo(const chess::Position& pos);

    void beginFrame();                            // prevTransform = transform
    void updateRestingTransforms();               // pieces not held get their resting transform

private:
    std::vector<PieceObject> pieces_;
    bool clockPosX_ = true;
    int captures_ = 0;                            // pieces set down as captured (captureOrder)
    int newPiece(chess::PieceType t, chess::Color c);
    // Distance from 'at' to the nearest piece standing beside the board (captured, spare; not in a
    // hand) or slot kept for one.
    float clearance(m::vec3 at) const;
    m::vec3 freeCaptureSlot(chess::Color beside) const;
    // takeSpare; inReach = false (syncTo snaps pieces into place) also takes captured pieces
    // standing beside the opponent.
    int offBoardPiece(chess::PieceType t, chess::Color c, bool inReach);
};

}  // namespace game
