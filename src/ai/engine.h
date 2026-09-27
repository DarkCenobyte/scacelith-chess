// Opponent AI: Stockfish 16 compiled into the executable, running its UCI loop in a background
// thread with in-memory streams (no child process, no files). Implemented by the stockfish work
// package. All methods are called from the game (main) thread and never block for long.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ai {

struct EngineSettings {
    int skillLevel = 20;        // UCI "Skill Level" 0..20 (20 = full strength); ignored if limitStrength
    bool limitStrength = false; // UCI_LimitStrength
    int elo = 1500;             // UCI_Elo (Stockfish 16: 1320..3190)
    int depth = 0;              // go depth N (0 = none)
    int moveTimeMs = 0;         // go movetime N (0 = none)
    int64_t nodes = 0;          // go nodes N (0 = none)
    int threads = 1;
    int hashMB = 64;
    bool useClock = true;       // pass wtime/btime/winc/binc so the engine manages its time
    bool humanize = true;       // add human-like thinking time (see Engine::thinkTimeMs)
};

struct Preset {
    const char* name;           // "Novice", "Club Player", ...
    const char* description;    // one line shown in the menu
    int approxElo;              // shown to the player ("~1200")
    EngineSettings settings;
};
// Ordered from weakest to strongest; the last entry is "Custom" (settings come from the menu).
const std::vector<Preset>& presets();

struct ClockInfo {
    bool timed = false;
    int64_t whiteMs = 0, blackMs = 0, whiteIncMs = 0, blackIncMs = 0;
};

class Engine {
public:
    Engine();
    ~Engine();
    bool start();               // spawns the engine thread (idempotent); false if unavailable
    void shutdown();            // sends quit and joins
    bool available() const;
    void newGame();             // ucinewgame + isready
    void configure(const EngineSettings& s);

    // Asynchronous search from the standard start position + moves (UCI long algebraic).
    void requestMove(const std::vector<std::string>& uciMoves, const ClockInfo& clock);
    bool moveReady() const;
    // Returns the best move once ready ("e2e4", "e7e8q"), empty on failure. evalCp is from the
    // engine's point of view (side to move), mate scores mapped to +-100000.
    std::string takeMove(int* evalCp = nullptr);
    void stopSearch();          // abort a running search (move is still reported)

    // Quick evaluation for draw offers/claims (fixed small depth), asynchronous as well.
    void requestEval(const std::vector<std::string>& uciMoves);
    bool evalReady() const;
    int takeEval();             // centipawns from the side to move's point of view

    // Human-like thinking time before the move is physically played (ms), based on the time
    // control, remaining time, move number and how forced the position looks. The game waits
    // max(0, thinkTime - searchTime) after the search finishes. 0 when humanize is off.
    int thinkTimeMs(const ClockInfo& clock, int plyCount, int legalMoveCount, bool inCheck) const;

    // Decides whether the AI accepts a draw offer / claims a draw given its evaluation.
    bool acceptsDraw(int evalCp, int plyCount) const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace ai
