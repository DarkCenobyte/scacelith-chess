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
    bool held = false;                         // following a hand
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
    m::vec3 nextCaptureSlot(chess::Color capturedColor);  // reserves and returns a slot
    m::vec3 reserveSlot(chess::Color c) const;
    float defaultYaw(chess::Color c) const { return c == chess::White ? m::PI : 0.0f; }

    // Physical operations (instantaneous; animations move the transforms in between).
    void setOnSquare(int id, chess::Square sq);
    void setCaptured(int id, m::vec3 pos);
    void removeFromBoard(int id);                 // picked up (square cleared)
    // Piece to bring in for a promotion: a captured one of that type/colour, else a spare from
    // the reserve (one queen per colour at start; created on demand). It stays where it is
    // until setOnSquare().
    int takeSpare(chess::PieceType t, chess::Color c);
    // Snap every piece to the given logical position (arbiter restoring the position, new game).
    void syncTo(const chess::Position& pos);

    void beginFrame();                            // prevTransform = transform
    void updateRestingTransforms();               // pieces not held get their resting transform

private:
    std::vector<PieceObject> pieces_;
    bool clockPosX_ = true;
    int captureCount_[2] = {0, 0};
    int newPiece(chess::PieceType t, chess::Color c);
};

}  // namespace game
