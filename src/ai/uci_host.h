// Process-wide host of the embedded Stockfish 16 (third_party/stockfish): runs the engine's UCI
// loop on a background thread with std::cin / std::cout redirected to in-memory, thread-safe line
// queues. Stockfish keeps its state in globals, so there is exactly one host and one owner (an
// ai::Engine) at a time. Internal to src/ai.
#pragma once
#include <string>

namespace ai::detail {

class UciHost {
public:
    static UciHost& instance();

    // Starts an engine session for `owner` (redirects the streams, spawns the thread). Returns
    // false if the engine is not compiled in, the CPU lacks SSE4.1/POPCNT, or another owner holds
    // the session. Idempotent for the current owner.
    bool acquire(const void* owner);
    // Sends "quit", joins the engine thread and restores std::cin / std::cout. No-op for others.
    void release(const void* owner);
    // Same for whoever owns the session, waiting at most timeoutMs for the thread (then it is
    // detached and no new session can start). Used at process exit.
    void releaseAny(int timeoutMs);
    bool ownedBy(const void* owner) const;

    void send(const std::string& line);                // queue one UCI command, never blocks
    bool poll(std::string& line);                      // pop one engine output line, never blocks
    bool waitLine(std::string& line, int timeoutMs);   // pop one line, waiting up to timeoutMs

    const char* arch() const;                          // "x86-64-sse41-popcnt" or "none"

    UciHost(const UciHost&) = delete;
    UciHost& operator=(const UciHost&) = delete;

private:
    UciHost();
    ~UciHost();
    struct State;
    State* s_;
};

}  // namespace ai::detail
