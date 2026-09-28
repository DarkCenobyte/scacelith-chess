// One player's scoresheet: an A5 FIDE scoresheet pad lying on the table in front of its owner,
// bound along its top edge (the edge towards the board), and the ballpoint pen beside it.
//
// The owner writes both players' moves on his own sheet, in his own hand (Config::handStyle):
// every value is shaped with ui::text::shapeLine + ui::font::handwritingFace, placed like natural
// handwriting (game/scoresheet_layout.h) and revealed where the pen tip passes.
//
// Writing an entry (a move, the header, the result, any field), with anim::Animator:
//     std::vector<anim::PenKey> path = sheet.beginMove(ply, san);   // world-space pen path
//     writeTask.path = path; animator.enqueueWriting(writeTask);
//     every frame: float t = animator.writingPathTime(); if (t >= 0) sheet.setWritingTime(t);
//     on the animator's WritingDone event: sheet.finishEntry();
// Entries are queued in the order they were begun: setWritingTime() drives the oldest unfinished
// one, finishEntry() completes it (its ink becomes part of the page).
//
// Turning a page (before the first move of page 2, 3...):
//     if (sheet.pageTurnNeeded(ply)) {
//         sheet.beginPageTurn();
//         turnTask.pageCorner = [&sheet](float s) { return sheet.pageCorner(s); };  // then enqueue
//     }
//     every frame: float s = animator.pageTurnProgress(); if (s >= 0) sheet.setTurnProgress(s);
//     on the PageTurned event: sheet.finishPageTurn();
// then beginMove() for that ply as usual (the move goes on the new page).
//
// Sounds (the scoresheet plays none; the integrator calls, on the animator's writing events):
//     PenDown:     audio::playPenStroke(event.position, sheet.strokeDurationAt(animator.writingPathTime()));
//     PageGripped: audio::play(audio::Sfx::PageTurn, sheet.pageCorner(0.0f));
//     PageTurned:  audio::play(audio::Sfx::PageFlap, sheet.pageCorner(1.0f));
//
// GL: init(), update() and the submit functions need the GL context; ui::font must be ready
// (ui::font::init()) before init(). Call update() once per frame before submit().
#pragma once
#include "../anim/animator.h"
#include "../render/renderer.h"
#include "scoresheet_layout.h"
#include <memory>
#include <string>
#include <vector>

namespace game {

class Scoresheet {
public:
    struct Config {
        int owner = 0;                    // 0 = White's pad (+Z), 1 = Black's (-Z)
        bool clockOnPositiveX = true;     // the pad lies on the other side
        int handStyle = 0;                // ui::font::HandStyle of the owner
        uint32_t seed = 1;                // handwriting randomness (deterministic per entry)
        sheet::PieceLetters letters;      // localized piece letters (default K Q R B N)
        m::vec3 inkColor{0.012f, 0.023f, 0.18f};  // linear, as the ink looks on the paper (blue ballpoint)
    };
    // Handwritten header values (empty values stay blank).
    struct Header {
        std::string event = "Scacelith";
        std::string date;                 // e.g. "28.09.2026"
        std::string round = "1";
        std::string board = "1";
        std::string white, whiteElo, black, blackElo;
        std::string note, reference;      // unlabeled additions (sheet::Field::Note / Reference), "" = none
    };

    Scoresheet();
    ~Scoresheet();
    Scoresheet(const Scoresheet&) = delete;
    Scoresheet& operator=(const Scoresheet&) = delete;

    bool init(const Config& c);           // GL context; ui::font ready
    void shutdown();
    // Blank pad (page 1 on top, every page turned back, nothing queued). Keeps the config.
    void reset();
    void setClockSide(bool clockOnPositiveX);
    // Hand style / piece letters / ink for the entries begun from now on.
    void setHandStyle(int style);
    void setLetters(const sheet::PieceLetters& letters);
    void setInkColor(m::vec3 linear);
    const Config& config() const { return cfg_; }

    // ---- Writing (paths in world space: pen tip, down = touching the paper) ----
    // Every header field (page 1) and the page number.
    std::vector<anim::PenKey> beginHeader(const Header& h);
    // Move 'ply' (0 = White's first move), SAN in English letters (localized with Config::letters).
    // The first move of a new page also writes that page's number.
    std::vector<anim::PenKey> beginMove(int ply, const std::string& san);
    // "1-0", "0-1", "½-½" in the result box of the page on top.
    std::vector<anim::PenKey> beginResult(const std::string& result);
    std::vector<anim::PenKey> beginField(sheet::Field f, const std::string& text);
    // Time along the oldest unfinished entry's path (animator.writingPathTime()); t < 0 is ignored.
    void setWritingTime(float t);
    void finishEntry();                   // completes the oldest unfinished entry
    int pendingEntries() const;
    // Remaining pen-down time of the stroke running at path time t of the entry being written
    // (0 when the pen is up): the length to give the writing sound on PenDown.
    float strokeDurationAt(float t) const;
    // Instantly written values (a game loaded or resumed): no path, the ink is there at once.
    void writeHeaderInstant(const Header& h);
    void writeMoveInstant(int ply, const std::string& san);
    void writeResultInstant(const std::string& result);
    void writeFieldInstant(sheet::Field f, const std::string& text);

    // ---- Pages ----
    // True when 'ply' goes on a page after the one that will be on top once the turns already
    // begun are done: call beginPageTurn() (and animate it) before beginMove(ply).
    bool pageTurnNeeded(int ply) const;
    void beginPageTurn();
    // Flip progress of the page being turned (animator.pageTurnProgress()): 0 = lying on the pad,
    // 1 = lying face down beyond the top edge. The page is self-supporting (falls over by itself)
    // past s ~ 0.55.
    void setTurnProgress(float s);
    // World position of the corner the hand pinches, for flip progress s, of the page being turned
    // (or of the next page to turn when none is): the bottom corner of the page (the edge nearest
    // the owner) on the outer side, away from the board.
    m::vec3 pageCorner(float s) const;
    void finishPageTurn();
    int currentPage() const { return page_; }       // page on top (0-based)
    int turnsPending() const { return pendingTurns_; }

    // ---- Placement ----
    const sheet::PadFrame& frame() const { return frame_; }
    m::mat4 padTransform() const { return frame_.toWorld(); }
    m::mat4 penRestTransform() const;     // pen lying on the table beside the pad's outer edge
    // World point on the paper in the outer margin next to the row of the next move (for
    // anim::Animator::setWritingRest); 'nextPly' = the next ply to be written.
    m::vec3 writingRest(int nextPly) const;

    // ---- Frame ----
    void update();                        // GL: renders the page / entry textures that changed
    // Pad and pages (objectIdBase .. objectIdBase + 15).
    void submit(render::Renderer& r, uint32_t objectIdBase) const;
    // The ballpoint pen at any transform (pen frame: tip at the origin, +Y to the back end).
    // Shared geometry (built on first use, GL context). prevPenToWorld: previous frame's
    // transform for motion vectors (nullptr = same).
    static void submitPen(render::Renderer& r, const m::mat4& penToWorld, uint32_t objectId,
                          const m::mat4* prevPenToWorld = nullptr);
    // Frees the shared pen geometry when no scoresheet is alive (called by shutdown()).
    static void releaseShared();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Config cfg_;
    sheet::PadFrame frame_;
    int page_ = 0;
    int pendingTurns_ = 0;
};

}  // namespace game
