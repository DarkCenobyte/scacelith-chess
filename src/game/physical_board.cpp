#include "physical_board.h"
#include "layout.h"

using namespace m;
using namespace chess;

namespace game {

int PhysicalBoard::newPiece(PieceType t, Color c) {
    PieceObject p;
    p.id = int(pieces_.size());
    p.type = t;
    p.color = c;
    p.yaw = defaultYaw(c);
    pieces_.push_back(p);
    return p.id;
}

void PhysicalBoard::reset(bool clockOnPositiveX) {
    clockPosX_ = clockOnPositiveX;
    pieces_.clear();
    captureCount_[0] = captureCount_[1] = 0;
    Position start;
    for (int sq = 0; sq < 64; ++sq) {
        Piece pc = start.at(Square(sq));
        if (pc.empty()) continue;
        int id = newPiece(pc.type, pc.color);
        setOnSquare(id, Square(sq));
    }
    // A spare queen per colour stands beside the board for promotions, as in tournaments.
    for (Color c : {White, Black}) {
        int id = newPiece(Queen, c);
        PieceObject* p = byId(id);
        p->inReserve = true;
        p->basePos = reserveSlot(c);
    }
    updateRestingTransforms();
    for (auto& p : pieces_) p.prevTransform = p.transform;
}

PieceObject* PhysicalBoard::at(Square sq) {
    for (auto& p : pieces_)
        if (p.square == sq && !p.captured && !p.inReserve) return &p;
    return nullptr;
}

PieceObject* PhysicalBoard::byId(int id) { return id >= 0 && id < int(pieces_.size()) ? &pieces_[size_t(id)] : nullptr; }

int PhysicalBoard::idAt(Square sq) const {
    for (auto& p : pieces_)
        if (p.square == sq && !p.captured && !p.inReserve) return p.id;
    return -1;
}

vec3 PhysicalBoard::squareBase(Square sq) const { return layout::squareCenter(sq); }

vec3 PhysicalBoard::nextCaptureSlot(Color capturedColor) {
    // Captured pieces stand on the table on the side without the clock, each colour on the side
    // of the player who captured them (White captures black pieces -> White's side, +Z).
    int n = captureCount_[capturedColor]++;
    float sideX = clockPosX_ ? -1.0f : 1.0f;
    int row = n / 8, col = n % 8;
    float x = sideX * (layout::CAPTURE_ROW_X + float(row) * layout::CAPTURE_SPACING);
    float zSign = capturedColor == Black ? 1.0f : -1.0f;  // near the capturer
    float z = zSign * (0.03f + float(col) * layout::CAPTURE_SPACING * 0.62f);
    return {x, layout::TABLE_TOP_Y, z};
}

vec3 PhysicalBoard::reserveSlot(Color c) const {
    // Spare queens wait at the far corner of the clock-free side, near their owner.
    float sideX = clockPosX_ ? -1.0f : 1.0f;
    float z = c == White ? 0.33f : -0.33f;
    return {sideX * (layout::BOARD_SIZE * 0.5f + 0.05f), layout::TABLE_TOP_Y, z};
}

void PhysicalBoard::setOnSquare(int id, Square sq) {
    PieceObject* p = byId(id);
    if (!p) return;
    p->square = sq;
    p->captured = false;
    p->inReserve = false;
    p->held = false;
    p->basePos = squareBase(sq);
}

void PhysicalBoard::setCaptured(int id, vec3 pos) {
    PieceObject* p = byId(id);
    if (!p) return;
    p->square = NoSquare;
    p->captured = true;
    p->inReserve = false;
    p->held = false;
    p->basePos = pos;
}

void PhysicalBoard::removeFromBoard(int id) {
    PieceObject* p = byId(id);
    if (!p) return;
    p->square = NoSquare;
    p->held = true;
}

int PhysicalBoard::takeSpare(PieceType t, Color c) {
    // Prefer a captured piece of that type and colour, as players do, then a spare piece. The
    // piece keeps its current place until it is put on a square (setOnSquare).
    for (auto& p : pieces_)
        if (p.captured && !p.held && p.type == t && p.color == c) return p.id;
    for (auto& p : pieces_)
        if (p.inReserve && !p.held && p.type == t && p.color == c) return p.id;
    int id = newPiece(t, c);
    PieceObject* p = byId(id);
    p->inReserve = true;
    p->basePos = reserveSlot(c);
    p->transform = translate(p->basePos) * rotateY(p->yaw);
    p->prevTransform = p->transform;
    return id;
}

void PhysicalBoard::syncTo(const Position& pos) {
    // Reassign on-board pieces to match 'pos' with minimal changes: keep pieces that already
    // stand on a square with the right type/colour, move the others.
    std::vector<int> free;
    bool used[64] = {};
    for (auto& p : pieces_) {
        if (p.captured || p.inReserve) continue;
        p.held = false;
        if (p.square != NoSquare) {
            Piece want = pos.at(p.square);
            if (!want.empty() && want.type == p.type && want.color == p.color && !used[p.square]) {
                used[p.square] = true;
                p.basePos = squareBase(p.square);
                continue;
            }
        }
        free.push_back(p.id);
    }
    for (int sq = 0; sq < 64; ++sq) {
        if (used[sq]) continue;
        Piece want = pos.at(Square(sq));
        if (want.empty()) continue;
        int found = -1;
        for (size_t k = 0; k < free.size(); ++k) {
            PieceObject* p = byId(free[k]);
            if (p->type == want.type && p->color == want.color) { found = int(k); break; }
        }
        int id;
        if (found >= 0) {
            id = free[size_t(found)];
            free.erase(free.begin() + found);
        } else {
            id = takeSpare(want.type, want.color);
        }
        setOnSquare(id, Square(sq));
        used[sq] = true;
    }
    for (int id : free) setCaptured(id, nextCaptureSlot(byId(id)->color));
    updateRestingTransforms();
}

void PhysicalBoard::beginFrame() {
    for (auto& p : pieces_) p.prevTransform = p.transform;
}

void PhysicalBoard::updateRestingTransforms() {
    for (auto& p : pieces_) {
        if (p.held) continue;
        p.transform = translate(p.basePos) * rotateY(p.yaw);
    }
}

}  // namespace game
