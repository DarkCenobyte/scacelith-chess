// One run of a challenge (coach/challenge.h) by the coach: the session (coach/session.h) holds it
// instead of a game when SessionConfig::challenge names a set, and forwards the scene's events.
//
// The flow, position after position:
//   - the set's introduction (ch.intro.<id>) before the first position, "Next position." before
//     the others;
//   - the set-up (SetPosition), the coach's move into it when there is one (PlayMove, by hand), the
//     task ("Mate in 2.", "Find the fork.", "Checkmate my king."), then a WaitMove;
//   - the player's move: a line is judged against the solution at once (ChallengePosition::accepts);
//     a right move gets a nod and the coach's answer from the line, the last one the praise. A
//     play-out move is judged by Stockfish (the position after it, the coach to move): it must keep
//     the win (mate, promote) or the draw (hold); the same analysis gives the coach's reply. A
//     reply that draws on the board (stalemate, no mating material) solves a hold once played,
//     with no wait after it: no position is ever waited in without a move to make;
//   - a wrong move: Stockfish's best answer is played by hand while the coach says it ("Not this
//     one: I'd answer Qxd1."), taken back with the player's move, and the same WaitMove waits again.
//     Every sentence is proved by the board or the engine: "would be checkmate" only when the
//     answer mates on the board, "That's stalemate" only on the board, "it takes longer" only when
//     the engine still sees a mate, longer than the position asks;
//   - hints only on request: the H key (requestHint) at any time the position waits, or the
//     coach's offer, a question with a card, after every kHintOfferAfter wrong moves at one move.
//     Step 1 the piece (or the square when that piece is the only one that can move), 2 the square,
//     3 the move shown by hand (challengeHint);
//   - after the last position, the closing words; then the session wants the handshake.
// Nothing is recorded: no review, no appraisal, no history; the game's moves are the board's only.
#pragma once
#include "challenge.h"
#include "director.h"
#include "stage.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace chess { class Game; }

namespace coach {

constexpr int kHintOfferAfter = 3;          // wrong moves at one move before the coach offers a hint
constexpr float kChallengeAnswerWait = 6.0f;   // seconds the reaction to a wrong move waits for the engine

class ChallengeRun {
public:
    ChallengeRun();
    ~ChallengeRun();
    ChallengeRun(const ChallengeRun&) = delete;
    ChallengeRun& operator=(const ChallengeRun&) = delete;

    // Starts 'challenge' (copied) on the director, which performs on 'stage'. Talks at once.
    // 'from': the position to begin at, 0-based (tests and screenshots; the set's introduction is
    // said all the same).
    void start(const Challenge& challenge, Director& director, Stage& stage, Analyst& analyst, int from = 0);
    void stop();                                   // the analyses are cancelled (the director is the session's)
    void update(const chess::Game& game, float dt);

    // ---- Events ----------------------------------------------------------------------------------
    void onMove(const chess::Game& game);          // a move was completed (the player's or the coach's)
    void onPlayerActive();                         // a piece touched: the hint offer is declined
    void answerOffer(bool accept);                 // the hint card's answer
    void requestHint(const chess::Game& game);     // H

    // ---- What the session and the scene ask ---------------------------------------------------------
    bool playerMayMove(const chess::Game& game) const;
    bool hintAvailable(const chess::Game& game) const;   // H would give a hint now
    bool offerOpen() const;                        // the hint offer's card is up
    bool completed() const;                        // every position solved
    bool finished() const;                         // completed and the closing words said
    int position() const;                          // the position being played, 0-based
    int positions() const;
    const Challenge& challenge() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace coach
