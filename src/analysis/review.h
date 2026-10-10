// Analysis mode: Stockfish's review of a whole game, position by position, and what it makes of
// each move: the evaluation bar, the annotation symbols (!!, !, !?, ?!, ?, ??) and the better move
// the arrows show. Engine-free and GL-free: the game scene (game_scene_analysis.cpp) sends the
// requests this class asks for to ai::Engine and hands the results back; the tests build
// ai::Analysis values by hand.
//
// Positions and plies. Position i is the position before ply i (0 = the start position, plies()
// = the final one); ply i goes from position i to position i + 1. Every position is analysed on
// its own (MultiPV 2, scores from its side to move): the played move's value is the next
// position's score seen from the mover (the lichess way), so one search per position judges both
// the move that leads to it and the one that leaves it.
//
// Passes. A quick pass (Settings::quickDepth) over every position first, so that the bar and the
// symbols appear within seconds, then the deep pass (Settings::deepDepth) that makes them final.
// Each pass serves the positions around the one on the board first (focus, focus + 1, focus - 1,
// focus + 2 ...), then the rest from the start: nextRequest() is asked again after every result,
// so following the player as they move about the game costs nothing. Ahead of both passes come
// the urgent positions the caller names: those the comment waited for on the board needs final
// (Commentator::needs), quick then deep, so that it never waits for the whole quick pass of a
// long game. A position without a legal move (checkmate, stalemate) needs no search: it is known
// from the start.
//
// Move classes and symbols (verdict()):
//   - coach::judge (lichess win-percentage model) gives the class from the best move's score and
//     the played move's: Inaccuracy -> ?! (Dubious), Mistake -> ? , Blunder -> ?? .
//   - !! (Brilliant): the best move (or within 2 W% points of it) that gives material away (static
//     exchange on its square, or a piece of the mover's left to be taken with profit: at least 2
//     points), the mover not losing afterwards (W% >= 50) and not already winning outright before
//     (W% < 90); never in the opening book, never a recapture.
//   - !  (Good, "the only move"): the best move, the second best line at least 12 W% points worse,
//     more than one legal move, not a recapture, not the capture of material the previous move
//     gave away, nor a forced reply to a check that leaves one sensible answer, and the game not
//     already decided (best W% between 8 and 92); not in book.
//   - !? (Interesting): not the best move, within 5 W% points of it (Good or better), and it gives
//     material away as for !!.
//   - Book moves (the position after them is in coach::OpeningBook) get no symbol, unless they are
//     blunders: the book holds traps and losing lines too (1.f3 e5 2.g4?? Qh4#, "Fool's Mate").
// A verdict is shown from the quick pass on (provisional), final once both of its positions have
// had the deep pass.
#pragma once
#include "../ai/analysis.h"
#include "../chess/chess.h"
#include "../coach/review.h"
#include <cstdint>
#include <string>
#include <vector>

