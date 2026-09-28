// World layout: every module places things relative to these constants (meters, +Y up).
//
//   * Board centre on the XZ origin. Files a..h run along +X (a at -X), ranks 1..8 along -Z
//     (rank 1 at +Z). White sits at +Z looking towards -Z; Black sits at -Z looking towards +Z.
//   * Floor at y = 0. Table top at TABLE_TOP_Y, board playing surface at BOARD_TOP_Y.
//   * The hall's three tall windows are in the -X wall (White's left, Black's right).
//   * The chess clock stands on the table at the human player's right-hand side
//     (x = +CLOCK_OFFSET_X when the human plays White, -CLOCK_OFFSET_X when Black).
#pragma once
#include "../math/math.h"

namespace layout {

// ---- Table & board -------------------------------------------------------------------------
constexpr float TABLE_TOP_Y = 0.760f;        // standard table height
constexpr float TABLE_WIDTH = 1.20f;         // along X
constexpr float TABLE_DEPTH = 0.86f;         // along Z (between the players)
constexpr float TABLE_TOP_THICKNESS = 0.045f;
constexpr float BOARD_THICKNESS = 0.022f;    // marble slab on the table
constexpr float BOARD_TOP_Y = TABLE_TOP_Y + BOARD_THICKNESS;
constexpr float SQUARE_SIZE = 0.055f;        // FIDE: 5-6.5 cm
constexpr float BOARD_PLAY_SIZE = SQUARE_SIZE * 8.0f;  // 0.44 m
constexpr float BOARD_BORDER = 0.030f;       // frame around the squares
constexpr float BOARD_SIZE = BOARD_PLAY_SIZE + 2.0f * BOARD_BORDER;

// Centre of a square on the playing surface. file, rank in 0..7 (a1 = 0,0).
inline m::vec3 squareCenter(int file, int rank) {
    return {(float(file) - 3.5f) * SQUARE_SIZE, BOARD_TOP_Y, (3.5f - float(rank)) * SQUARE_SIZE};
}
// Square index 0..63 (a1 = 0, b1 = 1, ..., h8 = 63).
inline m::vec3 squareCenter(int sq) { return squareCenter(sq & 7, sq >> 3); }

// ---- Pieces (Staunton, tournament size "3.75 inch king") -----------------------------------
// Heights / base radii (m), indexed by chess::PieceType (1 Pawn .. 6 King; 0 unused).
constexpr float PIECE_HEIGHT[7] = {0.0f, 0.050f, 0.060f, 0.070f, 0.055f, 0.083f, 0.095f};
constexpr float PIECE_BASE_RADIUS[7] = {0.0f, 0.0145f, 0.0175f, 0.0175f, 0.0175f, 0.019f, 0.020f};
// Where fingers pinch a piece when lifting it (fraction of height) and the radius there.
constexpr float PIECE_GRIP_HEIGHT[7] = {0.0f, 0.62f, 0.60f, 0.62f, 0.72f, 0.62f, 0.60f};
constexpr float PIECE_GRIP_RADIUS[7] = {0.0f, 0.0075f, 0.010f, 0.0085f, 0.013f, 0.0095f, 0.010f};
constexpr float PIECE_LIFT_HEIGHT = 0.035f;  // clearance used when carrying pieces

// Captured pieces are lined up on the table beside the board, on the CLOCK side (the playing
// hand's side; the other side holds the scoresheets), in the capturing player's half: two rows
// along Z, from |z| = CAPTURE_Z0 (clear of the clock) towards the capturer.
constexpr float CAPTURE_ROW_X = 0.30f;       // |x| of the first row centre (clock side)
constexpr float CAPTURE_SPACING = 0.045f;    // between the two rows (along X)
constexpr float CAPTURE_Z0 = 0.12f;          // |z| of the first piece of a row
constexpr float CAPTURE_COL_SPACING = 0.024f;  // between pieces of a row (along Z)
// Spare queens for promotions stand beyond the clock, near their owner.
constexpr float RESERVE_X = 0.505f;
constexpr float RESERVE_Z = 0.15f;

// ---- Clock -----------------------------------------------------------------------------------
constexpr float CLOCK_OFFSET_X = 0.405f;     // |x| of the clock centre
constexpr float CLOCK_Z = 0.0f;              // centred between the players
constexpr float CLOCK_WIDTH = 0.19f;         // along Z (faces the side), displays face each player
constexpr float CLOCK_DEPTH = 0.085f;        // along X
constexpr float CLOCK_HEIGHT = 0.065f;

// ---- Scoresheets ----------------------------------------------------------------------------
// Each player keeps a scoresheet pad (FIDE art. 8.1) on the side of the table WITHOUT the clock,
// in front of its owner: x = -sign(clock x) * SCORESHEET_X, z = +-SCORESHEET_Z (White +Z). The
// hand on that side writes; the hand on the clock side plays and presses the clock (art. 6.2.5:
// the clock is pressed with the hand that moved). The pad is A5, portrait, bound along its top
// edge (the edge towards the board): a full page is flipped over the top and lies beyond it, face
// down, so the strip between the pad and the table centre (|z| < SCORESHEET_Z - LENGTH/2) on that
// side stays free of pieces.
constexpr float SCORESHEET_WIDTH = 0.148f;     // along X (A5)
constexpr float SCORESHEET_LENGTH = 0.210f;    // along Z
constexpr float SCORESHEET_THICKNESS = 0.005f; // back board + page stack
constexpr float SCORESHEET_X = 0.45f;          // |x| of the pad centre
constexpr float SCORESHEET_Z = 0.318f;         // |z| of the pad centre
constexpr int SCORESHEET_ROWS = 20;            // move rows per column, 2 columns per page (40 moves)
// Ballpoint pen (cap posted on the back while writing).
constexpr float PEN_LENGTH = 0.142f;
constexpr float PEN_RADIUS = 0.0045f;

// ---- Players (seated robots) ----------------------------------------------------------------
constexpr float SEAT_HEIGHT = 0.46f;
constexpr float PLAYER_PELVIS_Z = 0.60f;     // |z| of each player's pelvis (hip joint centre)
constexpr float PLAYER_PELVIS_Y = SEAT_HEIGHT + 0.10f;
constexpr float CHAIR_Z = 0.66f;             // |z| of the chair seat centre
// Approximate eye height when sitting upright (the camera follows the head bone).
constexpr float EYE_HEIGHT = 1.23f;

// ---- Hall ------------------------------------------------------------------------------------
constexpr float HALL_MIN_X = -7.0f, HALL_MAX_X = 7.0f;   // window wall at HALL_MIN_X
constexpr float HALL_MIN_Z = -11.0f, HALL_MAX_Z = 11.0f;
constexpr float HALL_HEIGHT = 9.0f;
constexpr int WINDOW_COUNT = 3;
constexpr float WINDOW_WIDTH = 2.6f;
constexpr float WINDOW_SILL_Y = 0.9f;
constexpr float WINDOW_TOP_Y = 7.4f;         // springing line of the round arch = top - width/2
constexpr float WINDOW_SPACING = 5.2f;       // centre-to-centre along Z, middle window at z = 0
constexpr float WALL_THICKNESS = 0.9f;       // deep window reveals

}  // namespace layout
