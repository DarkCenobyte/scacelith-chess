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

// A spot beside the board is free when everything standing there is a pitch away.
constexpr float kClear = layout::CAPTURE_PITCH - 0.001f;

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
    captures_ = 0;
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
        p->inReserve = p->spare = true;
        p->basePos = p->reserveSpot = reserveSlot(c);
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

float PhysicalBoard::clearance(vec3 at, const std::vector<vec3>* alsoTaken) const {
    float gap = 1e9f;
    for (const PieceObject& p : pieces_) {
        if ((p.captured || p.inReserve) && !p.held) gap = std::min(gap, length(vec2(p.basePos.x - at.x, p.basePos.z - at.z)));
        if (p.slotReserved) gap = std::min(gap, length(vec2(p.reservedSlot.x - at.x, p.reservedSlot.z - at.z)));
    }
    if (alsoTaken)
        for (vec3 s : *alsoTaken) gap = std::min(gap, length(vec2(s.x - at.x, s.z - at.z)));
    return gap;
}

vec3 PhysicalBoard::freeCaptureSlot(Color beside, const std::vector<vec3>* alsoTaken) const {
    // The first slot clear of what stands beside the board and of the slots kept for pieces on
    // their way there: holes left by pieces brought back are filled again.
    int n = layout::captureSlotCount(), roomiest = 0;
    float roomiestGap = -1.0f;
    for (int k = 0; k < n; ++k) {
        vec3 s = captureSlot(beside, k);
        float gap = clearance(s, alsoTaken);
        if (gap >= kClear) return s;
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
    if (p) p->slotReserved = p->capturedBesideOwner = false;  // a new plan replaces its earlier one
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
    p->capturedBesideOwner = false;
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
    p->captureOrder = ++captures_;
}

void PhysicalBoard::setInReserve(int id, vec3 pos) {
    PieceObject* p = byId(id);
    if (!p) return;
    p->square = NoSquare;
    p->captured = false;
    p->inReserve = true;
    p->held = false;
    p->capturedBesideOwner = false;
    p->slotReserved = false;
    p->basePos = p->reserveSpot = pos;
}

int PhysicalBoard::addSpare(PieceType t, Color c, vec3 spot) {
    int id = newPiece(t, c);
    PieceObject* p = byId(id);
    p->inReserve = p->spare = true;
    p->basePos = p->reserveSpot = spot;
    p->transform = translate(p->basePos) * rotateY(p->yaw);
    p->prevTransform = p->transform;
    return id;
}

void PhysicalBoard::removeFromBoard(int id) {
    PieceObject* p = byId(id);
    if (!p) return;
    p->square = NoSquare;
    p->held = true;
}

int PhysicalBoard::takeSpare(PieceType t, Color c) { return offBoardPiece(t, c, true); }

int PhysicalBoard::offBoardPiece(PieceType t, Color c, bool inReach) {
    // A captured piece of that type and colour, as players do, the one set down last first: the
    // arbiter taking a move back brings back the piece that move took off the board (its victim,
    // the pawn of a promotion), not one captured earlier. The hand only takes one standing in the
    // player's half (a pawn set down there at a promotion): the player's pieces the opponent
    // captured stand beside the opponent, out of reach. A victim of a demonstration waiting in
    // its owner's half is not one to take: it must come back to its square when the line is
    // taken back. Then a spare piece. The piece keeps its current place until it is put on a
    // square (setOnSquare).
    const PieceObject* last = nullptr;
    for (const PieceObject& p : pieces_)
        if (p.captured && !p.held && p.type == t && p.color == c &&
            (!inReach || (towards(c) * p.basePos.z > 0.0f && !p.capturedBesideOwner)) &&
            (!last || p.captureOrder > last->captureOrder))
            last = &p;
    if (last) return last->id;
    for (auto& p : pieces_)
        if (p.inReserve && !p.held && p.type == t && p.color == c) return p.id;
    // The arbiter brings another one: it stands in a free slot beside the player (not on the
    // spare queen's spot, which may still be taken).
    return addSpare(t, c, freeCaptureSlot(c));
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
    std::vector<vec3> vacated;   // spots of the pieces coming back from beside the board
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
            const PieceObject* p = byId(id);
            if (p->captured || p->inReserve) vacated.push_back(p->basePos);
        }
        setOnSquare(id, Square(sq));
        used[sq] = true;
    }
    // The pieces left over leave the board. Pawns that were promoted stand beside their owner
    // (GameScene::planPromotionSwap): as many as the position proves, less those already set down
    // there (a demonstration's victim waiting in its owner's half is not one of them). Each promotion on the table set such a pawn down and took a spare out of the reserve:
    // while more spares are out than pawns stand beside their owner, a spare left over is the new
    // piece of a promotion taken back. It goes back where it was taken from, else to a free slot
    // beside its owner (where the arbiter brings pieces). The other pieces were captured and
    // stand beside the opponent, a promoted piece among them. The slots of the pieces that came
    // back stay free: a hand setting these pieces down one at a time finds them still taken.
    int beside[2] = {}, sparesOut[2] = {}, pawnsLeft[2] = {}, promote[2] = {};
    for (const PieceObject& p : pieces_) {
        if (p.captured && p.type == Pawn && !p.capturedBesideOwner && towards(p.color) * p.basePos.z > 0.0f) ++beside[p.color];
        if (p.spare && !p.inReserve) ++sparesOut[p.color];
    }
    for (int id : free)
        if (byId(id)->type == Pawn) ++pawnsLeft[byId(id)->color];
    for (Color c : {White, Black}) {
        promote[c] = std::clamp(provenPromotions(pos, c) - beside[c], 0, pawnsLeft[c]);
        beside[c] += promote[c];
    }
    for (int id : free) {
        PieceObject* p = byId(id);
        Color c = p->color;
        p->capturedBesideOwner = false;
        if (p->type == Pawn && promote[c] > 0) {
            --promote[c];
            setCaptured(id, freeCaptureSlot(c, &vacated));
        } else if (p->spare && sparesOut[c] > beside[c]) {
            --sparesOut[c];
            setInReserve(id, clearance(p->reserveSpot) >= kClear ? p->reserveSpot : freeCaptureSlot(c, &vacated));
        } else {
            setCaptured(id, freeCaptureSlot(opposite(c), &vacated));
        }
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
