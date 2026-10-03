// The director performs coach scripts (coach/script.h) on the Stage: it renders each line twice
// (written for the subtitle, spoken for the voice, same variant), has it synthesised ahead of time,
// plays it, times the gestures and marks on the words they refer to (coach/pacing.h), runs the
// demonstrations and rewinds, and applies the turn-taking rules:
//   - Urgent beats go before everything queued and cut a running Normal/Low line at its next
//     pause at a punctuation mark (comma, colon, dash or sentence end; a pause of the synthesised
//     audio);
//   - Normal and Low beats queue behind beats of the same or a higher priority;
//   - Low beats are dropped when stale (their ply is more than two plies old) or once the player
//     acts (playerActed);
//   - Space skips the running skippable beat and the skippable beats of the same script after it
//     (skipCurrent(): the running beat only, and of a demonstration move only its narration); a
//     Rewind is never skipped, it only goes faster.
// Engine-free and GL-free: the world is reached through coach::Stage only.
#pragma once
#include "script.h"
#include "stage.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace coach {

struct DirectorConfig {
    std::string uiLanguage = "en";   // i18n::language(): the subtitles' language
    int subtitles = 0;               // SubtitleMode of game/settings.h: 0 Automatic, 1 On, 2 Off
    float speed = 1.0f;              // TTS speed (the rules lesson: kLessonSpeechSpeed)
};

// A mark as it is drawn this frame: World::submitCoachMarks for Square/Arrow, a PieceHighlight
// (plus a square outline) for Piece. strength fades in and out; age counts from its appearance.
struct ShownMark {
    Mark::Kind kind = Mark::Kind::Square;
    chess::Square square = chess::NoSquare;
    chess::Square from = chess::NoSquare, via = chess::NoSquare, to = chess::NoSquare;
    float strength = 0.0f;   // 0..1
    float age = 0.0f;        // seconds
};

class Director {
public:
    Director();
    ~Director();
    Director(const Director&) = delete;
    Director& operator=(const Director&) = delete;

    // Forget everything and perform on 'stage' from now on.
    void reset(Stage* stage, const DirectorConfig& config);
    void setConfig(const DirectorConfig& config);   // takes effect from the next line
    const DirectorConfig& config() const;

    // Queue a script (the priority rules above; beats keep their order inside a script).
    void play(const Script& script);
    // Insert a script right after the running beat, ahead of everything queued, without cutting
    // the running beat (the rules lesson's reactions before a WaitMove waits again).
    void playNext(const Script& script);
    // Synthesise these lines ahead of time (the next lesson chapter).
    void prefetch(const std::vector<Line>& lines);

    // Every frame while the game runs. 'ply' = the number of moves played in the game now (for
    // stale Low beats).
    void update(float dt, int ply);
    void setPaused(bool paused);     // focus lost, pause menu: the voice pauses, nothing starts
    void skip();                     // Space
    void skipCurrent();              // Space in the rules lesson: the running beat only
    void playerActed();              // the player touched a piece: queued Low beats are dropped
    // Stop now: voice, subtitle, gestures, marks, the offer card and every queued beat (a takeback,
    // leaving the game). Table actions already running finish in the scene.
    void clear();

    bool idle() const;               // nothing running, nothing queued
    bool speaking() const;           // a line is being heard (or read, without voice)
    // Key of the line the running beat says once it has started (heard, or shown without a voice);
    // nullptr otherwise.
    const std::string* runningKey() const;
    bool skippable() const;          // Space would skip something now
    // The running beat is a WaitMove (rules lesson); *expect = its expectation index.
    bool waitingMove(int* expect = nullptr) const;
    void endWait();                  // the WaitMove is satisfied: go on with the next beat
    // The running beat is an OfferTakeback whose line has been said: the card is shown and the
    // director waits for closeOffer().
    bool offerOpen() const;
    // Answered: hide the card, go on (the session drops beats if taken back). An offer not shown yet
    // is dropped. False when there was no offer.
    bool closeOffer();

    const std::vector<ShownMark>& marks() const;

    // ---- For the session and the tests ------------------------------------------------------------
    // Id of the script the last non-empty play() / playNext() queued (0 before any). pending(id)
    // stays true while a beat of that script runs or is queued (said, skipped or dropped: false).
    uint64_t lastScript() const;
    bool pending(uint64_t script) const;
    // Something runs or is queued, a WaitMove waiting for the player aside (lines said meanwhile
    // count).
    bool busy() const;
    // The player moved before the lesson reached its WaitMove: when only skippable lines and
    // pauses stand before the next queued WaitMove, drop them (cutting the running line) and wait
    // at once. False (nothing changes) otherwise.
    bool jumpToWait();
    // Drop the queued beats of this script (game over: the rest of the greeting, opening names).
    // The running beat finishes; table actions (demonstration moves, rewinds) are kept.
    void dropQueued(uint64_t script);
    // Called with every beat play() / playNext() queue, in order (tests, logs).
    void setObserver(std::function<void(const Beat&)> observer);

private:
    struct Impl;
    Impl* d_;
};

}  // namespace coach
