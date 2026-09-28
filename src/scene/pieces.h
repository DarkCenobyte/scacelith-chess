// Procedural tournament Staunton chess set (dimensions from game/layout.h).
//
// Object space of every piece: meters, origin at the centre of the base on the playing surface
// (the felt touches y = 0), +Y up. The knight's head faces +Z and the bishop's mitre slit is
// cut on its +Z side; the game rotates each piece by colour (White pieces 180 degrees about Y so
// they face Black).
//
// Construction: turned parts are lathe profiles (smooth normals, small fillets on every edge);
// non-rotational details (knight head, rook crenellations, bishop mitre slit, queen coronet,
// king cross) are signed distance fields polygonised at startup with surface nets
// (scene/sdf_mesher.h) and merged with the lathe at a natural joint (collar / turret base).
//
// UVs: u = angle around +Y / 2pi (lathe convention, seam at +X), v = y / piece height.
// Tangents run around the axis. Materials should prefer positionOS for marble veins.
#pragma once
#include "../render/mesh.h"

struct PieceMeshes {
    MeshData body;  // MaterialId::MarbleWhitePiece / MarbleBlackPiece
    MeshData felt;  // MaterialId::PieceFelt: thin baize disc slightly recessed under the base
};

// pieceType: 1 Pawn, 2 Knight, 3 Bishop, 4 Rook, 5 Queen, 6 King (chess::PieceType values).
PieceMeshes buildPiece(int pieceType);
