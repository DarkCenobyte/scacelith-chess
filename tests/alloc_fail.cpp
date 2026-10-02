// The global operator new / delete of the test binary, with the failures and counts of
// tests/alloc_fail.h. Nothing armed (the usual case): malloc and free, after one atomic load.
// Replaced for the whole binary, so it serves every test; the aligned forms keep the library's.
// Not under AddressSanitizer: it would pair ASan's nothrow new (std::get_temporary_buffer) with
// free(), a false alloc-dealloc-mismatch, and hide ASan's own new / delete mismatch checks.
#include "alloc_fail.h"

#include <atomic>
#include <cstdlib>
#include <new>

#if defined(__SANITIZE_ADDRESS__)
#define SCACELITH_ALLOCFAIL_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SCACELITH_ALLOCFAIL_ASAN 1
#endif
#endif

namespace allocfail {

#if defined(SCACELITH_ALLOCFAIL_ASAN)
bool available() { return false; }
#else
bool available() { return true; }
#endif

namespace {

std::atomic<size_t> gFailFrom{0}, gCountFrom{0};
std::atomic<int> gFailNext{0};        // threads with a failNextHere() still to come
std::atomic<bool> gArmed{false};      // any of the three above
thread_local bool tSpared = false, tFailNext = false;
thread_local size_t tCounted = 0;

void rearm() { gArmed.store(gFailFrom.load() != 0 || gCountFrom.load() != 0 || gFailNext.load() != 0); }

void check(size_t n) {
    if (tFailNext) {
        tFailNext = false;
        gFailNext.fetch_sub(1);
        rearm();
        throw std::bad_alloc();
    }
    const size_t fail = gFailFrom.load();
    if (fail != 0 && n >= fail && !tSpared) throw std::bad_alloc();
    const size_t count = gCountFrom.load();
    if (count != 0 && n >= count) tCounted += n;
}

}  // namespace

void failFrom(size_t bytes) {
    gFailFrom.store(bytes);
    rearm();
}

void spareThisThread() { tSpared = true; }

void failNextHere() {
    if (!tFailNext) {
        tFailNext = true;
        gFailNext.fetch_add(1);
    }
    rearm();
}

void countFrom(size_t bytes) {
    tCounted = 0;
    gCountFrom.store(bytes);
    rearm();
}

size_t countedHere() { return tCounted; }

Reset::~Reset() {
    tSpared = false;
    if (tFailNext) {
        tFailNext = false;
        gFailNext.fetch_sub(1);
    }
    gFailFrom.store(0);
    gCountFrom.store(0);
    rearm();
}

}  // namespace allocfail

#if !defined(SCACELITH_ALLOCFAIL_ASAN)
void* operator new(std::size_t n) {
    if (allocfail::gArmed.load(std::memory_order_relaxed)) allocfail::check(n);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif
