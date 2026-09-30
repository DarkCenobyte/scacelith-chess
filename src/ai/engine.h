// Opponent AI: Stockfish 19 compiled into the executable, running its UCI loop in a background
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
    int elo = 1500;             // UCI_Elo (Stockfish 19: 1320..3190)
    int depth = 0;              // go depth N (0 = none)
    int moveTimeMs = 0;         // go movetime N (0 = none)
    int64_t nodes = 0;          // go nodes N (0 = none)
    int threads = 1;
    int hashMB = 64;
    bool useClock = true;       // pass wtime/btime/winc/binc so the engine manages its time
    bool humanize = true;       // add human-like thinking time (see Engine::thinkTimeMs)
    // UCI "MultiPV". While Skill Level / UCI_Elo weaken the engine, Stockfish picks its move among
    // max(4, multiPV) candidate lines, so values above 4 make it weaker still (used below 1320).
    int multiPV = 1;
};

struct Preset {
    const char* name;           // "Novice", "Club Player", ...
    const char* description;    // one line shown in the menu
    int approxElo;              // shown to the player ("~1200")
    EngineSettings settings;
};
// Ordered from weakest to strongest; the last entry is "Custom" (settings come from the menu).
const std::vector<Preset>& presets();
// Rating of a Stockfish player using preset 'index': the preset's approxElo, or estimateElo(custom)
// for "Custom" (the last entry) and out-of-range indices. The human's Elo is updated against it and
// the scoresheets show it for the Stockfish players.
int presetElo(int index, const EngineSettings& custom = EngineSettings{});

struct ClockInfo {
    bool timed = false;
    int64_t whiteMs = 0, blackMs = 0, whiteIncMs = 0, blackIncMs = 0;
    // Clock time the AI loses per move outside thinking (arm movement, clock press). Passed to
    // Stockfish as "Move Overhead" and reserved by thinkTimeMs so the AI never flags.
    int moveOverheadMs = 1000;
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    bool start();               // spawns the engine thread (idempotent); false if unavailable
    void shutdown();            // sends quit and joins
    bool available() const;
    // Only one Engine can run at a time (the engine reads and writes the process's std::cin /
    // std::cout): start() on a second instance returns false until the first one is shut down.
    bool ready() const;                  // UCI handshake done (commands sent earlier are queued)
    bool waitReady(int timeoutMs);       // blocks until ready(); for loading screens and tests
    void newGame();             // ucinewgame + isready
    // Settings used by the next requests. Two players may share the engine (AI vs AI in the
    // viewer mode): configure it with the side to move's settings before each requestMove(). A
    // move search whose strength settings differ from those of the previous move search starts
    // with a cleared hash table and search history ("Clear Hash", the same as ucinewgame), so a
    // side never plays with what the other one computed: the weak presets are calibrated with a
    // hash table that only ever saw their own shallow searches (presets.cpp), and each side of an
    // engine match has its own hash. Costs ~25 ms per switch at 64 MB (ai_engine_side_switch).
    void configure(const EngineSettings& s);
    // Queues a hash / history clear before the next search.
    void clearHash();
    int hashClears() const;     // clears sent so far (switches between sides included)
    // Blocks until the engine has processed every command sent or queued so far (isready /
    // readyok, after a running search completes); false on timeout. For tests and measurements.
    bool sync(int timeoutMs);

    // Asynchronous search from the standard start position + moves (UCI long algebraic). A new
    // request replaces a pending one; on an engine that is not running, or with a move that is
    // not legal (never sent: Stockfish would end the process), it fails (moveReady() true,
    // takeMove() empty). Untimed games without depth/nodes/movetime search 1 s (handicapped) or
    // 3 s (full strength); the humanised thinking time runs concurrently.
    void requestMove(const std::vector<std::string>& uciMoves, const ClockInfo& clock);
    bool moveReady() const;
    // Returns the best move once ready ("e2e4", "e7e8q"), empty on failure. evalCp is from the
    // engine's point of view (side to move), mate scores mapped to +-100000.
    std::string takeMove(int* evalCp = nullptr);
    void stopSearch();          // abort a running search (move is still reported)
    int lastSearchMs() const;   // wall time of the last completed move search

    // Quick evaluation for draw offers/claims (full strength whatever the preset, depth 12, at most
    // 1.5 s), asynchronous as well; queued behind a running move search and vice versa. Fails
    // like requestMove(), with a neutral 0.
    void requestEval(const std::vector<std::string>& uciMoves);
    bool evalReady() const;
    int takeEval();             // centipawns from the side to move's point of view

    // Human-like thinking time before the move is physically played (ms), based on the time
    // control, remaining time, move number and how forced the position looks. The game waits
    // max(0, thinkTime - searchTime) after the search finishes. 0 when humanize is off.
    // plyCount = half-moves played so far (the AI is to move). Call it after takeMove(): an
    // obvious recapture of the opponent's last capture is then detected and played faster.
    // Randomised (log-normal), so call it once per move.
    int thinkTimeMs(const ClockInfo& clock, int plyCount, int legalMoveCount, bool inCheck) const;

    // Decides whether the AI accepts a draw offer / claims a draw given its evaluation (evalCp
    // from the AI's point of view, e.g. takeEval() with the AI to move). Thresholds in humanize.cpp.
    bool acceptsDraw(int evalCp, int plyCount) const;
    // Whether the AI offers a draw along with the move it is about to play (evalCp from its own
    // point of view, as for acceptsDraw; pliesSinceOwnOffer = half-moves since its last offer,
    // -1 = never). Used between two AIs; the opponent answers with acceptsDraw().
    bool offersDraw(int evalCp, int plyCount, int pliesSinceOwnOffer) const;

    const EngineSettings& settings() const;
    // Rough playing strength (Elo) of a settings block, as used for the presets and thinkTimeMs.
    static int estimateElo(const EngineSettings& s);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace ai
