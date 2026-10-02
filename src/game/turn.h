// The turn of a game at the table (GameScene, game_scene.h): who acts and what the hands are doing,
// apart so that the unit tests (tests/game_mode_tests.cpp) reach its rules without the scene.
#pragma once

namespace game {

enum class Turn {
    None,
    HumanIdle,       // nothing touched yet
    HumanTouched,    // a piece is gripped on its square (touch-move applies)
    HumanPlacing,    // the hand is moving the piece (and any capture / castling rook)
    HumanPromotion,  // pawn on the last rank: choosing the new piece
    HumanPlaced,     // move made on the board, waiting for the clock press (untimed: completed at once)
    HumanPressing,   // hand on its way to the clock
    AiThinking,
    AiMoving,
    RemoteWaiting,   // online: waiting for the opponent's move
    RemoteMoving,    // online: the opponent's robot is placing the move
    CoachTable,      // coach: moves being taken back, a lesson position set up or a lesson move
                     // played by hand (the hands work on the table; nobody plays meanwhile)
    LessonWait       // rules lesson: the coach's side is to move, or the lesson has the floor
};

// No move on its way between the board and the clock: a draw agreed now leaves on the board what
// the game has.
inline bool quietTurn(Turn t) { return t == Turn::HumanIdle || t == Turn::HumanTouched || t == Turn::AiThinking; }

// The pause menu's Offer draw, Claim draw and Resign (a game against the AI, or two players on one
// PC): on a quiet turn, and while my move waits for the clock press (a draw offer goes with it,
// FIDE 9.1.2.1). Greyed while a hand, the human's or the robot's, carries out a move: the end of
// the game would cut it off, the piece on its new square and the record without it.
inline bool menuMayEndGame(Turn t) { return quietTurn(t) || t == Turn::HumanPlaced; }

// A coach game: what becomes of the player's draw offer, made at offerPly, once the coach's
// evaluation is in and ply moves are on the board (GameScene::updateCoach).
enum class CoachDrawStep {
    Wait,           // a move is on its way (quietTurn): the answer waits for it
    Answer,         // the evaluation is of the position on the board: the coach answers from it
    EvaluateAgain,  // the player moved or took a move back since: the offer stands until the coach
                    // answers (FIDE 9.1.2.3), so the position now on the board is evaluated again
    NoAnswer        // the game ended meanwhile
};
inline CoachDrawStep coachDrawStep(bool playing, Turn t, int offerPly, int ply) {
    if (!playing) return CoachDrawStep::NoAnswer;
    if (!quietTurn(t)) return CoachDrawStep::Wait;
    return offerPly == ply ? CoachDrawStep::Answer : CoachDrawStep::EvaluateAgain;
}

}  // namespace game
