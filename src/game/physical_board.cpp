#include "physical_board.h"
#include "../core/log.h"
#include "layout.h"
#include <algorithm>

using namespace m;
using namespace chess;

namespace game {

namespace {

// Promotions the position proves: every piece beyond the initial set (a third knight, a second
// queen) came from a pawn. A promoted piece that replaced a captured one leaves no trace.
int provenPromotions(const Position& pos, Color c) {
    static const int initial[7] = {0, 8, 2, 2, 2, 1, 1};
    int count[7] = {};
    for (int sq = 0; sq < 64; ++sq) {
        Piece pc = pos.at(Square(sq));
        if (!pc.empty() && pc.color == c) ++count[pc.type];
    }
    int n = 0;
    for (int t = Knight; t <= Queen; ++t) n += std::max(0, count[t] - initial[t]);
    return std::min(n, 8 - count[Pawn]);
}

float towards(Color player) { return player == White ? 1.0f : -1.0f; }  // White sits at +Z

}  // namespace

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

const PieceObject* PhysicalBoard::at(Square sq) const { return const_cast<PhysicalBoard*>(this)->at(sq); }

PieceObject* PhysicalBoard::byId(int id) { return id >= 0 && id < int(pieces_.size()) ? &pieces_[size_t(id)] : nullptr; }
const PieceObject* PhysicalBoard::byId(int id) const { return const_cast<PhysicalBoard*>(this)->byId(id); }

int PhysicalBoard::idAt(Square sq) const {
    for (auto& p : pieces_)
        if (p.square == sq && !p.captured && !p.inReserve) return p.id;
    return -1;
}

vec3 PhysicalBoard::squareBase(Square sq) const { return layout::squareCenter(sq); }

vec3 PhysicalBoard::captureSlot(Color beside, int k) const {
    vec2 s = layout::captureSlot(k);
    float sideX = clockPosX_ ? 1.0f : -1.0f;
    return {sideX * s.x, layout::TABLE_TOP_Y, towards(beside) * s.y};
}

vec3 PhysicalBoard::freeCaptureSlot(Color beside) const {
    // What stands beside the board (captured and spare pieces, not those in a hand) and the slots
    // kept for pieces on their way there. A slot is free when all of them are a pitch away: holes
    // left by pieces brought back for a promotion are filled again.
    std::vector<vec2> taken;
    for (const PieceObject& p : pieces_) {
        if ((p.captured || p.inReserve) && !p.held) taken.push_back(vec2(p.basePos.x, p.basePos.z));
        if (p.slotReserved) taken.push_back(vec2(p.reservedSlot.x, p.reservedSlot.z));
    }
    const float clear = layout::CAPTURE_PITCH - 0.001f;
    int n = layout::captureSlotCount(), roomiest = 0;
    float roomiestGap = -1.0f;
    for (int k = 0; k < n; ++k) {
        vec3 s = captureSlot(beside, k);
        float gap = 1e9f;
        for (vec2 t : taken) gap = std::min(gap, length(vec2(s.x, s.z) - t));
        if (gap >= clear) return s;
        if (gap > roomiestGap) {
            roomiestGap = gap;
            roomiest = k;
        }
    }
    // Far more pieces than a game can take off the board (at most 23 per half): the roomiest slot.
    LOGW("capture slots: %s's half is full, a piece goes %.0f mm from another", beside == White ? "White" : "Black",
         roomiestGap * 1000.0f);
    return captureSlot(beside, roomiest);
}

vec3 PhysicalBoard::nextCaptureSlot(Color beside, int pieceId) {
    PieceObject* p = byId(pieceId);
    if (p) p->slotReserved = false;  // a new plan replaces its earlier one
    vec3 slot = freeCaptureSlot(beside);
    if (p) {
        p->slotReserved = true;
        p->reservedSlot = slot;
    }
    return slot;
}

vec3 PhysicalBoard::reserveSlot(Color c) const {
    // Spare queens stand beyond the clock, near their owner (the playing hand's side).
    float sideX = clockPosX_ ? 1.0f : -1.0f;
    float z = c == White ? layout::RESERVE_Z : -layout::RESERVE_Z;
    return {sideX * layout::RESERVE_X, layout::TABLE_TOP_Y, z};
}

void PhysicalBoard::setOnSquare(int id, Square sq) {
    PieceObject* p = byId(id);
    if (!p) return;
    p->square = sq;
    p->captured = false;
    p->inReserve = false;
    p->held = false;
    p->slotReserved = false;
    p->basePos = squareBase(sq);
}

void PhysicalBoard::setCaptured(int id, vec3 pos) {
    PieceObject* p = byId(id);
    if (!p) return;
    p->square = NoSquare;
    p->captured = true;
    p->inReserve = false;
    p->held = false;
    p->slotReserved = false;
    p->basePos = pos;
}

void PhysicalBoard::removeFromBoard(int id) {
    PieceObject* p = byId(id);
    if (!p) return;
    p->square = NoSquare;
    p->held = true;
}

int PhysicalBoard::takeSpare(PieceType t, Color c) { return offBoardPiece(t, c, true); }

int PhysicalBoard::offBoardPiece(PieceType t, Color c, bool inReach) {
    // A captured piece of that type and colour, as players do. The hand only takes one standing in
    // the player's half (a pawn set down there at a promotion): the player's pieces the opponent
    // captured stand beside the opponent, out of reach. Then a spare piece. The piece keeps its
    // current place until it is put on a square (setOnSquare).
    for (auto& p : pieces_)
        if (p.captured && !p.held && p.type == t && p.color == c && (!inReach || towards(c) * p.basePos.z > 0.0f)) return p.id;
    for (auto& p : pieces_)
        if (p.inReserve && !p.held && p.type == t && p.color == c) return p.id;
    // The arbiter brings another one: it stands in a free slot beside the player (not on the
    // spare queen's spot, which may still be taken).
    vec3 pos = freeCaptureSlot(c);
    int id = newPiece(t, c);
    PieceObject* p = byId(id);
    p->inReserve = true;
    p->basePos = pos;
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
        p.held = false;
        p.slotReserved = false;  // the plans that kept slots are dropped
        if (p.captured || p.inReserve) continue;
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
            id = offBoardPiece(want.type, want.color, false);  // a snap: any piece off the board will do
        }
        setOnSquare(id, Square(sq));
        used[sq] = true;
    }
    // The pieces left over were captured, and stand beside the opponent, or are pawns that were
    // promoted, which stand beside their owner (GameScene::planPromotionSwap): as many as the
    // position proves, less those already set down there.
    int promoted[2] = {provenPromotions(pos, White), provenPromotions(pos, Black)};
    for (const PieceObject& p : pieces_)
        if (p.captured && p.type == Pawn && towards(p.color) * p.basePos.z > 0.0f) --promoted[p.color];
    for (int id : free) {
        const PieceObject* p = byId(id);
        Color beside = opposite(p->color);
        if (p->type == Pawn && promoted[p->color] > 0) {
            --promoted[p->color];
            beside = p->color;
        }
        setCaptured(id, nextCaptureSlot(beside, id));
    }
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
