// A Coach-mode game (levels 1-6) or the rules lesson (level 0), seen from the coach: what to say
// and when. The session owns the director and the coach's brain (Reviewer, Appraisal,
// OpeningAnnouncer, Lesson, the event lines) and follows the flows documented in the headers of
// coach/review.h, coach/appraisal.h, coach/openings.h, coach/lesson.h and coach/events.h:
//   - game start: greeting (and the level's introduction), opening names as they are identified;
//   - the human's turn: analysis A0 (and A3 at levels 3-4), background evaluations when idle;
//   - after every move: "Check!" / "Checkmate!"; after the human's move: A1/A2 when needed, the
//     review (verdict, explanation, demonstration, rewind, takeback offer, praise); after the
//     coach's move: its remarks and threat warnings;
//   - the takeback offer's answer, the player's takeback requests, draw answers;
//   - game end: closing words, the handshake (played by the scene), the appraisal (skippable);
//   - level 0: the lesson's chapters, WaitMove judging, illegal-move explanations, idle hints.
// Engine-free and GL-free: the world is reached through coach::Stage and coach::Analyst.
#pragma once
#include "appraisal.h"
#include "director.h"
#include "stage.h"
#include "../chess/chess.h"
#include <cstdint>
#include <string>
#include <vector>

namespace coach {

struct SessionConfig {
    int level = 1;                        // 0 = the rules lesson, 1..6 = the coach levels
    chess::Color human = chess::White;    // the lesson: always White
    DirectorConfig director;
    bool introduceLevel = false;          // first game at this level (or the level changed)
    bool offersEnabled = true;            // takeback offers after blunders
    std::vector<GameRecord> history;      // earlier coach games, oldest first (suggestLevel)
    bool accuracyExplained = false;       // the appraisal has already explained what accuracy is
    int lessonChapter = 0;                // level 0: resume at this chapter (0 = from the start)
};

class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // A new game (or lesson) on 'game' as it stands (the start position, or the lesson's first
    // set-up follows). Starts talking at once.
    void start(Stage& stage, Analyst& analyst, const chess::Game& game, const SessionConfig& config);
    void stop();                          // leaving: silence, cancel the coach's analyses

    // Every frame while the game runs (not while paused).
    void update(const chess::Game& game, float dt);
    void setPaused(bool paused);

    // ---- Events from the scene --------------------------------------------------------------------
    void onMove(const chess::Game& game);          // a move was completed (either side; lesson moves too)
    // The player touched a piece (idle timers, stale lines). While the takeback card is shown it
    // answers it: the player plays on (as onOfferAnswer(false), with "Let's play on").
    void onPlayerActive();
    // A placement refused as illegal (legal-move hints on): the rules lesson explains why.
    void onIllegalAttempt(const chess::Game& game, chess::Square from, chess::Square to);
    void onOfferAnswer(const chess::Game& game, bool accept);   // the takeback card's answer
    // The pause menu's "Take back": back to the player's last move (the coach's reply included).
    bool canTakeBack(const chess::Game& game) const;
    void onTakeBackRequested(const chess::Game& game);
    void onDrawAnswer(bool accepted);              // the coach answered the player's draw offer
    // The game is over (Game::isOver(): mate, stalemate, a draw, a resignation recorded on the Game).
    void onGameOver(const chess::Game& game, bool humanResigned);
    void skip();                                   // Space
    // Every frame while the engine searches the coach's move: seconds since that search began
    // (0 when none runs). Once the move is held by the engine alone (coachMayMove() true) for 6 s,
    // the coach says a short filler (once per move).
    void onCoachThinking(float seconds);

    // ---- What the scene asks -----------------------------------------------------------------------
    // The coach may play its move now (computed or not): no review, offer, demonstration or rewind is
    // running or pending for the human's last move.
    bool coachMayMove() const;
    // The player may pick up a piece: levels 1-6, the human's turn (no takeback card shown); the
    // lesson, while an exercise waits for the move. A lesson move made otherwise is taken back.
    bool playerMayMove(const chess::Game& game) const;
    bool handshakeWanted() const;                  // the closing words are said: play the handshake
    void onHandshakeDone(const chess::Game& game); // then the appraisal (level 0: the lesson ends)
    bool finished() const;                         // everything is said: show the end-of-game menu
    bool offerOpen() const;                        // show the takeback card (Stage::showTakebackOffer too)

    // ---- Results to persist (Settings [coach]) ------------------------------------------------------
    const std::vector<GameRecord>& history() const;   // with the game just finished appended
    bool accuracyExplained() const;
    int suggestedLevel() const;                    // 0 = no suggestion
    int lessonChapter() const;                     // the chapter to resume at
    bool lessonCompleted() const;

    Director& director();
    const Director& director() const;

private:
    struct Impl;
    Impl* d_;
};

}  // namespace coach
