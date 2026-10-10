#include "threads.h"
#include <algorithm>
#if !defined(__aarch64__)
#include <xmmintrin.h>
#endif
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#else
#include <cerrno>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace tts {

#if defined(__aarch64__)
namespace {
// FPCR.FZ (bit 24): denormal inputs and results of single and double precision flushed to zero,
// what MXCSR's FTZ (0x8000) and DAZ (0x40) do together on x86.
constexpr uint64_t kFpcrFz = uint64_t(1) << 24;
uint64_t readFpcr() {
    uint64_t v;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(v) : : "memory");
    return v;
}
void writeFpcr(uint64_t v) { __asm__ __volatile__("msr fpcr, %0" : : "r"(v) : "memory"); }
}  // namespace
#endif

void lowerThreadPriority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__APPLE__)
    // macOS: setpriority() acts on the whole process. The thread keeps its quality-of-service class
    // (which also steers it between the performance and efficiency cores) at the relative
    // priority -5 within it (QOS_MIN_RELATIVE_PRIORITY is -15), like nice +5; a thread without a
    // class takes the default one. A helper started by a lowered thread gets the same.
    qos_class_t qos = qos_class_self();
    pthread_set_qos_class_self_np(qos == QOS_CLASS_UNSPECIFIED ? QOS_CLASS_DEFAULT : qos, -5);
#else
    // Linux: the nice value of a thread id applies to that thread only. +5 from the process's
    // (its main thread's) value, so helpers started by an already lowered thread get the same.
    errno = 0;
    int base = getpriority(PRIO_PROCESS, id_t(getpid()));
    if (errno == 0) setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), std::min(base + 5, 19));
#endif
}

#if defined(__aarch64__)
FpGuard::FpGuard() : csr(readFpcr()) { writeFpcr(csr | kFpcrFz); }
FpGuard::~FpGuard() { writeFpcr(csr); }
#else
FpGuard::FpGuard() : csr(_mm_getcsr()) { _mm_setcsr(csr | 0x8040u); }
FpGuard::~FpGuard() { _mm_setcsr(csr); }
#endif

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
#if defined(__aarch64__)
    writeFpcr(readFpcr() | kFpcrFz);
#else
    _mm_setcsr(_mm_getcsr() | 0x8040u);
#endif
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
