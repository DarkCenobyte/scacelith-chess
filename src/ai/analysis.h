// Full-strength analysis results of the embedded Stockfish (see Engine::requestAnalysis): scores
// with mate distances and win/draw/loss chances, principal variations, and the parser of
// Stockfish's "info" lines. Engine-free value types, unit-testable without the engine.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ai {

// One Stockfish score, from the point of view of the side to move in the analysed position (as UCI
// reports it). Mate distances are kept: UCI "mate N" counts moves, not plies.
struct Score {
    int cp = 0;               // centipawns (meaningless when mate != 0). Stockfish normalises them
                              // with a material-dependent win-rate model: 100 is not "one pawn of
                              // material", derive material from the moves when explaining
    int mate = 0;             // > 0: the side to move mates in N moves; < 0: it is mated in -N; 0: no mate
    bool matedNow = false;    // "score mate 0" (no legal move and in check): the side to move is mated
    bool matesNow = false;    // flipped() of matedNow: the other side has no legal move and is mated
    bool hasWdl = false;
    int win = 0, draw = 1000, loss = 0;       // per mille (UCI_ShowWDL), side to move; W + D + L = 1000
    // Exact, or a bound left by an iteration that a stop interrupted ("lowerbound": at least this).
    enum class Bound : uint8_t { Exact, Lower, Upper } bound = Bound::Exact;

    Score flipped() const;                    // the other side's view: cp/mate negated, win<->loss, bounds swapped
    Score forWhite(bool whiteToMove) const { return whiteToMove ? *this : flipped(); }
    bool isMate() const { return mate != 0 || matedNow || matesNow; }
    // Expected score of the side to move, 0..1: (W + D/2) / 1000 with WDL, else a logistic of cp
    // (lichess' win-percentage curve); forced mates 1 / 0.
    double expected() const;
    // Total order (higher = better for the side to move): mates above any cp, shorter mates first;
    // being mated below any cp, later mates first; matesNow / matedNow at the two ends.
    int sortKey() const;
    std::string text() const;                 // "+1.25", "-0.40", "0.00", "M3", "-M2", "M0", "-M0" (UI, logs)
};

struct PvLine {
    int multipv = 1;                          // 1 = best
    int depth = 0, seldepth = 0;
    Score score;
    std::vector<std::string> pv;              // UCI moves from the analysed position
};

struct AnalysisRequest {
    std::string startFen;                     // "" = standard start position
    std::vector<std::string> moves;           // UCI, played from startFen
    int multiPV = 3;                          // lines wanted, clamped to 1..256 (Stockfish shows at most one per legal move)
    // Limits: at least one of depth, moveTimeMs and nodes must be > 0 (a search without one never
    // ends: the request fails). Use depth-only requests where results must be reproducible.
    int depth = 18;                           // go depth (0 = none), clamped to 1..245
    int moveTimeMs = 2500;                    // go movetime (0 = none), at most 1 hour
    int64_t nodes = 0;                        // go nodes (0 = none)
    int mateIn = 0;                           // go mate N (0 = none): stop once a mate in <= N moves is proven
    // Restrict the root to these moves (UCI). Each must be legal in the analysed position, else the
    // request fails: Stockfish would silently ignore it and search every move instead.
    std::vector<std::string> searchMoves;
    // Higher runs first among queued requests (moves have priority 0; background work such as an
    // end-of-game review should use a negative priority). A request never
    // overtakes one of equal or higher priority, a running search, or a queued ucinewgame / hash clear.
    int priority = 0;
};

struct Analysis {
    uint32_t id = 0;
    bool ok = false;                          // false: engine unavailable or invalid request (FEN, move, searchmove, no limit)
    bool whiteToMove = true;                  // side to move in the analysed position (scores are its view)
    std::string fen;                          // FEN of the analysed position (after the moves): a cache key
    // No legal move in the analysed position: checkmate (lines[0].score.matedNow) or stalemate
    // (lines[0].score.cp == 0, a certain draw). lines then holds one depth-0 line without moves.
    bool noLegalMove = false;
    std::string bestMove;                     // UCI; with searchMoves the best of those
    std::vector<PvLine> lines;                // the last complete report, sorted by multipv (best first)
    int depth = 0;                            // of lines[0]
    int64_t nodes = 0;
    int timeMs = 0;
    const PvLine* line(const std::string& firstMove) const;  // the line starting with that move, or nullptr
};

// Parses one "info ..." line of Stockfish 19 (depth, seldepth, multipv, score cp|mate, bounds, wdl,
// nodes, time, pv). False for "info string", "currmove" lines and any line without a score. A line
// without "multipv" (the depth-0 report of a position without legal moves) counts as multipv 1.
bool parseInfoLine(const std::string& line, PvLine& out, int64_t* nodes = nullptr, int* timeMs = nullptr);

}  // namespace ai
