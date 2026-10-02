#include "threads.h"
#include <algorithm>
#include <xmmintrin.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace tts {

void lowerThreadPriority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    // Linux: the nice value of a thread id applies to that thread only. +5 from the process's
    // (its main thread's) value, so helpers started by an already lowered thread get the same.
    errno = 0;
    int base = getpriority(PRIO_PROCESS, id_t(getpid()));
    if (errno == 0) setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), std::min(base + 5, 19));
#endif
}

FpGuard::FpGuard() : csr(_mm_getcsr()) { _mm_setcsr(csr | 0x8040u); }
FpGuard::~FpGuard() { _mm_setcsr(csr); }

ThreadPool::ThreadPool(int threads) {
    threads = std::clamp(threads, 1, 16);
    try {
        for (int i = 1; i < threads; ++i) workers_.emplace_back([this] { workerMain(); });
    } catch (const std::exception&) {
        // No more threads (std::system_error): run with the helpers started so far, size() counts them.
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    for (std::thread& t : workers_) t.join();
}

void ThreadPool::workerMain() {
    lowerThreadPriority();
    _mm_setcsr(_mm_getcsr() | 0x8040u);
    uint64_t seen = 0;
    for (;;) {
        const std::function<void(int)>* fn;
        int n;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            wake_.wait(lk, [&] { return quit_ || generation_ != seen; });
            if (quit_) return;
            seen = generation_;
            if (!job_) continue;   // woke after the others had finished that job
            fn = job_;
            n = jobSize_;
            ++active_;
        }
        std::exception_ptr error;
        try {
            for (int i = next_.fetch_add(1); i < n; i = next_.fetch_add(1)) (*fn)(i);
        } catch (...) {
            error = std::current_exception();
        }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (error && !error_) error_ = error;
            if (--active_ == 0) done_.notify_one();
        }
    }
}

void ThreadPool::run(int n, const std::function<void(int)>& fn) {
    if (n <= 0) return;
    if (workers_.empty() || n == 1) {
        for (int i = 0; i < n; ++i) fn(i);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        job_ = &fn;
        jobSize_ = n;
        next_.store(0);
        ++generation_;
    }
    wake_.notify_all();
    std::exception_ptr error;
    try {
        for (int i = next_.fetch_add(1); i < n; i = next_.fetch_add(1)) fn(i);
    } catch (...) {
        error = std::current_exception();
    }
    // Every item is taken (or the caller stopped on an exception). Wait only for the helpers still
    // running one: a helper that has not woken up yet (the machine is busy: render thread,
    // Stockfish) is not waited for, it finds no job when it does. 'fn' must outlive them all.
    std::unique_lock<std::mutex> lk(mutex_);
    job_ = nullptr;
    done_.wait(lk, [&] { return active_ == 0; });
    if (!error) error = error_;
    error_ = nullptr;
    lk.unlock();
    if (error) std::rethrow_exception(error);
}

}  // namespace tts
