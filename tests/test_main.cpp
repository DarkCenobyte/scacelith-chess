#include "test.h"
#include <chrono>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace testing {
std::vector<Case>& registry() { static std::vector<Case> r; return r; }
int g_failures = 0;
}  // namespace testing

namespace {

#ifdef _WIN32
// Whether this file system takes a file name with a letter outside ASCII, probed beside the
// executable (where the tests write their files). Windows file systems take any name, but Wine
// names the Linux files in the character set of the host locale (LC_ALL, LC_CTYPE, LANG): in the
// POSIX locale (LANG unset) it is ASCII, and every Win32 call that creates "Élodie-vs-...pgn"
// fails with ERROR_FILE_NOT_FOUND. The tests that save such a name (the saved games' archive) then
// fail for that reason alone: a note says so, at the start and next to the result. Nothing is
// skipped. nullptr when the name is taken, or when the folder takes no file at all.
bool canCreate(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

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
    const char* filter = argc > 1 ? argv[1] : nullptr;
    const char* note = nullptr;
#ifdef _WIN32
    note = nonAsciiNamesRefused();
#endif
    if (note) std::fprintf(stderr, "note: %s\n", note);
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
    if (note && failedCases) std::fprintf(stderr, "note: %s\n", note);
    return failedCases ? 1 : 0;
}
