// Minimal unit test framework. Add tests in any tests/*.cpp:
//   TEST(chess_perft_startpos) { CHECK_EQ(pos.perft(3), 8902u); }
// Run: build/scacelith_tests [--require=WHAT[,WHAT...]] [filter-substring]
// CHECK and CHECK_EQ record a failure and go on; REQUIRE ends the test when it fails (a check the
// rest of the test relies on), and SKIP(why) ends it when a prerequisite is missing (the model
// files, a live server, an opt-in variable): it is reported [SKIP] instead of [ OK ]. REQUIRE and
// SKIP return from the function they are in, so they go in TEST bodies only.
// A prerequisite a run can make mandatory has a name (WHAT: "tts-model", the voice model's files
// third_party/supertonic3 prepares in <build>/coach/): SKIP_WITHOUT(WHAT, why) skips like SKIP,
// and testing::uses(WHAT) records that the test has it. A run started with --require=WHAT (or with
// SCACELITH_TESTS_REQUIRE=WHAT in the environment) fails a test that skips without it, and fails
// when no test passed with it; it lists the tests that did (the CI checks the reference ones).
#pragma once
#include <cstdio>
#include <functional>
#include <type_traits>
#include <string>
#include <vector>

namespace testing {
struct Case { const char* name; std::function<void()> fn; };
std::vector<Case>& registry();
extern int g_failures;
extern std::string g_skip;  // why the running test was skipped, "" when it was not
extern std::string g_skipWithout;          // the prerequisite it was skipped without (SKIP_WITHOUT)
extern std::vector<std::string> g_uses;    // the prerequisites it has (uses)
inline void uses(const std::string& what) { g_uses.push_back(what); }
struct Registrar { Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
}  // namespace testing

#define TEST(NAME)                                                        \
    static void test_##NAME();                                            \
    static testing::Registrar reg_##NAME(#NAME, test_##NAME);             \
    static void test_##NAME()

#define CHECK(COND)                                                                          \
    do {                                                                                     \
        if (!(COND)) {                                                                       \
            std::fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #COND); \
            ++testing::g_failures;                                                           \
        }                                                                                    \
    } while (0)

#define CHECK_EQ(A, B)                                                                                  \
    do {                                                                                                \
        auto scacelith_check_lhs_ = (A);                                                                \
        auto scacelith_check_rhs_ = (B);                                                                \
        if (!(scacelith_check_lhs_ == scacelith_check_rhs_)) {                                          \
            std::fprintf(stderr, "  %s:%d: CHECK_EQ(%s, %s) failed: %s vs %s\n", __FILE__, __LINE__, #A, #B, \
                         testing::str(scacelith_check_lhs_).c_str(),                                    \
                         testing::str(scacelith_check_rhs_).c_str());                                   \
            ++testing::g_failures;                                                                      \
        }                                                                                               \
    } while (0)

#define REQUIRE(COND)                                                                          \
    do {                                                                                       \
        if (!(COND)) {                                                                         \
            std::fprintf(stderr, "  %s:%d: REQUIRE(%s) failed\n", __FILE__, __LINE__, #COND); \
            ++testing::g_failures;                                                             \
            return;                                                                            \
        }                                                                                      \
    } while (0)

#define SKIP(WHY)                   \
    do {                            \
        testing::g_skip = (WHY);    \
        return;                     \
    } while (0)

#define SKIP_WITHOUT(WHAT, WHY)            \
    do {                                   \
        testing::g_skipWithout = (WHAT);   \
        testing::g_skip = (WHY);           \
        return;                            \
    } while (0)

namespace testing {
inline std::string str(const std::string& s) { return "\"" + s + "\""; }
inline std::string str(const char* s) { return str(std::string(s)); }
template <class T> std::string str(const T& v) {
    if constexpr (std::is_same_v<T, bool>) {
        return v ? "true" : "false";
    } else if constexpr (std::is_enum_v<T>) {
        return str(static_cast<std::underlying_type_t<T>>(v));
    } else if constexpr (std::is_floating_point_v<T>) {
        char b[32];
        std::snprintf(b, sizeof b, "%.17g", double(v));
        return b;
    } else if constexpr (std::is_unsigned_v<T>) {
        return std::to_string((unsigned long long)v);
    } else if constexpr (std::is_integral_v<T>) {
        return std::to_string((long long)v);
    } else {
        return "<value>";
    }
}
}  // namespace testing
