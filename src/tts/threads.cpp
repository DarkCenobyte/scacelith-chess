#include "threads.h"
#include <algorithm>
#include <xmmintrin.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace tts {

void lowerThreadPriority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    // Linux: the nice value of a thread id applies to that thread only.
    setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), 5);
#endif
}

FpGuard::FpGuard() : csr(_mm_getcsr()) { _mm_setcsr(csr | 0x8040u); }
FpGuard::~FpGuard() { _mm_setcsr(csr); }

ThreadPool::ThreadPool(int threads) {
    threads = std::clamp(threads, 1, 16);
    for (int i = 1; i < threads; ++i) workers_.emplace_back([this] { workerMain(); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    for (std::thread& t : workers_) t.join();
}

void ThreadPool::work() {
    const std::function<void(int)>& fn = *job_;
    for (int i = next_.fetch_add(1); i < jobSize_; i = next_.fetch_add(1)) fn(i);
}

void ThreadPool::workerMain() {
    lowerThreadPriority();
    _mm_setcsr(_mm_getcsr() | 0x8040u);
    uint64_t seen = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(mutex_);
            wake_.wait(lk, [&] { return quit_ || generation_ != seen; });
            if (quit_) return;
            seen = generation_;
        }
        work();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (--busy_ == 0) done_.notify_one();
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
        busy_ = int(workers_.size());
        ++generation_;
    }
    wake_.notify_all();
    work();
    std::unique_lock<std::mutex> lk(mutex_);
    done_.wait(lk, [&] { return busy_ == 0; });
    job_ = nullptr;
}

}  // namespace tts
