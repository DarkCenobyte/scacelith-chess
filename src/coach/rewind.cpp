#include "rewind.h"
#include "../core/log.h"
#include "../game/layout.h"
#include <algorithm>
#include <climits>

using namespace m;
using namespace chess;
using game::PhysicalBoard;
using game::PieceObject;

namespace coach {

namespace {

// Spots beside the board closer than this share a place: PhysicalBoard keeps a pitch between the
// pieces standing there (its own clearance test).
constexpr float kClear = layout::CAPTURE_PITCH - 0.001f;
constexpr float kSameSpot = 1e-4f;

Rest restOf(const PieceObject& p) {
    Rest r;
    r.pos = p.basePos;
    if (p.captured) {
        r.kind = RestKind::Captured;
    } else if (p.inReserve) {
        r.kind = RestKind::Reserve;
    } else {
        r.kind = RestKind::Square;
        r.square = p.square;
    }
    return r;
}

bool offBoard(const Rest& r) { return r.kind != RestKind::Square; }

float flatDistance(vec3 a, vec3 b) { return length(vec2(a.x - b.x, a.z - b.z)); }

bool sameRest(const Rest& a, const Rest& b) {
    if (a.kind != b.kind) return false;
    if (a.kind == RestKind::Square) return a.square == b.square;
    return flatDistance(a.pos, b.pos) < kSameSpot;
}

// Order among the trips free to go: pieces leaving the board, then pieces moving on it (a castling
// rook before its king: the reverse of how castling is played), then pieces coming back to it, the
// last set down first. Pieces leaving for the captured ones go in id order, the order the snap
// sets them down in, so that they come back in the same order later (PieceObject::captureOrder).
long long priority(const PieceTrip& t, const PhysicalBoard& now) {
    const long long id = t.pieceId;
    if (offBoard(t.to)) return id;
    if (!offBoard(t.from)) return (1LL << 40) + (t.type == King ? (1LL << 20) : 0) + id;
    const PieceObject* p = now.byId(t.pieceId);
    if (t.from.kind == RestKind::Captured && p) return (2LL << 40) + (INT_MAX - (long long)p->captureOrder);
    return (3LL << 40) + id;   // spares out of the reserve, pieces the arbiter brings
}

}  // namespace

std::vector<PieceTrip> planRewind(const PhysicalBoard& now, const Position& target) {
    PhysicalBoard want = now;
    want.syncTo(target);

    std::vector<PieceTrip> pending;
    for (const PieceObject& b : want.pieces()) {
        PieceTrip t;
        t.pieceId = b.id;
        t.type = b.type;
        t.color = b.color;
        t.to = restOf(b);
        if (const PieceObject* a = now.byId(b.id)) {
            t.from = restOf(*a);
            if (sameRest(t.from, t.to)) continue;
        } else {
            t.created = true;   // the snap brought a new spare: it appeared on its reserve spot
            t.from.kind = RestKind::Reserve;
            t.from.pos = b.reserveSpot;
        }
        pending.push_back(t);
    }
    std::sort(pending.begin(), pending.end(),
              [&now](const PieceTrip& a, const PieceTrip& b) { return priority(a, now) < priority(b, now); });

    // What stands where while the trips are carried out: on the squares, and the pieces with a trip
    // still to make (the others never move, and the snap set nothing down near them). A piece the
    // snap brought counts as standing on its reserve spot from the start, as in the snap.
    int onSquare[64];
    std::fill(onSquare, onSquare + 64, -1);
    for (const PieceObject& p : now.pieces())
        if (!p.captured && !p.inReserve && p.square >= 0 && p.square < 64) onSquare[p.square] = p.id;
    std::vector<Rest> at(pending.size());
    std::vector<bool> done(pending.size(), false), unborn(pending.size(), false);
    for (size_t i = 0; i < pending.size(); ++i) {
        at[i] = pending[i].from;
        unborn[i] = pending[i].created;
    }
    // A spot (or square) is taken while a piece with a trip to make still stands on it. The snap
    // may bring two new pieces out on one spot, one after the other: they appear in turn.
    auto taken = [&](const Rest& spot, size_t self, bool birth) {
        for (size_t j = 0; j < pending.size(); ++j) {
            if (j == self || done[j] || (birth && unborn[j])) continue;
            if (spot.kind == RestKind::Square) {
                if (at[j].kind == RestKind::Square && at[j].square == spot.square) return true;
            } else if (offBoard(at[j]) && flatDistance(at[j].pos, spot.pos) < kClear) {
                return true;
            }
        }
        return false;
    };
    auto ready = [&](size_t i) {
        const PieceTrip& t = pending[i];
        if (taken(t.to, i, false)) return false;
        if (unborn[i] && taken(t.from, i, true)) return false;   // it appears once its spot is free
        if (t.to.kind == RestKind::Captured)
            for (size_t j = 0; j < pending.size(); ++j)
                if (!done[j] && j != i && pending[j].to.kind == RestKind::Captured && pending[j].pieceId < t.pieceId)
                    return false;
        return true;
    };
    auto move = [&](size_t i, const Rest& to) {
        if (at[i].kind == RestKind::Square && at[i].square >= 0 && onSquare[at[i].square] == pending[i].pieceId)
            onSquare[at[i].square] = -1;
        if (to.kind == RestKind::Square) onSquare[to.square] = pending[i].pieceId;
        at[i] = to;
        unborn[i] = false;
    };

    std::vector<PieceTrip> out;
    out.reserve(pending.size() + 2);
    for (size_t left = pending.size(); left > 0;) {
        size_t next = pending.size();
        for (size_t i = 0; i < pending.size() && next == pending.size(); ++i)
            if (!done[i] && ready(i)) next = i;
        if (next < pending.size()) {
            PieceTrip t = pending[next];
            t.from = at[next];
            out.push_back(t);
            move(next, t.to);
            done[next] = true;
            --left;
            continue;
        }
        // Blocked in a cycle: a piece standing where another must land waits on a square nothing
        // needs (empty now and in the target), the nearest to it.
        size_t blocker = pending.size();
        for (size_t i = 0; i < pending.size() && blocker == pending.size(); ++i) {
            if (done[i] || at[i].kind != RestKind::Square) continue;
            for (size_t j = 0; j < pending.size(); ++j)
                if (!done[j] && j != i && pending[j].to.kind == RestKind::Square && pending[j].to.square == at[i].square)
                    blocker = i;
        }
        Square park = NoSquare;
        if (blocker < pending.size()) {
            float best = 1e9f;
            for (int sq = 0; sq < 64; ++sq) {
                if (onSquare[sq] >= 0 || !target.at(Square(sq)).empty()) continue;
                float d = flatDistance(want.squareBase(Square(sq)), want.squareBase(at[blocker].square)) +
                          0.01f * flatDistance(want.squareBase(Square(sq)), pending[blocker].to.pos);
                if (d < best) {
                    best = d;
                    park = Square(sq);
                }
            }
        }
        if (park == NoSquare) {
            // Not reachable from a real table (a cycle always leaves a square free): keep going in order.
            LOGW("rewind: %d trips blocked, carried out in order", int(left));
            for (size_t i = 0; i < pending.size(); ++i)
                if (!done[i]) {
                    PieceTrip t = pending[i];
                    t.from = at[i];
                    out.push_back(t);
                    done[i] = true;
                }
            break;
        }
        PieceTrip stop = pending[blocker];
        stop.from = at[blocker];
        stop.to.kind = RestKind::Square;
        stop.to.square = park;
        stop.to.pos = want.squareBase(park);
        stop.park = true;
        stop.created = false;
        out.push_back(stop);
        move(blocker, stop.to);
    }
    return out;
}

void applyTrip(PhysicalBoard& board, const PieceTrip& trip) {
    if (trip.created && !board.byId(trip.pieceId)) {
        int id = board.addSpare(trip.type, trip.color, trip.from.pos);
        if (id != trip.pieceId) LOGW("rewind: new spare %d stands for piece %d", id, trip.pieceId);
    }
    switch (trip.to.kind) {
    case RestKind::Square: board.setOnSquare(trip.pieceId, trip.to.square); break;
    case RestKind::Captured:
        // As the snap sets it down: beside the capturer, not a demonstration's victim any more.
        if (PieceObject* p = board.byId(trip.pieceId)) p->capturedBesideOwner = false;
        board.setCaptured(trip.pieceId, trip.to.pos);
        break;
    case RestKind::Reserve: board.setInReserve(trip.pieceId, trip.to.pos); break;
    }
}

bool tableMatches(const PhysicalBoard& board, const Position& position) {
    int count[64] = {};
    for (const PieceObject& p : board.pieces()) {
        if (p.held) return false;
        if (p.captured || p.inReserve || p.square == NoSquare) continue;
        if (p.square < 0 || p.square > 63 || ++count[p.square] > 1) return false;
        Piece want = position.at(p.square);
        if (want.empty() || want.type != p.type || want.color != p.color) return false;
    }
    for (int sq = 0; sq < 64; ++sq)
        if (!position.at(Square(sq)).empty() && count[sq] == 0) return false;
    return true;
}

vec3 demoCaptureSlot(PhysicalBoard& board, int victimId, Color half) {
    vec3 slot = board.nextCaptureSlot(half, victimId);
    if (PieceObject* p = board.byId(victimId)) p->capturedBesideOwner = p->color == half;
    return slot;
}

}  // namespace coach
