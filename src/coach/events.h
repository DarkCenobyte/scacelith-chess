// The coach's words around the game: greetings per level, who starts, level introductions,
// turn-taking, takebacks, encouragement, draw answers, the end of a game and the rules lesson's
// resume/next. Each helper returns a short Script (coach/script.h) whose lines are in
// assets/coach/speech/<lang>/events.lang; the director performs it like any other script.
// Check, checkmate and praise of good moves belong to the review (review.lang).
//
// Gaze rule: these lines are about the game with the player, so the coach looks at the player
// (with a nod or an open palm), except the thinking filler, said while looking at the position.
#pragma once
#include "script.h"
#include <string>
#include <vector>

namespace coach {

class Lesson;

enum class Encouragement : uint8_t {
    AfterMistake,   // after the player's mistake has been explained
    Behind,         // the evaluation below -3 pawns for three of the player's moves in a row
    PlayingWell     // several good moves in a row
};

enum class GameEnd : uint8_t { Win, Loss, Draw, Resigned };   // from the player's point of view

// Start of a coach game at 'level' (1 First steps .. 6 Expert): the greeting, who starts, and the
// level's introduction when 'introduceLevel' (the first game at that level, or after a change).
Script greetingScript(int level, chess::Color human, bool introduceLevel);
// A level's introduction alone (1 .. 6).
Script levelIntroScript(int level);

// Turn-taking: "Your move." after an explanation or a demonstration (Normal priority).
Script yourMoveScript(int ply = -1);
// "Take your time." when the player has been thinking long (Low: dropped once the player acts).
Script takeYourTimeScript(int ply = -1);
// A short filler while the coach still looks at the position (Low, looking at the board).
Script fillerScript(int ply = -1);

// The player's answer to the coach's takeback offer.
Script takebackScript(bool taken, int ply = -1);
// After an explanation without an offer.
Script playOnScript(int ply = -1);

Script encouragementScript(Encouragement kind, int ply = -1);
// The coach's answer to the player's draw offer.
Script drawAnswerScript(bool accepted);
// The coach's closing words, then "Let's shake hands." (the scene plays the handshake once it is
// said: Session::handshakeWanted).
Script gameEndScript(GameEnd end);

// The rules lesson: resuming at 'chapter' (the chapter is named), or going on to it.
Script lessonResumeScript(const Lesson& lesson, int chapter);
Script lessonNextScript(const Lesson& lesson, int chapter);

// Every catalog key the helpers above may say (base keys; the catalog adds the variants), for
// the tests.
std::vector<std::string> eventKeys();

}  // namespace coach
