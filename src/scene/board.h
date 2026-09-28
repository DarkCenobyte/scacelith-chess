// Marble chess board (world space, see game/layout.h): 64 inlaid marble slabs with tiny chamfers
// and hairline gaps, framed by a marble border with a moulded, bevelled outer edge. The board
// rests on the table top (y = TABLE_TOP_Y) and its playing surface is exactly flat at
// y = BOARD_TOP_Y (top faces of the squares and of the frame), so it can be a planar reflector.
//
// Parts:
//   "squares_light"  MaterialId::BoardSquareLight   (a1 is dark: light when file + rank is odd)
//   "squares_dark"   MaterialId::BoardSquareDark
//   "frame"          MaterialId::BoardFrame          (border + the base slab seen in the gaps)
//
// Square uv (both square parts): u = (x + BOARD_PLAY_SIZE/2) / BOARD_PLAY_SIZE,
// v = (BOARD_PLAY_SIZE/2 - z) / BOARD_PLAY_SIZE. Square (file, rank) therefore spans
// [file/8, (file+1)/8] x [rank/8, (rank+1)/8] (a1 at the origin, u along the files towards h,
// v along the ranks towards 8) and floor(uv * 8) = (file, rank) everywhere on it: the chamfers
// and sides are clamped just inside the square's range. Tangent = +X (u), bitangent = -Z (v).
// Frame uv: u = distance along the border outline (m), v = distance along the moulding profile.
#pragma once
#include "model.h"

Model buildBoard();
