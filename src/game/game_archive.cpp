// Saved games folder (see game_archive.h): file names, atomic saving, the cached listing, loading
// and removal. The file system calls are the platform's own (wide-character paths on Windows,
// POSIX elsewhere) so that UTF-8 names work everywhere.
#include "game_archive.h"
#include "../core/log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace game {
namespace archive {

using chess::pgn::Record;
using chess::pgn::Tag;

namespace {

// ---- File system ----------------------------------------------------------------------------------
struct FileInfo {
    std::string name;
    uint64_t size = 0;
    int64_t timeMs = 0;
};

enum class Place { Ok, Exists, Failed };

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}
std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
int64_t fileTimeMs(const FILETIME& ft) {
    const int64_t t = (int64_t(ft.dwHighDateTime) << 32) | int64_t(ft.dwLowDateTime);
    return (t - 116444736000000000LL) / 10000;  // 100 ns since 1601 -> ms since 1970
}
std::string lastError() { return "error " + std::to_string(unsigned(GetLastError())); }

// 0 = listed, 1 = the folder does not exist, 2 = it cannot be read.
int listFolder(const std::string& dir, std::vector<FileInfo>& out, std::string& err) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(widen(joinPath(dir, "*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return 1;
        err = "cannot read the folder (" + lastError() + ")";
        return 2;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        FileInfo f;
        f.name = narrow(fd.cFileName);
        f.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        f.timeMs = fileTimeMs(fd.ftLastWriteTime);
        out.push_back(f);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}
bool statFile(const std::string& path, FileInfo& f) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &d)) return false;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    f.size = (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    f.timeMs = fileTimeMs(d.ftLastWriteTime);
    return true;
}
FILE* openFile(const std::string& path, const char* mode) {
    return _wfopen(widen(path).c_str(), mode[0] == 'r' ? L"rb" : L"wb");
}
bool makeDir(const std::string& path) {
    return CreateDirectoryW(widen(path).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}
bool dirExists(const std::string& path) {
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool flushToDisk(FILE* f) { return fflush(f) == 0; }
Place placeNew(const std::string& tmp, const std::string& dst, std::string& err) {
    // Without MOVEFILE_REPLACE_EXISTING the move fails when the name is taken: never a replacement.
    if (MoveFileExW(widen(tmp).c_str(), widen(dst).c_str(), MOVEFILE_WRITE_THROUGH)) return Place::Ok;
    DWORD e = GetLastError();
    if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) return Place::Exists;
    err = "cannot name the file (" + lastError() + ")";
    return Place::Failed;
}
bool deleteFile(const std::string& path) { return DeleteFileW(widen(path).c_str()) != 0; }
unsigned processId() { return unsigned(GetCurrentProcessId()); }
bool localTime(std::time_t t, std::tm& out) { return localtime_s(&out, &t) == 0; }
const char kSeparator = '\\';
#else
int listFolder(const std::string& dir, std::vector<FileInfo>& out, std::string& err) {
    DIR* d = opendir(dir.empty() ? "." : dir.c_str());
    if (!d) {
        if (errno == ENOENT || errno == ENOTDIR) return 1;
        err = std::string("cannot read the folder (") + std::strerror(errno) + ")";
        return 2;
    }
    while (dirent* e = readdir(d)) {
        FileInfo f;
        f.name = e->d_name;
        struct stat st;
        if (stat(joinPath(dir, f.name).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        f.size = uint64_t(st.st_size);
        f.timeMs = int64_t(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
        out.push_back(f);
    }
    closedir(d);
    return 0;
}
bool statFile(const std::string& path, FileInfo& f) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    f.size = uint64_t(st.st_size);
    f.timeMs = int64_t(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
    return true;
}
FILE* openFile(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode[0] == 'r' ? "rb" : "wb"); }
bool makeDir(const std::string& path) { return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST; }
bool dirExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool flushToDisk(FILE* f) { return fflush(f) == 0 && fsync(fileno(f)) == 0; }
Place placeNew(const std::string& tmp, const std::string& dst, std::string& err) {
    // A hard link fails when the name is taken: never a replacement. File systems without hard
    // links (FAT, some network shares) get a rename after a check instead.
    if (link(tmp.c_str(), dst.c_str()) == 0) {
        unlink(tmp.c_str());
        return Place::Ok;
    }
    if (errno == EEXIST) return Place::Exists;
    if (errno == EPERM || errno == ENOTSUP || errno == EOPNOTSUPP || errno == ENOSYS || errno == EMLINK) {
        struct stat st;
        if (stat(dst.c_str(), &st) == 0) return Place::Exists;
        if (rename(tmp.c_str(), dst.c_str()) == 0) return Place::Ok;
    }
    err = std::string("cannot name the file (") + std::strerror(errno) + ")";
    return Place::Failed;
}
bool deleteFile(const std::string& path) { return unlink(path.c_str()) == 0; }
unsigned processId() { return unsigned(getpid()); }
bool localTime(std::time_t t, std::tm& out) { return localtime_r(&t, &out) != nullptr; }
const char kSeparator = '/';
#endif

bool isSeparator(char c) { return c == '/' || c == '\\'; }

// Creates the folder and its missing parents.
bool makeDirs(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && isSeparator(p.back())) p.pop_back();
    if (p.empty() || dirExists(p)) return true;
    size_t cut = p.size();
    while (cut > 0 && !isSeparator(p[cut - 1])) --cut;
    if (cut > 1 && !(cut == 3 && p[1] == ':')) makeDirs(p.substr(0, cut - 1));
    return makeDir(p);
}

bool readWhole(const std::string& path, std::string& out, size_t maxBytes, std::string& err) {
    FILE* f = openFile(path, "r");
    if (!f) {
        err = "cannot open the file";
        return false;
    }
    out.clear();
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (out.size() + n > maxBytes) {
            err = "the file is too large (more than " + std::to_string(maxBytes >> 20) + " MB)";
            ok = false;
            break;
        }
        out.append(buf, n);
    }
    if (ok && ferror(f)) {
        err = "cannot read the file";
        ok = false;
    }
    fclose(f);
    return ok;
}

bool hasPgnExtension(const std::string& name) {
    if (name.size() < 5) return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return ext == ".pgn";
}

std::string twoDigits(int v) {
    char b[8];
    std::snprintf(b, sizeof b, "%02d", v % 100);
    return b;
}

// ---- UTF-8 ---------------------------------------------------------------------------------------
// Next codepoint of s at i (advances i); invalid bytes give 0xFFFD.
uint32_t nextCodepoint(const std::string& s, size_t& i) {
    const unsigned char c = (unsigned char)s[i];
    int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (len == 0 || i + size_t(len) > s.size()) {
        ++i;
        return 0xFFFD;
    }
    uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k < len; ++k) {
        const unsigned char d = (unsigned char)s[i + size_t(k)];
        if ((d & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (d & 0x3F);
    }
    i += size_t(len);
    return cp;
}
void appendCodepoint(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

// ---- Listing cache --------------------------------------------------------------------------------
struct CachedFile {
    uint64_t size = 0;
    int64_t timeMs = 0;
    std::vector<Entry> games;
};
std::mutex g_cacheLock;
std::map<std::string, CachedFile> g_cache;  // by path

// The entries of one file, read and scanned.
std::vector<Entry> scanFile(const std::string& folder, const FileInfo& info) {
    std::vector<Entry> out;
    Entry base;
    base.path = joinPath(folder, info.name);
    base.file = info.name;
    base.fileSize = info.size;
    base.fileTimeMs = info.timeMs;
    const chess::pgn::Limits limits;
    std::string text, err;
    if (info.size > limits.maxBytes) {
        err = "the file is too large (more than " + std::to_string(limits.maxBytes >> 20) + " MB)";
    } else if (readWhole(base.path, text, limits.maxBytes, err)) {
        chess::pgn::Result<chess::pgn::Summary> sc = chess::pgn::scan(text, limits);
        if (!sc.error.empty()) err = sc.error;
        else if (sc.games.empty()) err = "no game in this file";
        if (sc.truncated) LOGW("archive: %s holds more than %d games, the rest is not listed", info.name.c_str(), limits.maxGames);
        for (size_t i = 0; err.empty() && i < sc.games.size(); ++i) {
            const chess::pgn::Summary& s = sc.games[i];
            Entry e = base;
            e.index = int(i);
            e.games = int(sc.games.size());
            e.offset = s.offset;
            e.length = s.length;
            e.line = s.line;
            e.column = s.column;
            e.tags = s.tags;
            e.plies = s.plies;
            e.result = s.result;
            e.mode = modeFromName(s.tag("ScacelithMode"));
            e.error = s.error.text();
            out.push_back(std::move(e));
        }
    }
    if (!err.empty()) {
        Entry e = base;
        e.error = err;
        e.fileError = true;
        e.result = "";
        out.push_back(std::move(e));
    }
    return out;
}

// "2026.10.01" with unknown parts as zeros, for sorting.
std::string sortableDate(const std::string& d) {
    std::string s = d;
    for (char& c : s)
        if (c == '?') c = '0';
    return s;
}

bool newerFirst(const Entry& a, const Entry& b) {
    const std::string da = sortableDate(a.date()), db = sortableDate(b.date());
    if (da != db) return da > db;
    const std::string ta = a.time(), tb = b.time();
    if (ta != tb) return ta > tb;
    if (a.fileTimeMs != b.fileTimeMs) return a.fileTimeMs > b.fileTimeMs;
    if (a.path != b.path) return a.path < b.path;
    return a.index < b.index;
}

int64_t roundTenth(int64_t ms) { return ms < 0 ? -1 : (ms + 50) / 100 * 100; }

}  // namespace

// ---- Modes ------------------------------------------------------------------------------------------

const char* modeName(Mode m) {
    switch (m) {
    case Mode::Play: return "play";
    case Mode::Coach: return "coach";
    case Mode::HotSeat: return "hotseat";
    case Mode::Direct: return "direct";
    case Mode::Server: return "server";
    case Mode::Watch: return "watch";
    case Mode::Imported: return "";
    }
    return "";
}

Mode modeFromName(const std::string& name) {
    for (Mode m : {Mode::Play, Mode::Coach, Mode::HotSeat, Mode::Direct, Mode::Server, Mode::Watch})
        if (name == modeName(m)) return m;
    return Mode::Imported;
}

bool shouldSave(Mode mode, int coachLevel, int plies, bool finished, bool enabled) {
    if (!enabled) return false;
    switch (mode) {
    case Mode::Play:
    case Mode::HotSeat:
    case Mode::Direct: break;
    case Mode::Coach:
        if (coachLevel < 1) return false;  // the rules lesson (or an unknown level)
        break;
    default: return false;                 // server games, the viewer mode, imported games
    }
    return plies > 0 || finished;
}

// ---- Records ----------------------------------------------------------------------------------------

Record makeRecord(const chess::Game& game, const GameInfo& info) {
    Record r = Record::fromGame(game);
    const std::time_t when = info.started ? info.started : std::time(nullptr);
    std::tm tm{};
    const bool haveTime = localTime(when, tm);
    std::string event = info.event;
    if (event.empty()) event = info.mode == Mode::Coach ? "Coach game" : info.mode == Mode::Direct ? "Direct match" : "Casual game";
    r.setTag("Event", event);
    r.setTag("Site", info.site.empty() ? std::string("Scacelith") : info.site);
    r.setTag("Date", haveTime ? std::to_string(tm.tm_year + 1900) + "." + twoDigits(tm.tm_mon + 1) + "." + twoDigits(tm.tm_mday)
                              : std::string("????.??.??"));
    r.setTag("Round", info.round > 0 ? std::to_string(info.round) : std::string("-"));
    r.setTag("White", info.white.empty() ? std::string("?") : info.white);
    r.setTag("Black", info.black.empty() ? std::string("?") : info.black);
    if (!info.result.empty()) {
        const std::string res = chess::pgn::normalizeResult(info.result);
        if (!res.empty()) r.result = res;
    }
    r.setTag("Result", r.result);
    r.setTag("TimeControl", info.timeControl.empty() ? std::string("?") : info.timeControl);
    std::string termination = info.termination;
    if (termination.empty()) {
        termination = r.result == "*" ? "unterminated"
                      : game.isOver() ? chess::pgn::terminationValue(game.status(), game.endReason())
                                      : "normal";
    }
    r.setTag("Termination", termination);
    if (!info.eco.empty()) r.setTag("ECO", info.eco);
    if (!info.opening.empty()) r.setTag("Opening", info.opening);
    if (info.whiteElo > 0) r.setTag("WhiteElo", std::to_string(info.whiteElo));
    if (info.blackElo > 0) r.setTag("BlackElo", std::to_string(info.blackElo));
    if (haveTime) r.setTag("Time", twoDigits(tm.tm_hour) + ":" + twoDigits(tm.tm_min) + ":" + twoDigits(tm.tm_sec));
    if (info.mode != Mode::Imported) r.setTag("ScacelithMode", modeName(info.mode));
    if (info.mode == Mode::Coach && info.coachLevel >= 0) r.setTag("CoachLevel", std::to_string(info.coachLevel));
    std::string end = info.endKey;
    if (end.empty() && game.isOver()) end = chess::endReasonKey(game.endReason());
    if (end.compare(0, 7, "reason.") == 0) end = end.substr(7);
    if (!end.empty() && r.result != "*") r.setTag("ScacelithEnd", end);
    for (size_t i = 0; i < r.plies.size(); ++i) {
        if (i < info.elapsedMs.size()) r.plies[i].elapsedMs = roundTenth(info.elapsedMs[i]);
        if (i < info.clockMs.size()) r.plies[i].clockMs = roundTenth(info.clockMs[i]);
    }
    return r;
}

// ---- Files ---------------------------------------------------------------------------------------------

std::string sanitizeName(const std::string& name, size_t maxBytes) {
    std::string out;
    size_t i = 0;
    while (i < name.size()) {
        uint32_t cp = nextCodepoint(name, i);
        if (cp < 0x20 || cp == 0x7F || cp == 0xFFFD || cp == 0xFEFF) continue;
        if ((cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2069)) continue;
        if (cp == ' ' || cp == 0xA0 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x3000 || std::strchr("<>:\"/\\|?*", int(cp)))
            cp = '_';
        if (cp == '_' && !out.empty() && out.back() == '_') continue;
        appendCodepoint(out, cp);
    }
    auto trimEnds = [](std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::strchr("._-", s[a])) ++a;
        while (b > a && std::strchr("._-", s[b - 1])) --b;
        s = s.substr(a, b - a);
    };
    trimEnds(out);
    if (out.size() > maxBytes) {
        size_t n = maxBytes;
        while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80) --n;
        out.resize(n);
        trimEnds(out);
    }
    // Windows device names, whatever the case and the extension ("con", "Com1.x").
    std::string base = out.substr(0, out.find('.'));
    for (char& c : base)
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    static const char* devices[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
    bool device = false;
    for (const char* d : devices) device = device || base == d;
    if ((base.size() == 4 && (base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0) && base[3] >= '0' && base[3] <= '9') ||
        (base.size() == 5 && (base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0) && base[3] == '\xC2' &&
         (base[4] == '\xB9' || base[4] == '\xB2' || base[4] == '\xB3')))
        device = true;
    if (device) out += '_';
    return out.empty() ? std::string("Unknown") : out;
}

std::string fileName(const Record& record, std::time_t when) {
    std::tm tm{};
    std::string stamp = "0000-00-00_000000";
    if (localTime(when, tm)) {
        char b[96];
        std::snprintf(b, sizeof b, "%04d-%02d-%02d_%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                      tm.tm_sec);
        stamp = b;
    }
    std::string mode = modeName(modeFromName(record.tag("ScacelithMode")));
    if (mode.empty()) mode = "game";
    return stamp + "_" + mode + "_" + sanitizeName(record.tag("White")) + "-vs-" + sanitizeName(record.tag("Black")) + ".pgn";
}

std::string joinPath(const std::string& folder, const std::string& name) {
    if (folder.empty()) return name;
    if (isSeparator(folder.back())) return folder + name;
    const char sep = folder.find('\\') != std::string::npos ? '\\' : folder.find('/') != std::string::npos ? '/' : kSeparator;
    return folder + sep + name;
}

bool makeFolder(const std::string& folder) { return !folder.empty() && makeDirs(folder); }

SaveResult save(const std::string& folder, const Record& record, std::time_t when) {
    SaveResult res;
    if (!makeDirs(folder)) {
        res.error = "cannot create the folder " + folder;
        return res;
    }
    const std::string text = chess::pgn::write(record);
    static std::atomic<unsigned> counter{0};
    const std::string tmp =
        joinPath(folder, ".scacelith-save-" + std::to_string(processId()) + "-" + std::to_string(counter++) + ".tmp");
    FILE* f = openFile(tmp, "w");
    if (!f) {
        res.error = "cannot write in the folder " + folder;
        return res;
    }
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = flushToDisk(f) && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        deleteFile(tmp);
        res.error = "cannot write the game (disk full?)";
        return res;
    }
    std::string name = fileName(record, when ? when : std::time(nullptr));
    const std::string stem = name.substr(0, name.size() - 4);
    for (int n = 1; n <= 99; ++n) {
        const std::string candidate = n == 1 ? name : stem + "_" + std::to_string(n) + ".pgn";
        const std::string path = joinPath(folder, candidate);
        std::string err;
        const Place p = placeNew(tmp, path, err);
        if (p == Place::Ok) {
            res.ok = true;
            res.path = path;
            LOGI("archive: game saved as %s", path.c_str());
            return res;
        }
        if (p == Place::Failed) {
            res.error = err;
            break;
        }
    }
    if (res.error.empty()) res.error = "too many files named " + name;
    deleteFile(tmp);
    return res;
}

// ---- Listing --------------------------------------------------------------------------------------------

std::string Entry::tag(const std::string& name, const std::string& fallback) const {
    for (const Tag& t : tags)
        if (t.name == name) return t.value;
    return fallback;
}

std::string Entry::date() const {
    std::string d = tag("Date");
    if (d.empty() || d.find_first_not_of("?.") == std::string::npos) {
        const std::string u = tag("UTCDate");
        if (!u.empty()) d = u;
    }
    return d;
}

std::string Entry::time() const {
    std::string t = tag("Time");
    if (t.empty()) t = tag("UTCTime");
    return t;
}

int Entry::coachLevel() const {
    const std::string v = tag("CoachLevel");
    if (v.empty() || v.size() > 3 || v.find_first_not_of("0123456789") != std::string::npos) return -1;
    return std::stoi(v);
}

std::vector<Entry> list(const std::string& folder, ListStats* stats) {
    ListStats st;
    std::vector<FileInfo> files;
    const int r = listFolder(folder, files, st.error);
    std::vector<Entry> out;
    std::lock_guard<std::mutex> lock(g_cacheLock);
    std::map<std::string, bool> seen;
    for (const FileInfo& f : files) {
        if (!hasPgnExtension(f.name) || f.name[0] == '.') continue;
        ++st.files;
        const std::string path = joinPath(folder, f.name);
        seen[path] = true;
        auto it = g_cache.find(path);
        if (it != g_cache.end() && it->second.size == f.size && it->second.timeMs == f.timeMs) {
            ++st.cached;
        } else {
            CachedFile c;
            c.size = f.size;
            c.timeMs = f.timeMs;
            c.games = scanFile(folder, f);
            g_cache[path] = std::move(c);
            it = g_cache.find(path);
            ++st.read;
        }
        out.insert(out.end(), it->second.games.begin(), it->second.games.end());
    }
    // Files of this folder that are gone.
    const std::string prefix = joinPath(folder, "");
    for (auto it = g_cache.begin(); it != g_cache.end();) {
        if (r != 2 && it->first.compare(0, prefix.size(), prefix) == 0 && !seen.count(it->first) &&
            it->first.find_first_of("/\\", prefix.size()) == std::string::npos)
            it = g_cache.erase(it);
        else
            ++it;
    }
    std::stable_sort(out.begin(), out.end(), newerFirst);
    if (r == 2) LOGW("archive: %s: %s", folder.c_str(), st.error.c_str());
    if (stats) *stats = st;
    return out;
}

void clearCache() {
    std::lock_guard<std::mutex> lock(g_cacheLock);
    g_cache.clear();
}

namespace {
LoadResult loadGame(const std::string& path, int index, const Entry* listed) {
    LoadResult res;
    FileInfo now;
    if (!statFile(path, now)) {
        res.error = "the file is gone";
        return res;
    }
    const chess::pgn::Limits limits;
    std::string text, err;
    if (!readWhole(path, text, limits.maxBytes, err)) {
        res.error = err;
        return res;
    }
    size_t offset = 0, length = text.size();
    chess::pgn::Origin origin;
    if (listed && listed->fileSize == now.size && listed->fileTimeMs == now.timeMs && listed->offset + listed->length <= text.size()) {
        offset = listed->offset;
        length = listed->length;
        origin = chess::pgn::Origin{listed->line, listed->column};
    } else {
        // Unknown or changed file: find the game by its index.
        chess::pgn::Result<chess::pgn::Summary> sc = chess::pgn::scan(text, limits);
        if (!sc.error.empty()) {
            res.error = sc.error;
            return res;
        }
        if (index < 0 || index >= int(sc.games.size())) {
            res.error = "the file has no game " + std::to_string(index + 1);
            return res;
        }
        const chess::pgn::Summary& s = sc.games[size_t(index)];
        offset = s.offset;
        length = s.length;
        origin = chess::pgn::Origin{s.line, s.column};
    }
    chess::pgn::Result<chess::pgn::ParsedGame> rd = chess::pgn::read(text.substr(offset, length), limits, origin);
    if (rd.games.empty()) {
        res.error = rd.error.empty() ? std::string("no game found") : rd.error;
        return res;
    }
    chess::pgn::ParsedGame& g = rd.games[0];
    res.record = std::move(g.record);
    res.ok = g.ok();
    res.error = g.error.text();
    return res;
}
}  // namespace

LoadResult load(const Entry& entry) {
    if (entry.fileError) {
        LoadResult res;
        res.error = entry.error;
        return res;
    }
    return loadGame(entry.path, entry.index, &entry);
}

LoadResult loadFile(const std::string& path, int index) { return loadGame(path, index, nullptr); }

RemoveResult remove(const Entry& entry) {
    RemoveResult res;
    if (entry.games != 1 || entry.fileError) {
        res.status = entry.fileError ? RemoveStatus::Failed : RemoveStatus::SeveralGames;
        res.error = entry.fileError ? "the file could not be read" : "the file holds " + std::to_string(entry.games) + " games";
        return res;
    }
    if (!hasPgnExtension(entry.path)) {
        res.error = "not a PGN file";
        return res;
    }
    FileInfo now;
    if (!statFile(entry.path, now)) {
        res.status = RemoveStatus::NotFound;
        res.error = "the file is gone";
        return res;
    }
    if (now.size != entry.fileSize || now.timeMs != entry.fileTimeMs) {
        res.status = RemoveStatus::Changed;
        res.error = "the file changed since it was listed";
        return res;
    }
    if (!deleteFile(entry.path)) {
        res.status = RemoveStatus::Failed;
        res.error = "the file could not be deleted";
        return res;
    }
    {
        std::lock_guard<std::mutex> lock(g_cacheLock);
        g_cache.erase(entry.path);
    }
    LOGI("archive: %s deleted", entry.path.c_str());
    res.status = RemoveStatus::Removed;
    return res;
}

}  // namespace archive
}  // namespace game
