// What the pointer touches on a chess piece: its outline, taken from its mesh (scene/pieces.h).
// A cylinder of the base's radius over the whole height is far wider than a pawn's head or a
// bishop's mitre: seen from a chair, it covered the lower part of the piece standing behind, which
// the pointer then could not reach. The outline follows the piece slice by slice instead.
#pragma once
#include "../math/math.h"
#include "../render/mesh.h"

// Object space of the piece meshes: meters, origin at the centre of the base, +Y up.
struct PieceSilhouette {
    static constexpr int kSlices = 64;    // along the height
    static constexpr int kSectors = 16;   // around +Y (the knight's head is not round)
    float height = 0.0f;                  // top of the piece; 0 = empty
    float maxRadius = 0.0f;
    float radius[kSlices][kSectors] = {}; // farthest surface from the axis per slice and sector

    // From a piece's body mesh (its triangles, conservatively: a triangle counts in every slice and
    // sector it spans, with its farthest corner).
    static PieceSilhouette fromMesh(const MeshData& mesh);
    bool empty() const { return height <= 0.0f; }
    // Distance along an object-space ray (origin o, unit direction d) to where it enters the outline
    // grown by 'margin' (meters), searched up to maxT; -1 when it misses.
    float intersect(m::vec3 o, m::vec3 d, float margin = 0.0f, float maxT = 1e30f) const;
};
