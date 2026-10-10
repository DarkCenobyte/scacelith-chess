#include "net_sys.h"
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#else
#include <cerrno>
#include <climits>
#include <csignal>
#include <filesystem>
#include <system_error>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <cctype>
#include <cstring>
#include <mach-o/dyld.h>
#include <pwd.h>
#endif
extern char** environ;
#endif

namespace net {
namespace sys {

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}

namespace {
std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
}  // namespace

std::wstring moduleFileName(const std::function<unsigned long(wchar_t*, unsigned long)>& get) {
    // A path of MAX_PATH characters or more (long paths enabled) is read again into a larger
    // buffer, up to the 32767 characters of the longest path.
    std::wstring w(MAX_PATH, L'\0');
    DWORD n = get(&w[0], DWORD(w.size()));
    while (n >= w.size() && w.size() <= 32767) {
        w.resize(w.size() * 2);
        n = get(&w[0], DWORD(w.size()));
    }
    if (n == 0 || n >= w.size()) return std::wstring();
    w.resize(n);
    return w;
}

std::string exeDirectory() {
    std::wstring w = moduleFileName([](wchar_t* buffer, unsigned long size) {
        return GetModuleFileNameW(nullptr, buffer, size);
    });
    if (w.empty()) return ".\\";
    std::string s = narrow(w.c_str());
    size_t p = s.find_last_of("\\/");
    return p == std::string::npos ? std::string(".\\") : s.substr(0, p + 1);
}

std::string userDataDirectory() {
    wchar_t w[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, w))) {
        std::wstring dir = std::wstring(w) + L"\\scacelith";
        CreateDirectoryW(dir.c_str(), nullptr);
        return narrow(dir.c_str()) + "\\";
    }
    return exeDirectory();
}

std::string appDataDirectory() { return userDataDirectory(); }   // Roaming

bool fileExists(const std::string& path) {
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool directoryWritable(const std::string& dir) {
    std::wstring probe = widen(dir + "scacelith-write-test.tmp");
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

bool readFile(const std::string& path, std::string& out, size_t maxBytes) {
    FILE* f = _wfopen(widen(path).c_str(), L"rb");
    if (!f) return false;
    out.clear();
    char buf[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (out.size() + n > maxBytes) { ok = false; break; }
        out.append(buf, n);
    }
    fclose(f);
    return ok;
}

// Replaces 'to' with 'from'. Windows refuses to replace a file that is open, even for a moment:
// the game reading it on another thread, an antivirus scan, a backup tool. Such a refusal is tried
// again for up to a second before the replace fails.
static bool replaceFile(const std::wstring& from, const std::wstring& to) {
    const ULONGLONG start = GetTickCount64();
    DWORD wait = 2;
    for (;;) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        DWORD err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED && err != ERROR_LOCK_VIOLATION) return false;
        if (GetTickCount64() - start >= 1000) return false;
        Sleep(wait);
        wait = wait < 64 ? wait * 2 : 64;
    }
}

