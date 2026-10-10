#include "test.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace testing {
std::vector<Case>& registry() { static std::vector<Case> r; return r; }
int g_failures = 0;
std::string g_skip;
std::string g_skipWithout;
std::vector<std::string> g_uses;
}  // namespace testing

namespace {

#ifdef _WIN32
// Whether a file can be created at this path (the probe file is deleted on close).
bool canCreate(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// Whether this file system takes a file name with a letter outside ASCII, probed beside the
// executable (where the tests write their files). Windows file systems take any name, but Wine
// names the Linux files in the character set of the host locale (LC_ALL, LC_CTYPE, LANG): in the
// POSIX locale (LANG unset) it is ASCII, and every Win32 call that creates "Élodie-vs-...pgn"
// fails with ERROR_FILE_NOT_FOUND. The tests that save such a name (the saved games' archive) then
// fail for that reason alone: a note says so, at the start and next to the result. They are not
// skipped. nullptr when the name is taken, or when the folder takes no file at all.
const char* nonAsciiNamesRefused() {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return nullptr;
    std::wstring dir(exe, n);
    const size_t cut = dir.find_last_of(L"\\/");
    dir.resize(cut == std::wstring::npos ? 0 : cut + 1);
    const std::wstring probe = dir + L"scacelith-tests-name-probe-" + std::to_wstring(GetCurrentProcessId());
    if (!canCreate(probe + L".tmp")) return nullptr;
    if (canCreate(probe + L"-\u00C9.tmp")) return nullptr;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll && GetProcAddress(ntdll, "wine_get_version"))
        return "Wine runs in a host locale whose character set is not UTF-8 (ASCII when LANG is unset): "
               "file names with letters outside ASCII cannot be created, and the tests that save one fail. "
               "Run them in a UTF-8 locale: LC_ALL=C.UTF-8 (tools/test_win.sh does).";
    return "this file system refuses file names with letters outside ASCII: the tests that save one fail.";
}
#endif

}  // namespace

int main(int argc, char** argv) {
    // The prerequisites this run requires (SKIP_WITHOUT, testing::uses: test.h), from the
    // environment and the command line; the first other argument filters the tests by name.
    std::map<std::string, std::vector<std::string>> required;   // each one: the tests that passed with it
    auto require = [&](const std::string& list) {
        size_t at = 0;
        while (at <= list.size()) {
            size_t comma = list.find(',', at);
            if (comma == std::string::npos) comma = list.size();
            if (comma > at) required[list.substr(at, comma - at)];
            at = comma + 1;
        }
    };
    if (const char* env = std::getenv("SCACELITH_TESTS_REQUIRE")) require(env);
    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--require=", 0) == 0) {
            require(arg.substr(10));
        } else if (arg.rfind("--", 0) == 0) {
            std::fprintf(stderr, "unknown option %s (scacelith_tests [--require=WHAT[,WHAT...]] [filter-substring])\n",
                         argv[i]);
            return 2;
        } else if (!filter) {
            filter = argv[i];
        }
    }
    const char* note = nullptr;
#ifdef _WIN32
    note = nonAsciiNamesRefused();
#endif
    if (note) std::fprintf(stderr, "note: %s\n", note);
#ifndef _WIN32
    // Silent and deterministic on a desktop with a sound card: the null audio backend, unless the
    // run asks for another (SCACELITH_AUDIO=alsa ./scacelith_tests audio_live, to listen).
    setenv("SCACELITH_AUDIO", "null", 0);
#ifdef __APPLE__
    // macOS keeps no session in a file: the tests' sessions stay in a keyring in this process's
    // memory (credential_store.cpp), never in the user's keychain, whatever the environment says.
    setenv("SCACELITH_KEYRING", "memory", 1);
#else
    // The saved sessions of the tests stay in their temporary files, never in the keyring of the
    // desktop running them (the keyring tests give their stores a keyring themselves), whatever the
    // environment of the run says.
    setenv("SCACELITH_KEYRING", "off", 1);
#endif
#endif
    int run = 0, failedCases = 0, skipped = 0;
    for (auto& c : testing::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        int before = testing::g_failures;
        testing::g_skip.clear();
        testing::g_skipWithout.clear();
        testing::g_uses.clear();
        auto t0 = std::chrono::steady_clock::now();
        // An exception out of a test fails that test; the others still run.
        try {
            c.fn();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  uncaught exception: %s\n", e.what());
            ++testing::g_failures;
        } catch (...) {
            std::fprintf(stderr, "  uncaught exception\n");
            ++testing::g_failures;
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        bool ok = testing::g_failures == before;
        if (ok && !testing::g_skip.empty() && required.count(testing::g_skipWithout)) {
            // Skipped without a prerequisite this run requires: a failure.
            std::fprintf(stderr, "  %s is required (--require=%s): %s\n", testing::g_skipWithout.c_str(),
                         testing::g_skipWithout.c_str(), testing::g_skip.c_str());
            ok = false;
        }
        if (ok && !testing::g_skip.empty()) {
            std::fprintf(stderr, "[SKIP] %s (%s)\n", c.name, testing::g_skip.c_str());
            ++skipped;
        } else {
            std::fprintf(stderr, "[%s] %s (%.1f ms)\n", ok ? " OK " : "FAIL", c.name, ms);
        }
        if (ok && testing::g_skip.empty()) {
            for (const std::string& what : testing::g_uses) {
                auto it = required.find(what);
                if (it != required.end() && (it->second.empty() || it->second.back() != c.name)) it->second.push_back(c.name);
            }
        }
        ++run;
        if (!ok) ++failedCases;
    }
    if (skipped) std::fprintf(stderr, "%d test(s), %d failed, %d skipped\n", run, failedCases, skipped);
    else std::fprintf(stderr, "%d test(s), %d failed\n", run, failedCases);
    if (note && failedCases) std::fprintf(stderr, "note: %s\n", note);
    // A filter that names no test is a mistake (a typo), not a passing run.
    if (filter && run == 0) {
        std::fprintf(stderr, "no test name contains \"%s\"\n", filter);
        return 2;
    }
    // A required prerequisite no test passed with (none uses it, a typo, a filter that left them
    // out): the run did not check what it was asked to.
    bool unmet = false;
    for (const auto& r : required) {
        if (r.second.empty()) {
            std::fprintf(stderr, "required %s: no test passed with it\n", r.first.c_str());
            unmet = true;
            continue;
        }
        std::string names;
        for (const std::string& n : r.second) names += (names.empty() ? "" : ", ") + n;
        std::fprintf(stderr, "required %s: %zu test(s) passed with it: %s\n", r.first.c_str(), r.second.size(), names.c_str());
    }
    return failedCases || unmet ? 1 : 0;
}
