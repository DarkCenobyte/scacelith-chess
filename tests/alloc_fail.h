// Running out of memory, for the tests: the test binary replaces the global operator new and
// operator delete (tests/alloc_fail.cpp) so that a test can make allocations throw std::bad_alloc,
// as they do when the system has no memory left, and count the large allocations a call makes.
// Off by default: every allocation is then malloc's, for one atomic load more.
#pragma once
#include <cstddef>

namespace allocfail {

// false in AddressSanitizer builds: the operators are not replaced there (tests/alloc_fail.cpp), the
// functions below have no effect, and the tests that need them skip.
bool available();

// From now on (0: no more), every allocation of at least 'bytes' throws std::bad_alloc on every
// thread that was not spared: the network threads of the client under test fail, the test's
// fake server (spareThisThread() in its handler) does not.
void failFrom(size_t bytes);
// The calling thread is never failed by failFrom() (until a Reset of its own goes).
void spareThisThread();
// The next allocation of the calling thread, whatever its size, throws std::bad_alloc (once).
void failNextHere();
// From now on (0: no more), the bytes of every allocation of at least 'bytes' that the calling
// thread makes are added up (countedHere()); the count of the calling thread starts at 0.
void countFrom(size_t bytes);
size_t countedHere();

// Everything above off when it goes, the calling thread no longer spared (a test that stops
// early leaves nothing armed).
struct Reset {
    Reset() = default;
    Reset(const Reset&) = delete;
    Reset& operator=(const Reset&) = delete;
    ~Reset();
};

}  // namespace allocfail
