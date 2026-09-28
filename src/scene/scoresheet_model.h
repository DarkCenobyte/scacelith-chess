// Scoresheet pad and ballpoint pen geometry (scoresheet package).
//
// Pad space (see game/scoresheet_layout.h): metres, origin at the centre of the pad's footprint
// on the table top, +Y up, +X = page right, +Z = page down (towards the owner); the bound top edge
// is at -Z. Cross-section: grey card back board (0 .. BOARD_T), page stack up to PAD_TOP - SHEET_T
// (its top face is the next page), the top page at PAD_TOP, a cloth tape wrapping the top edge
// and holding the pages down to HINGE_Y.
//
// Paper surfaces use the uv conventions of shaders/materials/paper.glsl (front [0,1], back
// [2,3], stack edge >= 4); game::Scoresheet draws the ScoresheetPaper parts with its own material
// copy and sets their page layer.
#pragma once
#include "model.h"
#include "../game/scoresheet_layout.h"
#include <vector>

// Static parts of the pad. Part names: "board", "tape", "stack_top" (the page under the top page,
// front uv), "stack_edges" (edge uv).
Model buildScoresheetPad();

// A page surface from a grid of mid-surface points (pad space, m): grid[j * xs.size() + i] is
// page point (xs[i], ys[j]) (mm). Front and back faces 0.08 mm apart (paper), smooth normals.
void buildPageMesh(const std::vector<float>& xs, const std::vector<float>& ys, const std::vector<m::vec3>& grid,
                   MeshData& out);
// Page sample positions for a deformable page: every column of the grid (xs) and denser rows (ys)
// near the hinge where the page wraps around the binding.
void pageGridSamples(std::vector<float>& xs, std::vector<float>& ys);

// Ballpoint pen in the pen frame: tip (ball) at the origin, +Y along the barrel to the back end
// (layout::PEN_LENGTH), cap posted on the back end, clip on the +Z side. Parts: PenBody (barrel,
// cap), PenMetal (tip, cone, rings, clip, finial).
Model buildBallpointPen();