namespace analysis {

// Annotation symbols, numbered as the PGN NAGs $1..$6.
enum class Nag : uint8_t {
    None = 0,
    Good = 1,          // !
    Mistake = 2,       // ?
    Brilliant = 3,     // !!
    Blunder = 4,       // ??
    Interesting = 5,   // !?
    Dubious = 6        // ?!
};
const char* nagSymbol(Nag n);   // "!", "?", "!!", "??", "!?", "?!"; "" for None
// The symbol's colour (linear RGB, for the 3D marks; the 2D overlay converts it): ?? red,
// ? orange, ?! yellow, !? violet, ! blue, !! teal. None: neutral grey.
struct Rgb { float r, g, b; };
Rgb nagColor(Nag n);
// The better move's arrow (the engine's choice where the played move was worse): green.
Rgb betterMoveColor();

struct Settings {
    int quickDepth = 10;
    int deepDepth = 18;
    int multiPV = 2;
    int deepMoveTimeMs = 6000;   // a deep search never runs longer than this (a long endgame think)
    int quickMoveTimeMs = 800;
};

// One analysed position.
struct PositionEval {
    int depth = 0;                 // 0 = not analysed yet
    bool final = false;            // the deep pass is done (or no search is needed)
    bool failed = false;           // the engine refused it (no engine, bad FEN): never analysed
    bool terminal = false;         // no legal move: checkmate (best.matedNow) or stalemate (cp 0)
    ai::Score best;                // the best line's score, side to move's view
    std::string bestUci;           // "" when terminal
    std::vector<std::string> pv;   // the best line (UCI), its first move = bestUci
    bool hasSecond = false;        // a second line (MultiPV 2; absent with a single legal move)
    ai::Score second;
    std::string secondUci;
    int legalMoves = 0;
};

// What the review makes of ply i (position i -> i + 1).
struct Verdict {
    bool known = false;            // both positions analysed (quick pass at least)
    bool final = false;            // both at the deep pass
    chess::Color mover = chess::White;
    std::string uci, san;          // the move played
    coach::MoveClass cls = coach::MoveClass::Unjudged;
    Nag nag = Nag::None;
    double wBest = 50.0;           // mover's W% with the engine's move (position i)
    double wPlayed = 50.0;         // mover's W% after the played move (position i + 1)
    double loss = 0.0;             // max(0, wBest - wPlayed); 0 when the played move is the best
    bool playedBest = false;       // the played move is the engine's first choice
    bool book = false;             // the position after it is in the opening book
    bool sacrifice = false;        // it gives material away (see !!)
    bool onlyMove = false;         // see !
    std::string betterUci, betterSan;   // the engine's move when the played one was not it ("" otherwise)
    int whiteCpAfter = 0;          // the evaluation after the move, White's view, mates +-10000
};

// The evaluation bar for a position: White's share of it and the figure written on it.
struct EvalBar {
    bool known = false;
    float white = 0.5f;            // 0..1: lichess winning chances of White (mates 1 / 0)
    std::string text;              // "+1.3", "-0.4", "0.0", "M3" (White mates), "-M2", "1-0" / "0-1" /
                                   // "½-½" for a final position that is mate or stalemate
    int mateWhite = 0;             // > 0 White mates in N, < 0 Black mates in N, 0 none
};

// Per side, over the plies analysed so far (book moves count as perfect).
struct SideSummary {
    int moves = 0;                 // plies judged
    double accuracy = 0.0;         // lichess game accuracy (0..100): the mean of the moves' accuracy
                                   // (coach::moveAccuracy), weighted as lichess does with the
                                   // volatility of the position, and its harmonic mean, averaged
    int count[7] = {0, 0, 0, 0, 0, 0, 0};   // by Nag (index = Nag value)
};

class GameReview {
public:
    // A new game: the start position and the moves (legal from it, in order). Positions without a
    // legal move are final at once.
    void reset(const chess::Position& start, const std::vector<chess::Move>& moves, const Settings& s = Settings());
    void clear();

    int plies() const { return int(moves_.size()); }
    int positions() const { return plies() + 1; }
    const chess::Position& positionAt(int i) const;
    const chess::Game& game() const { return game_; }   // the whole game (SAN, positions)

    // The next search to run: the request and the position it is for. 'focus' is the position on
    // the board; 'urgent' the positions wanted final before anything else, in order (the comment
    // waited for: their quick searches first, then their deep ones; out-of-range entries are
    // ignored). False when every position is final (or failed). A position already handed out and
    // not answered yet is not handed out again until accept() or fail() comes for it.
    bool nextRequest(int focus, ai::AnalysisRequest& out, int& position, const std::vector<int>& urgent = {});
    void accept(int position, const ai::Analysis& a);   // a result (ok false = fail)
    void fail(int position);                            // never analysable: left out
    // A pending request was dropped (the scene cancelled it): the position can be handed out again.
    void forget(int position);

    const PositionEval& position(int i) const;
    Verdict verdict(int ply) const;
    EvalBar bar(int position) const;
    float progress() const;        // 0..1: positions final (or failed) / positions
    bool complete() const;         // every position final or failed
    SideSummary summary(chess::Color c) const;
    const Settings& settings() const { return settings_; }

    // Cache: what an earlier review of the same game found (analysis/cache.h). The key names the
    // start position and the moves; restore() takes the evaluations of a saved review (those at
    // least as deep as this review's passes become quick or final at once). 'savedDeepDepth' is the
    // deep pass's depth of the review that saved them (0: unknown): when it is at least this one's,
    // a position saved final stays final whatever its depth (a deep search its time limit cut short
    // is not run again); a deeper review searches again what is not deep enough for it.
    std::string key() const;
    std::vector<PositionEval> evaluations() const { return evals_; }
    void restore(const std::vector<PositionEval>& evals, int savedDeepDepth = 0);

private:
    Settings settings_;
    chess::Game game_;
    std::vector<chess::Move> moves_;
    std::vector<chess::Position> positions_;
    std::vector<PositionEval> evals_;
    std::vector<bool> pending_;
    std::vector<bool> inBook_;     // position i is in the opening book
    // The board facts of each ply the symbols use, fixed for the game (reset()): the evaluations
    // only decide which of them count, so verdict() stays cheap enough for every frame.
    struct PlyFacts {
        bool gives = false;        // the move gives material away (Verdict::sacrifice)
        bool recapture = false;    // coach::isRecapture
        bool forcedReply = false;  // a reply to a check that leaves one sensible answer at most
        bool takesGift = false;    // a capture right after a move that gave material away
    };
    std::vector<PlyFacts> facts_;
};

}  // namespace analysis
