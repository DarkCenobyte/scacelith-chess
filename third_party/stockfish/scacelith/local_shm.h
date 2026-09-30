// Scacelith glue (not part of upstream Stockfish): replaces upstream src/shm.h in the embedded
// build. Force-included (-include) before every Stockfish source file, it defines shm.h's include
// guard, so upstream's #include "shm.h" (numa.h, engine.cpp) becomes a no-op and this
// process-local version of its public API is used instead.
//
// Upstream's SystemWideSharedConstant<T> first tries to share the unpacked network (115 MB) with
// the other processes running the same executable: on Linux a directory
// /tmp/stockfish-<uid>/sfshm_<hash>/ with a lock file and a Unix socket, a memfd, a socket-server
// thread and an atexit handler; on Windows a named file mapping ("Local\sf_...") and mutex. It
// falls back to a private large-page allocation only when that fails, and no option turns the
// sharing off. The game runs one engine per process, so here that private allocation (upstream's
// SharedMemoryBackendFallback) is the only backend: the network lives in process memory and the
// engine creates no file, socket, thread or named object for it.
//
// If upstream renames the guard or changes the class, both definitions meet and the build fails.
#ifndef SHM_H_INCLUDED
#define SHM_H_INCLUDED

// The standard headers upstream's shm.h and shm_unix.h include: numa.h relies on them (<variant>,
// <thread>, ...).
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "memory.h"
#include "misc.h"
#include "thread_native.h"
#include "types.h"

namespace Stockfish {

enum class SystemWideSharedConstantAllocationStatus {
    NoAllocation,
    LocalMemory,
    SharedMemory
};

template<typename T>
struct SystemWideSharedConstant {
    // Same requirements as upstream: the object is copied bytewise and never destroyed.
    static_assert(std::is_trivially_destructible_v<T>);
    static_assert(std::is_trivially_move_constructible_v<T>);
    static_assert(std::is_trivially_copy_constructible_v<T>);

    SystemWideSharedConstant() = default;

    // The discriminator tells upstream's shared copies of different NUMA nodes apart; a private
    // copy needs none.
    SystemWideSharedConstant(const T& value, [[maybe_unused]] usize discriminator = 0) :
        object(make_unique_large_page<T>(value)) {}

    SystemWideSharedConstant(const SystemWideSharedConstant&)            = delete;
    SystemWideSharedConstant& operator=(const SystemWideSharedConstant&) = delete;
    SystemWideSharedConstant(SystemWideSharedConstant&&) noexcept            = default;
    SystemWideSharedConstant& operator=(SystemWideSharedConstant&&) noexcept = default;

    const T& operator*() const { return *object; }

    bool operator==(std::nullptr_t) const noexcept { return object == nullptr; }

    bool operator!=(std::nullptr_t) const noexcept { return object != nullptr; }

    SystemWideSharedConstantAllocationStatus get_status() const {
        return object ? SystemWideSharedConstantAllocationStatus::LocalMemory
                      : SystemWideSharedConstantAllocationStatus::NoAllocation;
    }

    std::optional<std::string> get_error_message() const {
        if (!object)
            return "Not initialized";
        return std::nullopt;
    }

   private:
    LargePagePtr<T> object;
};

}  // namespace Stockfish

#endif  // SHM_H_INCLUDED
