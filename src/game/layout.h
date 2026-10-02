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
#include <cmath>

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

// Widest horizontal extent of each piece around its base centre (m), whichever way it faces: the
// base for most, the knight's nose (18.7 mm forward of the axis, 36 mm up) for the knight.
constexpr float PIECE_FOOTPRINT_RADIUS[7] = {0.0f, 0.0145f, 0.0187f, 0.0175f, 0.0175f, 0.019f, 0.020f};

// Spare queens for promotions stand beyond the clock, near their owner.
constexpr float RESERVE_X = 0.505f;
constexpr float RESERVE_Z = 0.15f;

// Captured pieces stand on the table beside the board, on the CLOCK side (the playing hand's side;
// the other side holds the scoresheets), in the half of the player who captured them; a pawn
// leaving the board by promotion is set down in its owner's half. The slots of a half form a
// staggered grid: rows along Z, CAPTURE_ROW_STEP apart along X from the board outwards, every
// other row shifted by half a pitch, so that any two neighbours stand CAPTURE_PITCH apart. That
// clears the widest footprints (two queens: 38 mm) by 7 mm whichever way the pieces face. Slots
// run from |z| = CAPTURE_Z0 (clear of the clock case) to CAPTURE_Z_MAX (in front of the player),
// except around the spare queen and where the playing hand rests (below).
//
// Filling order: the hand sets a captured piece down from the pocket of its ring and little
// fingers, with the rest of the hand on the board side of the piece. The slots therefore fill from
// the table's edge inwards, row by row, each row from the clock towards the player: the hand always
// comes down beside slots that are still free. Two kinds of slots come last, once all the others
// are taken (more pieces than a game normally takes off the board), as the hand may brush a
// neighbour there: those with the spare queen on their board side, and those nearest the player
// (beyond CAPTURE_Z_LATE, where the hand turns and its fingers reach back over the row).
constexpr float CAPTURE_PITCH = 0.045f;      // centre distance between two neighbouring slots
constexpr float CAPTURE_ROW_STEP = 0.039f;   // PITCH * sqrt(3) / 2, rounded up: the stagger
constexpr float CAPTURE_X0 = 0.282f;         // |x| of the row beside the board
constexpr float CAPTURE_Z0 = 0.120f;         // |z| of the first slot of the unshifted rows
constexpr float CAPTURE_Z_LATE = 0.280f;     // |z| beyond which slots come last
constexpr float CAPTURE_Z_MAX = 0.300f;      // |z| limit of the slot centres
constexpr int CAPTURE_ROWS = 8;              // the last one 30 mm from the table's moulded edge
constexpr int CAPTURE_MAX_SLOTS = CAPTURE_ROWS * 5;
// The playing hand rests on the table beside the board, in front of its player (its rest spot, the
// hand's target, at |x| REST_HAND_X, |z| REST_HAND_Z): palm and fingers cover about this area, and
// no slot comes within a queen's footprint and a finger's thickness of it.
constexpr float REST_HAND_X = 0.24f, REST_HAND_Z = 0.34f;
constexpr float REST_HAND_MIN_X = 0.26f, REST_HAND_MAX_X = 0.36f, REST_HAND_MIN_Z = 0.20f;
constexpr float REST_HAND_CLEARANCE = 0.026f;

namespace detail {
struct CaptureSlots {
    m::vec2 at[CAPTURE_MAX_SLOTS];
    int count = 0, early = 0;
    CaptureSlots() {
        for (int late = 0; late < 2; ++late) {
            if (late) early = count;
            for (int row = CAPTURE_ROWS - 1; row >= 0; --row) {
                float x = CAPTURE_X0 + float(row) * CAPTURE_ROW_STEP;
                float z0 = CAPTURE_Z0 + ((row & 1) ? 0.5f * CAPTURE_PITCH : 0.0f);
                for (int k = 0;; ++k) {
                    float z = z0 + float(k) * CAPTURE_PITCH;
                    if (z > CAPTURE_Z_MAX + 1e-4f) break;
                    if (m::length(m::vec2(x - RESERVE_X, z - RESERVE_Z)) < CAPTURE_PITCH) continue;
                    if (x > REST_HAND_MIN_X - REST_HAND_CLEARANCE && x < REST_HAND_MAX_X + REST_HAND_CLEARANCE &&
                        z > REST_HAND_MIN_Z - REST_HAND_CLEARANCE)
                        continue;
                    bool queenOnBoardSide = x > RESERVE_X && std::fabs(z - RESERVE_Z) < CAPTURE_PITCH;
                    if ((queenOnBoardSide || z > CAPTURE_Z_LATE) != (late == 1)) continue;
                    at[count++] = m::vec2(x, z);
                }
            }
        }
    }
};
inline const CaptureSlots& captureSlots() {
    static const CaptureSlots slots;
    return slots;
}
}  // namespace detail

// Number of capture slots in each half, and of those before the late ones.
inline int captureSlotCount() { return detail::captureSlots().count; }
inline int captureSlotEarlyCount() { return detail::captureSlots().early; }
// Capture slot k (0 .. captureSlotCount() - 1, in filling order) as (|x|, |z|). The caller mirrors
// x to the clock side and z to the half of the player the pieces stand beside (White sits at +Z).
// Beyond the last slot, the last one.
inline m::vec2 captureSlot(int k) {
    const detail::CaptureSlots& s = detail::captureSlots();
    return s.at[k < 0 ? 0 : (k < s.count ? k : s.count - 1)];
}

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
