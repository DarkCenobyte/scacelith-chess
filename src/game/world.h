// The 3D world of the game: hall, table, chairs, board, chess set, clock and the two robots.
// Builds every procedural model (incrementally, so a loading screen can be shown), uploads it,
// and submits draw items each frame. Game logic lives in game_scene.cpp.
#pragma once
#include "../character/skeleton.h"
#include "../chess/chess.h"
#include "../render/renderer.h"
#include "../scene/model.h"
#include "physical_board.h"
#include <memory>
#include <string>
#include <vector>

namespace game {

// What the clock shows (see scene/clock_model.h for the display encoding).
struct ClockDisplay {
    int64_t ms[2] = {0, 0};       // per clock half (index 0 = local -Z half)
    int running = -1;             // half whose time runs, -1 = none
    bool flagged[2] = {false, false};
    bool unlimited = false;
    bool dashes = false;          // "--:--" on both halves: a clock nobody presses (an untimed game)
    bool paused = false;
    float leverSide = 0.0f;       // -1..+1: +1 = half 1 pressed down, -1 = half 0 pressed down
};

struct Marker {
    chess::Square square;
    int kind;                     // see shaders/materials/game_marker.glsl
    float strength = 1.0f;
};

// Coach mode: a piece the coach designates (World::submitPieces): a soft cobalt light hugging its
// silhouette, breathing slowly (DrawItem::highlight).
struct PieceHighlight {
    int pieceId = -1;             // PieceObject::id
    float strength = 1.0f;        // [0,1]; animate it for the fade in (~0.25 s) and out (~0.4 s)
};

// Coach mode: a mark on the board (World::submitCoachMarks, shaders/materials/coach_marker.glsl).
// Square: a rounded cobalt outline with a soft halo around 'sq', announced by a ring closing onto
// it, then breathing. Arrow: from 'from' to 'to' (through the centre of 'via' when set: the corner
// of a knight's L), drawn from the start to the tip in 0.45 s, then light flows along it. The
// arrow starts at the edge of a piece standing on 'from' and its tip stops short of the centre of
// 'to', so both pieces stay clear.
struct CoachMark {
    enum Kind { Square, Arrow } kind = Square;
    chess::Square sq = chess::NoSquare;                                                // Square
    chess::Square from = chess::NoSquare, via = chess::NoSquare, to = chess::NoSquare;  // Arrow
    float strength = 1.0f;        // [0,1]; animate it for the fade in and out
    float age = 0.0f;             // seconds since the mark appeared (drives its arrival and pulse)
};

class World {
public:
    World();
    ~World();

    // Incremental loading. Returns true when everything is ready.
    bool loadStep();
    float loadProgress() const;
    const char* loadLabel() const;
    bool loaded() const;

    // Must be called once after loading (registers planar reflectors on the renderer).
    void setupRenderer(render::Renderer& r);

    // The chess clock stands at the human player's right (layout.h).
    void setClockSide(bool positiveX);
    bool clockOnPositiveX() const { return clockPosX_; }
    m::mat4 clockTransform() const;
    m::vec3 clockPressPoint(int half) const;   // world, where a fingertip presses the lever
    bool rayHitsClock(const m::Ray& ray, float* t = nullptr) const;
    // Clock half used by the player sitting on the given side (+1 = White's seat at +Z).
    int clockHalfForSeat(float seatZSign) const;

    render::Environment environment(float time) const;

    void submitStatic(render::Renderer& r);
    void submitPieces(render::Renderer& r, const PhysicalBoard& board);
    void submitClock(render::Renderer& r, const ClockDisplay& d);
    // seat: 0 = White's chair (+Z), 1 = Black's chair (-Z). armSeeThrough in [0,1] fades the arm
    // on armSide to a see-through ghost in the main view (0 = opaque; its shadow stays).
    void submitRobot(render::Renderer& r, int seat, const m::mat4* globals, const m::mat4* prevGlobals, bool firstPerson,
                     float armSeeThrough = 0.0f, character::Side armSide = character::Side::Right);
    void submitMarkers(render::Renderer& r, const std::vector<Marker>& markers);

    // ---- Coach mode -------------------------------------------------------------------------------
    // The robot in this seat wears the "COACH" marking on its chest (-1 = none, the default).
    void setCoachSeat(int seat);
    // Board coordinates (Settings::showCoordinates; off by default, like tournament boards):
    // files a-h (Cinzel's small capitals) and ranks 1-8 inlaid in pale gold stone in the board's
    // marble border, under its polish; each player's files along his edge and his ranks on his
    // left, upright from his chair. Changing it needs no reload (the frame's material is swapped).
    void setBoardCoordinates(bool on);
    // submitPieces with the pieces the coach designates (nullptr or empty = none).
    void submitPieces(render::Renderer& r, const PhysicalBoard& board, const std::vector<PieceHighlight>* highlights);
    // Squares and arrows the coach shows, after the pieces (any number; strength 0 skips a mark).
    void submitCoachMarks(render::Renderer& r, const std::vector<CoachMark>& marks);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool clockPosX_ = true;
    float prevLeverAngle_ = 0.0f;
    bool hasPrevLever_ = false;
};

}  // namespace game
