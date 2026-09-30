// Level 0: the interactive rules lesson (research-pedagogy §3). Ten chapters of scripted beats
// (coach/script.h): the coach sets up small positions, explains, points, demonstrates, and waits
// for the player's move (WaitMove) that an Expectation judges. A wrong move is answered and taken
// back, an illegal one is refused and explained (why it is illegal, computed from the position),
// and the help escalates: a hint with pointing on the second failure, a demonstration of the
// solution on the third. The lesson never fails and is never recorded (no scoresheet, no clock,
// no rating, no end-of-game flow; touch-move relaxed: see the integrator notes in lesson.cpp).
//
// Engine-free: every chess fact comes from chess::Position.
#pragma once
#include "script.h"
#include "../chess/chess.h"
#include <string>
#include <utility>
#include <vector>

namespace coach {

constexpr float kLessonSpeechSpeed = 0.92f;   // TTS speed of the lesson (research-pedagogy §6.1)
constexpr float kLessonIdleHint1 = 20.0f;     // seconds without a touch before the first hint
constexpr float kLessonIdleHint2 = 45.0f;     // ... and the second one (with pointing)

// ---- Why a move is illegal ---------------------------------------------------------------------
enum class IllegalReason : uint8_t {
    None,                    // the move is legal
    NoPiece,                 // nothing on 'from' (the input layer never sends it)
    NotYourTurn,             // a piece of the side not to move
    OwnPieceOnTarget,
    WrongGeometry,           // the piece does not move like that
    PathBlocked,             // a piece stands in the way (culprit: the first one)
    PawnForwardBlocked,      // a pawn pushed onto an occupied square (culprit: the blocker)
    PawnCaptureNeedsVictim,  // a pawn moved diagonally onto an empty square
    EnPassantExpired,        // ... beside an enemy pawn that did not just make its double step
    LeavesKingInCheck,       // a pinned piece (culprit: the pinner)
    MustAnswerCheck,         // the king is in check and the move does not help (culprit: a checker)
    KingIntoCheck,           // the king steps onto an attacked square (culprit: an attacker)
    CastlingNoRights,        // the king or that rook has moved
    CastlingBlocked,         // a piece between them (culprit)
    CastlingOutOfCheck,      // castling while in check (culprit: a checker)
    CastlingThroughCheck,    // the king would cross an attacked square ('square', culprit: attacker)
    NeedsPromotionPiece      // a pawn reaching the last rank without a piece chosen
};
struct IllegalInfo {
    IllegalReason reason = IllegalReason::None;
    chess::Square culprit = chess::NoSquare;   // what to point at
    chess::Square square = chess::NoSquare;    // the square in question (the king's target, the crossed square)
};
// Reason why from->to is illegal in 'p' (IllegalReason::None when it is legal), from the public
// Position API alone. 'promo' as for Position::findLegal.
IllegalInfo whyIllegal(const chess::Position& p, chess::Square from, chess::Square to,
                       chess::PieceType promo = chess::NoPiece);
// Name of a reason: the catalog key of its explanation is "why." + name ("why.KingIntoCheck").
const char* illegalReasonName(IllegalReason r);

// ---- Exercises ----------------------------------------------------------------------------------
// The coach's answer to a wrong but legal move (the move is on the board while it is said).
struct LessonReply {
    enum class When : uint8_t {
        Moves,       // one of 'moves' (UCI)
        Piece,       // any move of a piece of type 'piece'
        Check,       // a check that is not checkmate
        Stalemate,   // a move that stalemates the coach
        Any          // any other move
    };
    When when = When::Any;
    std::vector<std::string> moves;
    chess::PieceType piece = chess::NoPiece;
    Line line;                  // "" with Check: the coach's escape is computed and said
    std::string demo;           // the coach's reply demonstrated (then rewound); "escape" = computed
    Line demoLine;              // said after the demonstration, before the rewind
    Line after;                 // said once the player's move is taken back
};

// A move accepted only from the player's n-th try on (1-based), with a gentler line.
struct LessonRetry {
    std::string uci;
    int fromTry = 2;
    Line line;
};

struct Expectation {
    enum class Kind : uint8_t {
        AnyOf,         // one of 'accept'
        AnyLegal,      // any legal move (the exercise is about the illegal ones)
        EscapesCheck,  // any legal move out of check (all of them are)
        Mates,         // a checkmating move
        Promotes       // a promotion in 'accept'
    };
    Kind kind = Kind::AnyOf;
    std::string fen;                                    // the position the player moves from
    std::vector<std::string> accept;                    // AnyOf / Promotes: UCI moves
    std::string solution;                               // demonstrated after the third failure (UCI)
    Line ask;                                           // the instruction (repeated by the first idle hint)
    Line success;                                       // said when the move is accepted
    std::vector<std::pair<std::string, Line>> successFor;   // ... for one accepted UCI move
    std::vector<LessonReply> replies;                   // wrong legal moves: the first match answers
    std::vector<LessonRetry> acceptOnRetry;
    std::vector<LessonReply> illegal;                   // own lines for some illegal attempts (Moves: "e1e2");
                                                        // the pointing at the culprit lands on their "{@}"
    Line hint1, hint2;                                  // idle hints (empty: the ask; the ask with pointing)
    std::vector<Gesture> hint2Gestures;
    std::vector<Mark> hint2Marks;
};

struct LessonChapter {
    std::string id;       // "welcome", "pieces", "safety", "check", "mate", "castling", "en_passant",
                          // "promotion", "draws", "etiquette"
    Line title;           // lesson.title.<id> ("the board"), for "Let's continue with {chapter}."
    Script beats;         // starts with a SetPosition
};

// What the director does after a move at a WaitMove.
struct LessonReaction {
    bool accepted = false;   // true: the lesson goes on after the WaitMove beat
    Script before;           // said and shown with the player's move still on the board
    int undo = 0;            // then this many plies of the lesson game are taken back by hand
    Script after;            // then this; not accepted: the same WaitMove waits again
};

class Lesson {
public:
    Lesson();   // builds the chapters (cheap: a few hundred beats)