bool writeFileAtomic(const std::string& path, const std::string& data, bool) {
    std::wstring tmp = widen(path + ".tmp"), dst = widen(path);
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fflush(f) == 0 && ok;
    // The data on the disk before the rename, as fsync() on POSIX (MOVEFILE_WRITE_THROUGH covers the
    // rename only). Best effort: a file system that cannot flush (some network drives) does not fail
    // the save.
    if (ok) _commit(_fileno(f));
    fclose(f);
    if (ok) ok = replaceFile(tmp, dst);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

bool removeFile(const std::string& path) { return DeleteFileW(widen(path).c_str()) != 0; }

bool openBrowser(const std::string& url) {
    if (url.compare(0, 8, "https://") != 0 && url.compare(0, 7, "http://") != 0) return false;
    HINSTANCE r = ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

std::FILE* openFile(const std::string& path, const char* mode) { return _wfopen(widen(path).c_str(), widen(mode).c_str()); }

bool fileSize(const std::string& path, uint64_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    size = uint64_t(d.nFileSizeHigh) << 32 | d.nFileSizeLow;
    return true;
}

bool renameFile(const std::string& from, const std::string& to) { return replaceFile(widen(from), widen(to)); }

bool directoryExists(const std::string& dir) {
    DWORD a = GetFileAttributesW(widen(dir).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool makeDirectories(const std::string& dir) {
    if (dir.empty()) return false;
    if (directoryExists(dir)) return true;
    std::string d = dir;
    while (d.size() > 1 && (d.back() == '\\' || d.back() == '/')) d.pop_back();
    size_t cut = d.find_last_of("\\/");
    if (cut != std::string::npos && cut > 0 && d[cut - 1] != ':') makeDirectories(d.substr(0, cut));
    return CreateDirectoryW(widen(d).c_str(), nullptr) != 0 || directoryExists(d);
}

#else  // POSIX (Linux, macOS)

#ifdef __APPLE__
std::string exeDirectory() {
    // The executable's path as dyld knows it (maybe through a symbolic link, or with "." parts),
    // resolved to its real path.
    std::string path(PATH_MAX, '\0');
    uint32_t size = uint32_t(path.size());
    if (_NSGetExecutablePath(&path[0], &size) != 0) {   // too small: size is the length needed
        path.assign(size_t(size) + 1, '\0');
        if (_NSGetExecutablePath(&path[0], &size) != 0) return "./";
    }
    path.resize(std::strlen(path.c_str()));
    char real[PATH_MAX];
    if (realpath(path.c_str(), real)) path = real;
    const size_t cut = path.find_last_of('/');
    return cut == std::string::npos ? std::string("./") : path.substr(0, cut + 1);
}
#else
std::string exeDirectory() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "./";
    buf[n] = 0;
    std::string s(buf);
    return s.substr(0, s.find_last_of('/') + 1);
}
#endif

// A per-user folder from the environment (just created), resolved once to its canonical path
// (weakly_canonical: absolute, no "." or ".." parts, no symbolic link where it exists), the one the
// game logs and shows. One that could not be created stays where it was asked for: writing there
// fails, as before.
static std::string canonicalDirectory(const std::string& dir) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::weakly_canonical(dir, ec);
    if (ec) p = std::filesystem::path(dir).lexically_normal();
    std::string d = p.string();
    if (d.empty() || d.back() != '/') d += '/';
    return d;
}

#ifdef __APPLE__
// macOS: one folder for the settings, the saved logins, the log and the game's data files, as
// Windows has: ~/Library/Application Support/scacelith/, private (0700) as on Linux. The home
// folder is $HOME, else the user database's.
std::string userDataDirectory() {
    std::string base;
    if (const char* home = getenv("HOME"); home && home[0] == '/') {
        base = home;
    } else {
        passwd pw{};
        passwd* found = nullptr;
        char buf[4096];
        if (getpwuid_r(getuid(), &pw, buf, sizeof buf, &found) == 0 && found && found->pw_dir && found->pw_dir[0] == '/')
            base = found->pw_dir;
    }
    if (base.empty()) return exeDirectory();
    if (base.back() != '/') base += '/';
    base += "Library/Application Support/";
    makeDirectories(base);
    const std::string d = base + "scacelith/";
    mkdir(d.c_str(), 0700);
    return canonicalDirectory(d);
}

std::string appDataDirectory() { return userDataDirectory(); }
#else
std::string userDataDirectory() {
    // The XDG base directory rule: $XDG_CONFIG_HOME when it is an absolute path (a relative one is
    // ignored), else ~/.config. Its scacelith folder is private (it holds the saved logins).
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    std::string base;
    if (xdg && xdg[0] == '/') base = xdg;
    else if (home && home[0]) base = std::string(home) + "/.config";
    else return exeDirectory();
    if (base.back() != '/') base += '/';
    makeDirectories(base);
    const std::string d = base + "scacelith/";
    mkdir(d.c_str(), 0700);
    return canonicalDirectory(d);
}

std::string appDataDirectory() {
    const char* xdg = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    std::string base;
    if (xdg && xdg[0] == '/') base = xdg;
    else if (home && home[0]) base = std::string(home) + "/.local/share";
    else return exeDirectory();
    const std::string d = base + (base.back() == '/' ? "" : "/") + "scacelith/";
    makeDirectories(d);
    return canonicalDirectory(d);
}
#endif

bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool directoryWritable(const std::string& dir) { return access(dir.c_str(), W_OK) == 0; }

bool readFile(const std::string& path, std::string& out, size_t maxBytes) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (out.size() + n > maxBytes) { ok = false; break; }
        out.append(buf, n);
    }
    fclose(f);
    return ok;
}

bool writeFileAtomic(const std::string& path, const std::string& data, bool privateFile) {
    std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, privateFile ? 0600 : 0644);
    if (fd < 0) return false;
    if (privateFile) fchmod(fd, 0600);  // an older tmp file may have had other permissions
    size_t done = 0;
    bool ok = true;
    while (done < data.size()) {
        ssize_t w = ::write(fd, data.data() + done, data.size() - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { ok = false; break; }
        done += size_t(w);
    }
    ok = fsync(fd) == 0 && ok;
    ::close(fd);
    if (ok) ok = rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) unlink(tmp.c_str());
    return ok;
}

