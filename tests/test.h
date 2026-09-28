// Minimal unit test framework. Add tests in any tests/*.cpp:
//   TEST(chess_perft_startpos) { CHECK_EQ(pos.perft(3), 8902u); }
// Run: build/scacelith_tests [filter-substring]
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
        auto a_ = (A);                                                                                  \
        auto b_ = (B);                                                                                  \
        if (!(a_ == b_)) {                                                                              \
            std::fprintf(stderr, "  %s:%d: CHECK_EQ(%s, %s) failed: %s vs %s\n", __FILE__, __LINE__, #A, #B, \
                         testing::str(a_).c_str(), testing::str(b_).c_str());                           \
            ++testing::g_failures;                                                                      \
        }                                                                                               \
    } while (0)

namespace testing {
inline std::string str(const std::string& s) { return "\"" + s + "\""; }
inline std::string str(const char* s) { return str(std::string(s)); }
template <class T> std::string str(const T& v) {
    if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) return std::to_string((long long)v);
    else return "<value>";
}
}  // namespace testing
