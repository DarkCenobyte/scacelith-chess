#include "ai/uci_host.h"

#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <thread>

#if defined(SCACELITH_HAS_STOCKFISH)
#include "stockfish_embedded.h"
#endif

namespace ai::detail {
namespace {

// std::cin replacement: a blocking queue of command lines. underflow() hands the engine one line
// (plus '\n') at a time and blocks until the client pushes the next one; a session ends with the
// "quit" command pushed by UciHost::releaseAny().
class LineInputBuf final : public std::streambuf {
public:
    void push(std::string line) {
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push_back(std::move(line));
        }
        cv_.notify_one();
    }
    void reset() {
        std::lock_guard<std::mutex> lk(m_);
        q_.clear();
        cur_.clear();
        setg(nullptr, nullptr, nullptr);
    }

protected:
    int_type underflow() override {
        if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return !q_.empty(); });
        cur_ = std::move(q_.front());
        q_.pop_front();
        cur_ += '\n';
        setg(cur_.data(), cur_.data(), cur_.data() + cur_.size());
        return traits_type::to_int_type(*gptr());
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::string> q_;
    std::string cur_;
};

// std::cout replacement: splits the byte stream into complete lines and queues them for the
// client. Unbuffered (every write lands in overflow/xsputn) and internally locked, so it is safe
// even for the few Stockfish writes that are not wrapped in its own sync_cout lock.
class LineOutputBuf final : public std::streambuf {
public:
    bool pop(std::string& line, int timeoutMs) {
        std::unique_lock<std::mutex> lk(m_);
        if (timeoutMs > 0 && q_.empty())
            cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return !q_.empty(); });
        if (q_.empty()) return false;
        line = std::move(q_.front());
        q_.pop_front();
        return true;
    }
    void reset() {
        std::lock_guard<std::mutex> lk(m_);
        q_.clear();
        partial_.clear();
    }

protected:
    int_type overflow(int_type c) override {
        if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
        char ch = traits_type::to_char_type(c);
        append(&ch, 1);
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        append(s, n);
        return n;
    }

private:
    void append(const char* s, std::streamsize n) {
        bool pushed = false;
        {
            std::lock_guard<std::mutex> lk(m_);
            for (std::streamsize i = 0; i < n; ++i) {
                char ch = s[i];
                if (ch == '\n') {
                    if (!partial_.empty() && partial_.back() == '\r') partial_.pop_back();
                    q_.push_back(std::move(partial_));
                    partial_.clear();
                    pushed = true;
                } else {
                    partial_ += ch;
                }
            }
        }
        if (pushed) cv_.notify_all();
    }

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::string> q_;
    std::string partial_;
};

}  // namespace

struct UciHost::State {
    mutable std::mutex m;  // guards owner / thread lifecycle (game thread only in practice)
    const void* owner = nullptr;
    bool wedged = false;   // a previous session could not be joined: never start another one
    std::thread thread;
    LineInputBuf in;
    LineOutputBuf out;
    std::streambuf* oldCin = nullptr;
    std::streambuf* oldCout = nullptr;
    std::mutex doneM;      // engine thread finished (so join() will not block)
    std::condition_variable doneCv;
    bool done = false;
    std::string archLimit = "auto";          // engine.arch, as accepted by the dispatcher
    std::vector<std::string> stopAtExitFor;  // variants whose initialisers preceded a stopEngineAtExit
};

UciHost::UciHost() : s_(new State) {}
UciHost::~UciHost() { delete s_; }

UciHost& UciHost::instance() {
    // Intentionally leaked: the engine thread may still use the stream buffers during process
    // exit if the game never called Engine::shutdown().
    static UciHost* host = new UciHost;
    return *host;
}

#if defined(SCACELITH_HAS_STOCKFISH)
namespace {
// Engine left running at process exit: stop it before Stockfish's globals are destroyed. A
// variant's static initialisers register those destructors with atexit when the variant is first
// chosen (stockfish_embedded_supported()), so this is registered after them, once per variant, and
// runs before them. The wait is bounded: on Windows exit() holds the CRT's atexit lock while
// running handlers, and an engine thread that is still initialising may block on that very lock
// (registering the destructor of a function-local static); it is then left parked until the
// process ends.
void stopEngineAtExit() {
    UciHost& h = UciHost::instance();
    h.releaseAny(2000);
}
}  // namespace
#endif

