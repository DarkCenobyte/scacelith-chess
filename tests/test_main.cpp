#include "test.h"
#include <chrono>
#include <cstring>

namespace testing {
std::vector<Case>& registry() { static std::vector<Case> r; return r; }
int g_failures = 0;
}  // namespace testing

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failedCases = 0;
    for (auto& c : testing::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        int before = testing::g_failures;
        auto t0 = std::chrono::steady_clock::now();
        c.fn();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        bool ok = testing::g_failures == before;
        std::fprintf(stderr, "[%s] %s (%.1f ms)\n", ok ? " OK " : "FAIL", c.name, ms);
        ++run;
        if (!ok) ++failedCases;
    }
    std::fprintf(stderr, "%d test(s), %d failed\n", run, failedCases);
    return failedCases ? 1 : 0;
}
