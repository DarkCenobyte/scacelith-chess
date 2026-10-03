// Process-wide host of the embedded Stockfish 19 (third_party/stockfish): runs the engine's UCI
// loop on a background thread with std::cin / std::cout redirected to in-memory, thread-safe line
// queues. The engine reads and writes the process's standard streams, so there is exactly one host
// and one owner (an ai::Engine) at a time. Internal to src/ai.
#pragma once
#include <string>
#include <vector>

namespace ai::detail {

class UciHost {
public:
    static UciHost& instance();

    // Starts an engine session for `owner` (redirects the streams, spawns the thread) with the
    // best Stockfish variant for this CPU within the arch limit. Returns false if the engine is not
    // compiled in, no variant of this build fits, or another owner holds the session. Idempotent
    // for the current owner.
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

    // Caps the instruction-set variant of the sessions started from now on (Scacelith.ini
    // engine.arch): "auto" (the default) or a Stockfish ARCH name such as "x86-64-avx2", never
    // above what the CPU runs. An unknown name is logged and leaves the limit unchanged.
    void setArchLimit(const std::string& arch);
    // Variant chosen by the last start attempt (e.g. "x86-64-avx2"); "none" before the first one
    // or when the last attempt found no variant within engine.arch.
    const char* arch() const;
    // Variants built into this executable, from the baseline up (empty without the engine).
    std::vector<std::string> variants() const;

    UciHost(const UciHost&) = delete;
    UciHost& operator=(const UciHost&) = delete;

private:
    UciHost();
    ~UciHost();
    struct State;
    State* s_;
};

}  // namespace ai::detail
