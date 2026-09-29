// Scoresheet layout (engine-free, compiled into the core library and unit-tested): the printed
// FIDE form of an A5 page, the move -> cell mapping, localized notation, the natural placement of
// handwritten glyphs, the pen path that writes them, the pad placement on the table and the
// geometry of a page flipped over the top edge. game/scoresheet.cpp (GL, fonts) and
// scene/scoresheet_model.cpp (meshes) are built on it.
//
// Page space: millimetres, origin at the top-left corner of the page as its owner reads it, +x to
// the right, +y downwards (towards the owner). The top edge is bound: every page is glued under
// the binding strip up to y = HINGE_Y and flips over that line.
// Pad space: metres, origin at the centre of the pad's footprint on the table top, +Y up, +X =
// page right, +Z = page down (towards the owner). PadFrame / padToWorld() place it in the world.
#pragma once
#include "../math/math.h"
#include "layout.h"
#include <cstdint>
#include <string>
#include <vector>

namespace game {
namespace sheet {

// ---- Dimensions -----------------------------------------------------------------------------
constexpr float PAGE_W = layout::SCORESHEET_WIDTH * 1000.0f;   // 148 mm (A5)
constexpr float PAGE_H = layout::SCORESHEET_LENGTH * 1000.0f;  // 210 mm
constexpr float HINGE_Y = 4.0f;          // mm: glued under the binding strip, pages flip over y = HINGE_Y
constexpr float FLIP_LENGTH = PAGE_H - HINGE_Y;  // mm of page that turns
constexpr int ROWS = layout::SCORESHEET_ROWS;    // rows per column
constexpr int MOVES_PER_PAGE = 2 * ROWS;         // two columns of moves per page
// Pad cross-section (mm, from the table top): back board, then the page stack; the top page lies
// on the stack with its upper face at PAD_TOP. The binding strip (cloth tape) wraps the top edge
// and covers the pages up to HINGE_Y with a lip of BINDING_T.
constexpr float PAD_TOP = layout::SCORESHEET_THICKNESS * 1000.0f;  // 5 mm
constexpr float BOARD_T = 1.6f;          // grey card back board
constexpr float SHEET_T = 0.1f;          // one sheet
constexpr float BINDING_T = 0.35f;       // cloth tape thickness
constexpr float PAD_YAW_DEG = 3.0f;      // the pad is turned so its top leans towards the board

// ---- Printed form -----------------------------------------------------------------------------
struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
    float cx() const { return 0.5f * (x0 + x1); }
    float cy() const { return 0.5f * (y0 + y1); }
    bool contains(float x, float y, float eps = 0.0f) const {
        return x >= x0 - eps && x <= x1 + eps && y >= y0 - eps && y <= y1 + eps;
    }
};

// Faces of the printed form (values of ui::font::Face).
enum PrintFace : int { PRINT_TEXT = 0, PRINT_ITALIC = 1, PRINT_TITLE = 2, PRINT_SYMBOL = 3 };

struct FormText {
    std::string text;   // UTF-8 (already translated)
    int face = PRINT_TEXT;
    float x = 0, baseline = 0;  // anchor (mm)
    float capHeight = 2.0f;     // mm
    int align = 0;              // 0 left, 1 centre, 2 right (of the anchor)
    float maxWidth = 0.0f;      // mm available; a longer translation is printed smaller (0 = no limit)
};
struct Form {
    std::vector<Rect> rules;    // filled rectangles (lines and box borders), printed ink
    std::vector<FormText> texts;
};
// The printed form of page 'page' (0-based): move numbers page*40+1 .. page*40+40. Labels come
// from i18n::tr("scoresheet.*") (English fallback).
Form printedForm(int page);

// ---- Handwritten fields ------------------------------------------------------------------------
// Note and Reference have no printed label: a player's own additions to the header (online
// games: "Online, 5+3 rated" beside Round and Board, the game number beside the result).
enum class Field { Event, Date, Round, Board, WhiteName, WhiteElo, BlackName, BlackElo, Page, Result, Note, Reference, Count };
const char* fieldName(Field f);   // "event", "date", ... (logs, tests)
// Header fields (Event .. BlackElo, Note, Reference) are written on the first page only; Page on
// every page; Result on the page that is on top when the game ends.
bool isHeaderField(Field f);

// Where a handwritten value goes: text starts after x0 (right-aligned before x1 for right-to-left
// text), on 'baseline', with capital letters about capHeight mm tall.
struct WriteBox {
    float x0 = 0, x1 = 0;
    float baseline = 0;
    float capHeight = 4.0f;
};
WriteBox fieldBox(Field f);

// ---- Moves --------------------------------------------------------------------------------------
// ply 0 = White's first move. Move number n (1-based) of page p = p*40 + block*20 + row + 1.
inline int moveNumber(int ply) { return ply / 2 + 1; }
inline int pageOfPly(int ply) { return (moveNumber(ply) - 1) / MOVES_PER_PAGE; }
struct Cell {
    int page = 0, block = 0, row = 0;  // block 0 = left column of moves (1-20), 1 = right (21-40)
    bool black = false;                // Black's cell of the row
};
Cell cellOf(int ply);
Rect cellRect(const Cell& c);          // the ruled cell (page mm)
Rect numberCellRect(int block, int row);
WriteBox moveBox(int ply);             // where the SAN of that ply is written

// Localized piece letters (FIDE practice: French R D T F C, German K D T L S...). Default English.
struct PieceLetters {
    std::string king = "K", queen = "Q", rook = "R", bishop = "B", knight = "N";
};
// Replaces the English piece letters of a SAN string (leading piece and promotion piece).
std::string localizeSan(const std::string& san, const PieceLetters& letters);

// ---- Handwriting placement ------------------------------------------------------------------------
// One shaped glyph (visual order), in em units relative to the start of the run (y down).
struct RunGlyph {
    uint32_t cp = 0;
    float penX = 0;                    // em, glyph origin from the run's left edge
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // em, ink box relative to the origin (y down); x1 <= x0 = blank
    int source = 0;                    // logical (writing) order
};
struct Run {
    std::vector<RunGlyph> glyphs;      // visual order
    float advance = 0;                 // em
    bool rtl = false;
    float xHeight = 0.45f, capHeight = 0.65f;  // em, of the writer's main face
};

// A placed glyph: its ink box in glyph-local mm (origin on the baseline at the pen position, y
// down) and the affine map glyph-local mm -> page mm: p = origin + ax * lx + ay * ly.
struct GlyphInk {
    uint32_t cp = 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // local mm (y0 < 0 above the baseline)
    float xHeight = 0, capHeight = 0;      // local mm above the baseline
    m::vec2 origin, ax{1, 0}, ay{0, 1};
    bool rtl = false;        // written right to left
    bool joinNext = false;   // the pen stays down into the next glyph (cursive scripts)
    bool wordEnd = false;    // a space follows in writing order
    int runIndex = 0;        // index in Run::glyphs
    m::vec2 toPage(float lx, float ly) const { return origin + ax * lx + ay * ly; }
};

// Natural handwriting in a box: slight baseline slope and drift, per-glyph jitter in size,
// rotation, position and slant, spacing variation, squeezed when too long, right-aligned for
// right-to-left runs. Deterministic for a seed. Returns the visible glyphs in WRITING order.
std::vector<GlyphInk> placeHandwriting(const Run& run, const WriteBox& box, uint32_t seed);
// Page-space bounding box of placed glyphs (ink boxes).
Rect inkBounds(const std::vector<GlyphInk>& glyphs);

// ---- Pen path ---------------------------------------------------------------------------------------
// The pen tip's path while writing: per glyph a quick oscillating sweep through its ink box in
// writing order (right to left for RTL), a separate tap for dots and accents, several strokes for
// CJK characters, pen lifts between glyphs (longer between words), pen down through cursive joins.
struct PathKey {
    float x = 0, y = 0;   // page mm
    float lift = 0;       // mm above the paper (0 = touching)
    float t = 0;          // s from the start
    bool down = false;    // the tip touches the paper from this key to the next
};
// Part of a glyph (glyph-local y range) inked by the pen-down path keys [k0, k1].
struct InkBand {
    int glyph = 0;
    float ly0 = 0, ly1 = 0;
    int k0 = 0, k1 = 0;
};
struct PenPath {
    std::vector<PathKey> keys;
    std::vector<InkBand> bands;
    float duration = 0;
};
// Speed at which ink appears around the passing tip (mm/s) and the farthest distance at which a
// stroke claims ink (mm): the reveal time of a point is min over the band's keys of
// t(key) + distance / REVEAL_SPEED (points farther than REVEAL_RADIUS: the band's end time).
constexpr float REVEAL_SPEED = 16.0f;
constexpr float REVEAL_RADIUS = 1.3f;
PenPath buildPenPath(const std::vector<GlyphInk>& glyphs, uint32_t seed);
// Appends 'b' after 'a' (pen lifted from a's end to b's start, 'gap' seconds in the air); times and
// key indices are shifted, band glyph indices are offset by glyphOffset (glyph lists concatenated).
void appendPath(PenPath& a, const PenPath& b, float gap, int glyphOffset);
// Reveal time of a page point for a band (same formula as the GPU): < 0 when the band never inks it.
float revealTime(const PenPath& path, const InkBand& band, float x, float y);
// Remaining time of the pen-down stroke running at time t, until the pen lifts (0 when the pen is
// up at t). Used to size the writing sound of a stroke.
float strokeDurationAt(const PenPath& path, float t);

// Script classes used by the path generator.
bool isCjk(uint32_t cp);
bool hasMarkAbove(uint32_t cp);   // i, j, accented Latin, Cyrillic й ё ї...

// ---- Writing sound ------------------------------------------------------------------------------
// The sound of one pen-down stroke (audio::playPenStroke), for the animator's PenDown event.
//   path:     the entry being written (nullptr: none, the touch-down tick alone).
//   downTime: path time at which the tip touched the paper (< 0: unknown, the tick alone). Events
//             come out at the end of an animator update, 'late' seconds after the instant itself
//             (downTime = Animator::writingPathTime() - late): the friction plays what is left of
//             the stroke. downTime may fall a rounding error short of its pen-down key.
//   The writer's own pen: a listener in the writer's head (the first-person player, within
//   FIRST_PERSON_RADIUS of his eyes) hears the stroke as a writer does, head bent over his sheet:
//   from WRITER_EAR_DISTANCE, in the direction of the tip (near field: a louder, slightly wider
//   direct sound; the hall's diffuse reverb is unchanged). His camera keeps watching the board and
//   the pad lies out of view beside him: this is how he knows that he writes. Any other listener
//   hears the pen at the tip (the opponent's pen, an observer).
constexpr float PEN_STROKE_GAIN = 0.9f;
constexpr float WRITER_EAR_DISTANCE = 0.35f;   // m, ear to pen tip while writing
constexpr float FIRST_PERSON_RADIUS = 0.3f;    // m, listener to the writer's eyes
struct PenStrokeSound {
    m::vec3 position;         // where to play it
    float seconds = 0.0f;     // friction window (0: the touch-down tick alone)
    float gain = PEN_STROKE_GAIN;
    bool writersOwn = false;  // heard by the writer himself
};
PenStrokeSound penStrokeSound(const PenPath* path, float downTime, float late, m::vec3 tip, m::vec3 writerEyes,
                              m::vec3 listener);

// ---- Pad placement ------------------------------------------------------------------------------------
// owner: 0 = White (seat at +Z), 1 = Black. The pad lies on the side without the clock, in front of
// its owner, turned by PAD_YAW_DEG so its top leans towards the board (the natural tilt for the
// hand that writes on that side).
struct PadFrame {
    m::vec3 center;              // world, on the table top
    m::vec3 right, up, down;     // world directions of pad +X, +Y, +Z
    float outerSign = 1.0f;      // pad X sign of the outer long edge (away from the board)
    int owner = 0;
    bool clockOnPositiveX = true;
    m::mat4 toWorld() const { return m::mat4(m::vec4(right, 0), m::vec4(up, 0), m::vec4(down, 0), m::vec4(center, 1)); }
    m::vec3 padToWorld(m::vec3 p) const { return center + right * p.x + up * p.y + down * p.z; }
};
PadFrame padFrame(int owner, bool clockOnPositiveX);
// Pad-local point (m) of a page point (mm) lying flat at 'heightMm' above the table.
inline m::vec3 pageToPad(float xMm, float yMm, float heightMm) {
    return m::vec3((xMm - 0.5f * PAGE_W) * 0.001f, heightMm * 0.001f, (yMm - 0.5f * PAGE_H) * 0.001f);
}
// Page x (mm) of the outer long edge: the page corner the writing hand pinches to turn a page is
// (outerEdgeX, PAGE_H), the bottom corner on the outer side.
inline float outerEdgeX(const PadFrame& f) { return f.outerSign > 0.0f ? PAGE_W : 0.0f; }
// Pen lying on the table beside the outer edge (pen frame: tip at the origin, +Y towards the back
// end), clip up, tip towards the top of the pad.
m::mat4 penRestTransform(const PadFrame& f);
// A natural pen orientation while writing with the tip at 'tipWorld' (for viewers; the animator
// holds the pen in the real game).
m::mat4 penWritingTransform(const PadFrame& f, m::vec3 tipWorld);

// ---- Page flip ------------------------------------------------------------------------------------------
// A page turning over the top edge. s in [0,1]: 0 = lying on the pad, 1 = lying face down beyond the
// top edge (over the binding strip and down onto the table, on top of 'turnedBelow' pages already
// turned). The page bends along lines parallel to the hinge (arc length along the page is kept
// exactly); the pinched outer corner leads, so the page also twists a little. Past s ~ 0.55 the page
// is beyond vertical and falls over by itself.
struct FlipParams {
    int turnedBelow = 0;
    float outerSign = 1.0f;      // pinched side (PadFrame::outerSign)
};
// Pad-local position (m) of the page point (xMm, yMm) at progress s (the strip above HINGE_Y stays
// glued flat under the tape).
m::vec3 flipPoint(float xMm, float yMm, float s, const FlipParams& p);
// Same for a grid: out[j * xs.size() + i] = flipPoint(xs[i], ys[j]); ys must be increasing.
void flipGrid(const std::vector<float>& xs, const std::vector<float>& ys, float s, const FlipParams& p,
              std::vector<m::vec3>& out);

}  // namespace sheet
}  // namespace game