bool UciHost::acquire(const void* owner) {
#if defined(SCACELITH_HAS_STOCKFISH)
    std::lock_guard<std::mutex> lk(s_->m);
    if (s_->owner) return s_->owner == owner;
    if (s_->wedged) return false;
    // Chooses the variant and, the first time it is chosen, runs its static initialisers.
    if (!stockfish_embedded_supported()) {
        std::string built;
        for (const std::string& v : variants()) built += (built.empty() ? "" : ", ") + v;
        const char* best = stockfish_embedded_best_arch();
        if (std::strcmp(best, "none") == 0)
            LOGW("ai: this CPU runs none of the Stockfish variants of this build (%s): the engine is unavailable",
                 built.c_str());
        else
            LOGW("ai: engine.arch = %s excludes every Stockfish variant this CPU runs (built: %s; best: %s): "
                 "the engine is unavailable",
                 s_->archLimit.c_str(), built.c_str(), best);
        return false;
    }
    const std::string arch = stockfish_embedded_arch();
    // After the variant's initialisers, once per variant (see stopEngineAtExit).
    if (std::find(s_->stopAtExitFor.begin(), s_->stopAtExitFor.end(), arch) == s_->stopAtExitFor.end()) {
        s_->stopAtExitFor.push_back(arch);
        std::atexit(stopEngineAtExit);
    }
    s_->in.reset();
    s_->out.reset();
    s_->oldCin = std::cin.rdbuf(&s_->in);
    s_->oldCout = std::cout.rdbuf(&s_->out);
    std::cin.clear();
    std::cout.clear();
    s_->owner = owner;
    s_->done = false;
    const char* best = stockfish_embedded_best_arch();
    if (arch == best)
        LOGI("ai: starting embedded Stockfish (%s)", arch.c_str());
    else
        LOGI("ai: starting embedded Stockfish (%s, limited by engine.arch = %s; this CPU runs %s)", arch.c_str(),
             s_->archLimit.c_str(), best);
    State* st = s_;
    s_->thread = std::thread([st] {
        stockfish_embedded_main();
        {
            std::lock_guard<std::mutex> dl(st->doneM);
            st->done = true;
        }
        st->doneCv.notify_all();
    });
    return true;
#else
    (void)owner;
    return false;
#endif
}

void UciHost::release(const void* owner) {
    {
        std::lock_guard<std::mutex> lk(s_->m);
        if (!s_->owner || s_->owner != owner) return;
    }
    releaseAny(15000);
}

void UciHost::releaseAny(int timeoutMs) {
    std::lock_guard<std::mutex> lk(s_->m);
    if (!s_->owner) return;
    s_->in.push("stop");
    s_->in.push("quit");
    bool finished;
    {
        std::unique_lock<std::mutex> dl(s_->doneM);
        finished = s_->doneCv.wait_for(dl, std::chrono::milliseconds(timeoutMs), [this] { return s_->done; });
    }
    s_->owner = nullptr;
    if (!finished) {
        // Leave the thread and the redirected streams alone; they stay valid (leaked host).
        LOGW("ai: Stockfish did not stop within %d ms, leaving it behind", timeoutMs);
        s_->thread.detach();
        s_->wedged = true;
        return;
    }
    s_->thread.join();
    std::cin.rdbuf(s_->oldCin);
    std::cout.rdbuf(s_->oldCout);
    std::cin.clear();
    s_->in.reset();
    s_->out.reset();
}

bool UciHost::ownedBy(const void* owner) const {
    std::lock_guard<std::mutex> lk(s_->m);
    return owner && s_->owner == owner;
}

void UciHost::send(const std::string& line) { s_->in.push(line); }

bool UciHost::poll(std::string& line) { return s_->out.pop(line, 0); }

bool UciHost::waitLine(std::string& line, int timeoutMs) { return s_->out.pop(line, timeoutMs > 0 ? timeoutMs : 0); }

void UciHost::setArchLimit(const std::string& arch) {
#if defined(SCACELITH_HAS_STOCKFISH)
    std::lock_guard<std::mutex> lk(s_->m);
    if (arch == s_->archLimit) return;
    // Takes effect at the next acquire(); a running session keeps its variant.
    if (!stockfish_embedded_limit_arch(arch.c_str())) {
#if defined(__aarch64__) && defined(__APPLE__)
        LOGW("ai: unknown engine.arch \"%s\" (auto or apple-silicon), keeping %s", arch.c_str(), s_->archLimit.c_str());
#elif defined(__aarch64__)
        LOGW("ai: unknown engine.arch \"%s\" (auto or one of armv8, armv8-dotprod), keeping %s", arch.c_str(),
             s_->archLimit.c_str());
#else
        LOGW("ai: unknown engine.arch \"%s\" (auto or one of x86-64, x86-64-sse41-popcnt, x86-64-avx2, "
             "x86-64-avxvnni, x86-64-avx512icl), keeping %s",
             arch.c_str(), s_->archLimit.c_str());
#endif
        return;
    }
    s_->archLimit = arch;
#else
    (void)arch;
#endif
}

const char* UciHost::arch() const {
#if defined(SCACELITH_HAS_STOCKFISH)
    return stockfish_embedded_arch();
#else
    return "none";
#endif
}

std::vector<std::string> UciHost::variants() const {
    std::vector<std::string> out;
#if defined(SCACELITH_HAS_STOCKFISH)
    for (int i = 0; i < stockfish_embedded_variant_count(); ++i) out.push_back(stockfish_embedded_variant(i));
#endif
    return out;
}

}  // namespace ai::detail