bool removeFile(const std::string& path) { return unlink(path.c_str()) == 0; }

int spawnProgram(char* const argv[]) {
    if (!argv || !argv[0]) return -1;
    // SIGPIPE back to its default action in the child: an ignored signal stays ignored across exec,
    // and the game ignores it (main.cpp), which xdg-open's pipelines and the browser do not expect.
    posix_spawnattr_t attr;
    if (posix_spawnattr_init(&attr) != 0) return -1;
    sigset_t dfl;
    sigemptyset(&dfl);
    sigaddset(&dfl, SIGPIPE);
    pid_t pid = -1;
    int spawned = posix_spawnattr_setsigdefault(&attr, &dfl);
#ifdef __APPLE__
    // macOS has no SOCK_CLOEXEC nor accept4: the child gets the standard descriptors that are open
    // and nothing else (POSIX_SPAWN_CLOEXEC_DEFAULT), whatever the game opened without O_CLOEXEC.
    posix_spawn_file_actions_t actions;
    const bool haveActions = posix_spawn_file_actions_init(&actions) == 0;
    if (!haveActions) spawned = -1;
    for (int fd = 0; fd <= 2 && spawned == 0; ++fd)
        if (fcntl(fd, F_GETFD) != -1) spawned = posix_spawn_file_actions_addinherit_np(&actions, fd);
    if (spawned == 0) spawned = posix_spawnattr_setflags(&attr, short(POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_CLOEXEC_DEFAULT));
    if (spawned == 0) spawned = posix_spawnp(&pid, argv[0], &actions, &attr, argv, environ);
    if (haveActions) posix_spawn_file_actions_destroy(&actions);
#else
    if (spawned == 0) spawned = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF);
    if (spawned == 0) spawned = posix_spawnp(&pid, argv[0], nullptr, &attr, argv, environ);
#endif
    posix_spawnattr_destroy(&attr);
    return spawned == 0 ? int(pid) : -1;
}

bool openBrowser(const std::string& url) {
    if (url.compare(0, 8, "https://") != 0 && url.compare(0, 7, "http://") != 0) return false;
#ifdef __APPLE__
    char* argv[] = {const_cast<char*>("/usr/bin/open"), const_cast<char*>(url.c_str()), nullptr};
#else
    char* argv[] = {const_cast<char*>("xdg-open"), const_cast<char*>(url.c_str()), nullptr};
#endif
    const pid_t pid = spawnProgram(argv);
    if (pid < 0) return false;
    // xdg-open returns quickly (it detaches the browser); reap it so no zombie stays behind.
    // open(1) returns once LaunchServices has the URL.
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::FILE* openFile(const std::string& path, const char* mode) {
    std::string m = std::string(mode) + "e";   // O_CLOEXEC
    return std::fopen(path.c_str(), m.c_str());
}

bool fileSize(const std::string& path, uint64_t& size) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    size = uint64_t(st.st_size);
    return true;
}

bool renameFile(const std::string& from, const std::string& to) { return rename(from.c_str(), to.c_str()) == 0; }

bool directoryExists(const std::string& dir) {
    struct stat st;
    return stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool makeDirectories(const std::string& dir) {
    if (dir.empty()) return false;
    if (directoryExists(dir)) return true;
    std::string d = dir;
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    size_t cut = d.find_last_of('/');
    if (cut != std::string::npos && cut > 0) makeDirectories(d.substr(0, cut));
    return mkdir(d.c_str(), 0755) == 0 || directoryExists(d);
}

#endif

#ifdef __APPLE__
bool insideAppBundle(const std::string& dir) {
    static const char kTail[] = ".app/Contents/MacOS/";
    const size_t n = sizeof(kTail) - 1;
    if (dir.size() <= n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)dir[dir.size() - n + i]) != std::tolower((unsigned char)kTail[i])) return false;
    return true;
}
#endif

std::string settingsDirectory() {
    const std::string exe = exeDirectory();
#ifdef __APPLE__
    // An application bundle is no portable install: Gatekeeper runs a quarantined app from a
    // random read-only copy (App Translocation), and an update replaces the whole bundle.
    if (insideAppBundle(exe)) return userDataDirectory();
#endif
    if (fileExists(exe + "Scacelith.ini")) return exe;   // portable
    const std::string data = userDataDirectory();
    return directoryWritable(data) ? data : exe;
}

}  // namespace sys
}  // namespace net
