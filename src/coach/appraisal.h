// The coach's end-of-game appraisal (research-pedagogy §5): one verdict per ply collected during
// the game (the human's from the review, the coach's from the neighbouring evaluations, background
// analyses for the gaps), the statistics of §5.2 (lichess game accuracy, phase accuracy, class
// counts, best streak, critical moment, turning point, best moment, recurring theme, takebacks,
// material), and the appraisal Script of §5.3-5.5: opener, highlight, numbers, one improvement,
// encouragement and level suggestion. Every beat is skippable. Lines are catalog keys of
// assets/coach/speech/<lang>/appraisal.lang.
//
// Engine-free: coach::Session hands in the analyses; nothing is searched at the end of the game.
#pragma once
#include "../ai/analysis.h"
#include "../chess/chess.h"
#include "review.h"
#include "script.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace coach {

// ---- Lichess accuracy (AccuracyPercent.scala) -----------------------------------------------------
// Game accuracy of each side from the evaluations after every ply (White's view, centipawns, mates
// as +-1000; clamped). The position before the first ply counts as 'startCp' (lichess: +15).
// 'firstMover' made the first ply of the series. -1 for a side without a move.
struct SideAccuracy {
    double white = -1.0, black = -1.0;
    double of(chess::Color c) const { return c == chess::White ? white : black; }
};
SideAccuracy gameAccuracy(const std::vector<int>& cpsWhiteAfterEachPly, chess::Color firstMover = chess::White,
                          int startCp = 15);

// The accuracy players of a rating typically reach (the server's anti-cheat priors: 600 -> 65,
// 1000 -> 71, 1500 -> 79, 2000 -> 86, 2500 -> 91, linear in between), and the rating of a level.
double typicalAccuracy(int rating);
int levelRating(int level);   // 750, 1050, 1350, 1650, 1950, 2250

// One finished Coach game, for the level suggestion (research-pedagogy §1.8).
struct GameRecord {
    int level = 1;
    int result = 0;          // +1 the human won, 0 draw, -1 lost
    double accuracy = -1.0;  // the human's game accuracy, -1 unknown
};
// Suggested level after the games so far (oldest first, the last one just finished): up after 2 wins
// in the last 3 games at this level with accuracy >= the next level's typical accuracy; down after
// 3 losses in a row at this level with accuracy below its typical accuracy; else the same level.
// Never down right after a win. Returns 1..6.
int suggestLevel(int level, const std::vector<GameRecord>& games);

// ---- Statistics (research-pedagogy §5.2) ------------------------------------------------------------
enum class BestMoment : uint8_t { None, Brilliant, Great, OnlyMove, WonMaterial, Streak, GoodCapture, Phase };

struct SideStats {
    double accuracy = -1.0;                        // lichess game accuracy, -1 unknown
    std::array<double, 3> phaseAccuracy{{-1.0, -1.0, -1.0}};   // opening, middlegame, endgame (>= 6 plies of the side)
    std::array<int, 9> classes{};                  // count per MoveClass (index = int(MoveClass))
    int judged = 0, unjudged = 0;                  // judged: cls other than Unjudged
    int topMoves = 0;                              // Best + Excellent among judged non-book non-forced plies
    int captured = 0;                              // points captured (1-3-3-5-9)
    int piecesCaptured = 0;                        // coach men (pawns included) captured
    int checks = 0;
    int count(MoveClass c) const { return classes[size_t(c)]; }
};

struct AppraisalStats {
    int plies = 0, fullMoves = 0;
    chess::GameStatus status = chess::GameStatus::Ongoing;
    chess::GameEndReason reason = chess::GameEndReason::None;
    int result = 0;                                // the human's view: +1 won, 0 draw, -1 lost (or unfinished)
    SideStats human, coach;
    int humanMoves = 0;
    bool numbers = false;                          // accuracy may be voiced: >= 10 human moves, <= 20 % unjudged
    int onlyMoves = 0, brilliant = 0, great = 0;
    int bestStreak = 0;                            // longest run of Best / Excellent (book / forced skipped)
    int criticalPly = -1;                          // the human ply with the largest Δ >= 10 (earliest on ties)
    int turningPly = -1;                           // first human ply from W% >= 50 to < 40 (or a comeback <= 50 to > 60)
    BestMoment bestMoment = BestMoment::None;
    int bestPly = -1;                              // the best moment's ply (-1: streak / phase)
    int bestPhase = -1;                            // BestMoment::Phase: 0..2
    ExType theme = ExType::None;                   // recurring theme, else the critical moment's type
    bool themeRecurring = false;
    int offers = 0, takebacks = 0, fixed = 0;      // takeback offers, taken, replayed with Δ < 5
    int coachHungPly = -1;                         // a coach move that hung a piece of 3+ the human did not take
    chess::PieceType coachHungType = chess::NoPiece;
    double humanWinAtEnd = -1.0;                   // the human's W% at the last evaluated position (resignation)
};

// ---- The appraisal script ----------------------------------------------------------------------------
struct AppraisalContext {
    bool humanResigned = false;
    bool explainAccuracy = false;   // first appraisal with numbers: say what accuracy means (level 3)
    std::string opening;            // W10 opening id for Arg::ofOpening ("" = none): said from level 3
    int outOfBookMove = 0;          // the move number the game left the book (level 5), 0 = unknown
    int suggestedLevel = 0;         // suggestLevel() result; 0 or the current level = no suggestion
};

class Appraisal {
public:
    void reset(int level, chess::Color human);
    int level() const { return level_; }
    chess::Color human() const { return human_; }

    // During the game. The human's reviewed move: its verdict (and the takeback record of a retry,
    // which replaces the verdict of the taken-back move at the same ply).
    void add(const Review& r);
    // The evaluation of the position after 'plies' moves (A0 of the human's turn, or a background
    // analysis): its best line, side to move's view, as the analysis reports it.
    void addEval(size_t plies, const ai::Analysis& a);
    // After Game::undo (a takeback, or anything else that shortens the game): forget what lies past
    // the current move list.
    void truncate(size_t plies);
    // Positions after 1..moves().size() plies whose evaluation is still unknown, for background
    // analyses when the engine is free (the coach's moves are judged from these evaluations).
    std::vector<size_t> missingEvals(const chess::Game& g) const;
    // A background evaluation request (priority -1, MultiPV 1, depth 14, 400 ms).
    ai::AnalysisRequest evalRequest(const chess::Game& g, size_t plies) const;

    // At the end of the game (whoever won; also callable during the game for a panel).
    AppraisalStats stats(const chess::Game& g) const;
    // The coach's verdict of every ply (the human's from the review, the coach's from the
    // neighbouring evaluations); Unjudged where nothing is known.
    std::vector<PlyVerdict> verdicts(const chess::Game& g) const;
    Script script(const chess::Game& g, const AppraisalContext& ctx) const;

private:
    int level_ = 1;
    chess::Color human_ = chess::White;
    std::vector<PlyVerdict> humanVerdicts_;   // index = ply; ply -1 = none
    std::vector<int> eval_;                    // index = plies played; White's view cp
    std::vector<uint8_t> evalKnown_;           // 0 unknown, 1 from a move's line, 2 from a root analysis
    int offers_ = 0, takebacks_ = 0, fixed_ = 0;
    void setEval(size_t plies, int cpWhite, uint8_t source);
};

}  // namespace coach
