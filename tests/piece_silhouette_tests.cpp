// Picking outlines of the chess pieces (src/scene/piece_silhouette.h), from the real meshes of
// scene/pieces.cpp: slimmer than the base where the piece is, rays that pass beside a pawn's head
// reach the piece behind it (the cylinder the game used took them), and the 3 mm margin.
#include "test.h"
#include "chess/chess.h"
#include "game/layout.h"
#include "scene/piece_silhouette.h"
#include "scene/pieces.h"
#include <cmath>

using namespace m;

namespace {

const PieceSilhouette& outline(chess::PieceType t) {
    static PieceSilhouette s[7];
    if (s[t].empty()) s[t] = PieceSilhouette::fromMesh(buildPiece(t).body);
    return s[t];
}

// Widest radius of a slice (any sector).
float sliceRadius(const PieceSilhouette& s, float y) {
    const int sl = std::min(PieceSilhouette::kSlices - 1, int(y / s.height * float(PieceSilhouette::kSlices)));
    float r = 0.0f;
    for (int k = 0; k < PieceSilhouette::kSectors; ++k) r = std::max(r, s.radius[sl][k]);
    return r;
}

}  // namespace

TEST(piece_silhouette_follows_the_piece) {
    const PieceSilhouette& pawn = outline(chess::Pawn);
    CHECK(!pawn.empty());
    CHECK(std::fabs(pawn.height - layout::PIECE_HEIGHT[chess::Pawn]) < 0.002f);
    const float base = layout::PIECE_BASE_RADIUS[chess::Pawn];
    CHECK(std::fabs(sliceRadius(pawn, 0.001f) - base) < 0.0015f);
    // The head and its neck are far slimmer than the base.
    CHECK(sliceRadius(pawn, pawn.height * 0.85f) < base * 0.75f);
    CHECK(sliceRadius(pawn, pawn.height * 0.6f) < base * 0.75f);
    // A horizontal ray through the axis meets the surface; one beside the head misses it, unless
    // grown by the margin; the same offset at the base hits.
    const float yHead = pawn.height * 0.85f, rHead = sliceRadius(pawn, yHead);
    float t = pawn.intersect(vec3(-0.1f, yHead, 0.0f), vec3(1, 0, 0));
    CHECK(t > 0.0f && std::fabs(t - (0.1f - rHead)) < 0.001f);
    const float off = rHead + 0.002f;
    CHECK(pawn.intersect(vec3(-0.1f, yHead, off), vec3(1, 0, 0)) < 0.0f);
    CHECK(pawn.intersect(vec3(-0.1f, yHead, off), vec3(1, 0, 0), 0.003f) > 0.0f);
    CHECK(pawn.intersect(vec3(-0.1f, 0.003f, off), vec3(1, 0, 0)) > 0.0f);
    // Above the top, below the base, past maxT: nothing.
    CHECK(pawn.intersect(vec3(-0.1f, pawn.height + 0.001f, 0.0f), vec3(1, 0, 0)) < 0.0f);
    CHECK(pawn.intersect(vec3(-0.1f, -0.001f, 0.0f), vec3(1, 0, 0)) < 0.0f);
    CHECK(pawn.intersect(vec3(-0.1f, yHead, 0.0f), vec3(1, 0, 0), 0.0f, 0.05f) < 0.0f);
    // The knight's head is not round: its outline differs around the axis.
    const PieceSilhouette& knight = outline(chess::Knight);
    const int sl = int(0.75f * PieceSilhouette::kSlices);
    float lo = 1.0f, hi = 0.0f;
    for (int k = 0; k < PieceSilhouette::kSectors; ++k) {
        lo = std::min(lo, knight.radius[sl][k]);
        hi = std::max(hi, knight.radius[sl][k]);
    }
    CHECK(hi > lo * 1.3f);
}

TEST(piece_silhouette_reaches_the_piece_behind) {
    // A pawn on the square in front of a king, seen from a seated player's eyes (35 cm above the
    // board, 45 cm away). Rays aimed at the king just beside the pawn's head pass through the
    // cylinder the game used for the pawn (base radius x 1.12, full height), which took the
    // pointer; they miss the pawn's outline and reach the king.
    const PieceSilhouette& pawn = outline(chess::Pawn);
    const PieceSilhouette& king = outline(chess::King);
    const vec3 pawnAt(0.0f, 0.0f, layout::SQUARE_SIZE), kingAt(0.0f, 0.0f, 0.0f);
    const vec3 eye(0.0f, 0.35f, 0.45f);
    const float cylinder = layout::PIECE_BASE_RADIUS[chess::Pawn] * 1.12f;
    int stolen = 0, reached = 0;
    for (float x = -0.016f; x <= 0.016f; x += 0.001f)
        for (float y = 0.0f; y <= 0.09f; y += 0.002f) {
            Ray r;
            r.o = eye;
            r.d = normalize(vec3(x, y, 0.0f) - eye);
            const float tKing = king.intersect(r.o - kingAt, r.d);
            if (tKing < 0.0f) continue;   // not on the king
            const float tOld = rayCylinderY(r, pawnAt, cylinder, layout::PIECE_HEIGHT[chess::Pawn]);
            if (tOld < 0.0f || tOld > tKing) continue;   // the old pick was right there too
            ++stolen;
            const float tPawn = pawn.intersect(r.o - pawnAt, r.d);
            if (tPawn < 0.0f) ++reached;
        }
    CHECK(stolen > 10);
    CHECK(reached * 2 > stolen);   // most of what the cylinder took goes back to the king
    std::fprintf(stderr, "  rays on the king the old cylinder gave to the pawn: %d, now the king's: %d\n", stolen,
                 reached);
}