    const std::vector<LessonChapter>& chapters() const { return chapters_; }
    const std::vector<Expectation>& expectations() const { return expectations_; }
    const Expectation& expectation(int index) const { return expectations_[size_t(index)]; }
    int chapterOf(int expectation) const;   // chapter holding that WaitMove, -1 if none

    // The player completed the legal move 'move' from 'before' at WaitMove 'expect'. failures:
    // wrong moves already made at this WaitMove (0 the first time; illegal attempts do not count).
    LessonReaction judge(int expect, const chess::Position& before, const chess::Move& move, int failures) const;
    // The player's placement from->to was refused as illegal at WaitMove 'expect' (-1: outside a
    // WaitMove). Empty when there is nothing to say (the promotion chooser is on screen, ...).
    Script explainIllegal(int expect, const chess::Position& pos, chess::Square from, chess::Square to,
                          chess::PieceType promo = chess::NoPiece) const;
    // Idle hints at WaitMove 'expect': stage 1 after kLessonIdleHint1 s without a touch, stage 2
    // after kLessonIdleHint2 s (once each; never while the coach speaks or demonstrates).
    Script idleHint(int expect, int stage) const;
    // The lines a chapter says whatever the player does (its beats, praise and hints), to
    // synthesise the next chapter's audio while this one plays. Lines of the answers to wrong
    // moves are left out (some name the square played).
    std::vector<Line> chapterLines(int chapter) const;

private:
    std::vector<LessonChapter> chapters_;
    std::vector<Expectation> expectations_;
    std::vector<int> chapterOfExpectation_;
    friend struct LessonBuilder;
};

}  // namespace coach
