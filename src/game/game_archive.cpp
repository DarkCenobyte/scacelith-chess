// Saved games folder (see game_archive.h): file names, atomic saving, the cached listing, loading,
// removal, and the games of an online server saved on request. The file system calls are the
// platform's own (wide-character paths on Windows, POSIX elsewhere) so that UTF-8 names work everywhere.
#include "game_archive.h"
#include "../core/log.h"
#include "../i18n/unicode.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#ifdef _WIN32
#include <io.h>
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
// fflush only hands the data to the system: _commit (FlushFileBuffers) puts it on the disk before
// the rename makes it the game's file, so a power cut never leaves an empty file under that name.
bool flushToDisk(FILE* f) { return fflush(f) == 0 && _commit(_fileno(f)) == 0; }
Place placeNew(const std::string& tmp, const std::string& dst, std::string& err) {
    // Without MOVEFILE_REPLACE_EXISTING the move fails when the name is taken: never a replacement.
    // An antivirus or the search indexer may hold the file just written for a few milliseconds: the
    // move is tried again meanwhile (10, 20, 40 and 80 ms later) before the save gives up.
    const std::wstring from = widen(tmp), to = widen(dst);
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return Place::Ok;
        DWORD e = GetLastError();
        if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) return Place::Exists;
        const bool held = e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION;
        if (!held || attempt == 4) break;
        Sleep(DWORD(10) << attempt);
    }
    err = "cannot name the file (" + lastError() + ")";
    return Place::Failed;
}
bool deleteFile(const std::string& path) { return DeleteFileW(widen(path).c_str()) != 0; }
unsigned processId() { return unsigned(GetCurrentProcessId()); }
const char kSeparator = '\\';
#else
// The modification time in milliseconds (macOS has st_mtimespec for POSIX's st_mtim).
static int64_t modifiedMs(const struct stat& st) {
#ifdef __APPLE__
    return int64_t(st.st_mtimespec.tv_sec) * 1000 + st.st_mtimespec.tv_nsec / 1000000;
#else
    return int64_t(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
#endif
}
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
        f.timeMs = modifiedMs(st);
        out.push_back(f);
    }
    closedir(d);
    return 0;
}
bool statFile(const std::string& path, FileInfo& f) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    f.size = uint64_t(st.st_size);
    f.timeMs = modifiedMs(st);
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

// 'length' bytes of the file from 'offset' (one game of a listed file).
bool readSlice(const std::string& path, size_t offset, size_t length, std::string& out, std::string& err) {
    FILE* f = openFile(path, "r");
    if (!f) {
        err = "cannot open the file";
        return false;
    }
    out.assign(length, '\0');
    // Offsets stay under pgn::Limits::maxBytes (32 MB): a long is wide enough everywhere.
    const bool ok = std::fseek(f, long(offset), SEEK_SET) == 0 && (length == 0 || fread(&out[0], 1, length, f) == length);
    fclose(f);
    if (!ok) err = "cannot read the file";
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

// ---- Listing cache --------------------------------------------------------------------------------
struct CachedFile {
    uint64_t size = 0;
    int64_t timeMs = 0;
    std::vector<Entry> games;
};
struct Cache {
    std::mutex lock;
    std::map<std::string, std::shared_ptr<const CachedFile>> files;  // by path
};
// Never destroyed: the library's listing worker can still be inside list() while the program's
// static objects are destroyed at exit.
Cache& cache() {
    static Cache* c = new Cache();
    return *c;
}

// The tags an Entry keeps (what the listing shows and sorts by); the others come with load().
bool listedTag(const std::string& name) {
    static const char* const names[] = {"Event",       "Site",     "Date",     "Round",       "White",     "Black",
                                        "Result",      "UTCDate",  "UTCTime",  "Time",        "WhiteElo",  "BlackElo",
                                        "TimeControl", "ECO",      "Opening",  "Variation",   "Termination", "ScacelithMode",
                                        "CoachLevel",  "ScacelithEnd", "ScacelithServer", "ScacelithGameId"};
    for (const char* n : names)
        if (name == n) return true;
    return false;
}

// The entries of one file, read and scanned.
std::vector<Entry> scanFile(const std::string& folder, const FileInfo& info) {
    std::vector<Entry> out;
    Entry base;
    base.path = joinPath(folder, info.name);
    base.file = info.name;
    base.fileSize = info.size;
    base.fileTimeMs = info.timeMs;
    chess::pgn::Limits limits;
    limits.summaryTag = &listedTag;
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
            for (const Tag& t : s.tags)  // without the reader's own Variant, SetUp and FEN
                if (listedTag(t.name)) e.tags.push_back(t);
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

// The listing's sort keys of an entry, found once (its tags are scanned for them).
struct SortKey {
    std::string date, time;  // sortableDate(date()), time()
};

bool newerFirst(const Entry& a, const SortKey& ka, const Entry& b, const SortKey& kb) {
    if (ka.date != kb.date) return ka.date > kb.date;
    if (ka.time != kb.time) return ka.time > kb.time;
    if (a.fileTimeMs != b.fileTimeMs) return a.fileTimeMs > b.fileTimeMs;
    if (a.path != b.path) return a.path < b.path;
    return a.index < b.index;
}

int64_t roundTenth(int64_t ms) { return ms < 0 ? -1 : (ms + 50) / 100 * 100; }

// The PGN Termination of an ending given by its i18n key (a direct match ended by the authority,
// whose local Game never ends).
const char* terminationOfKey(const std::string& key) {
    if (key == "reason.timeout" || key == "reason.timeout_vs_insufficient") return "time forfeit";
    if (key == "reason.illegal_moves" || key == "reason.illegal_vs_insufficient" || key == "reason.online.forfeit")
        return "rules infraction";
    const std::string abandonment = "reason.online.abandonment";  // and its "_vs_insufficient" draw
    if (key.compare(0, abandonment.size(), abandonment) == 0 || key == "reason.online.no_show" ||
        key == "reason.online.both_disconnected")
        return "abandoned";
    if (key == "reason.online.aborted" || key == "reason.online.server_aborted") return "unterminated";
    return "normal";
}

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
        termination = r.result == "*"          ? "unterminated"
                      : !info.endKey.empty()   ? terminationOfKey(info.endKey)
                      : game.isOver()          ? chess::pgn::terminationValue(game.status(), game.endReason())
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
    // Times are matched to plies by index: a vector longer than the game (moves taken back and
    // not trimmed from it) would put the times of undone moves on the moves played instead.
    const bool elapsedFit = info.elapsedMs.size() <= r.plies.size(), clockFit = info.clockMs.size() <= r.plies.size();
    if (!elapsedFit || !clockFit)
        LOGW("archive: %d/%d move times for %d plies: left out", int(info.elapsedMs.size()), int(info.clockMs.size()), int(r.plies.size()));
    for (size_t i = 0; i < r.plies.size(); ++i) {
        if (elapsedFit && i < info.elapsedMs.size()) r.plies[i].elapsedMs = roundTenth(info.elapsedMs[i]);
        if (clockFit && i < info.clockMs.size()) r.plies[i].clockMs = roundTenth(info.clockMs[i]);
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
        // The characters Windows refuses in a name are ASCII (strchr alone would test only the low
        // byte of the codepoint, and match its terminating NUL).
        if (cp == ' ' || cp == 0xA0 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x3000 ||
            (cp < 0x80 && std::strchr("<>:\"/\\|?*", int(cp))))
            cp = '_';
        if (cp == '_' && !out.empty() && out.back() == '_') continue;
        uni::append(out, cp);
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

SaveResult saveFile(const std::string& folder, const std::string& name, const std::string& text) {
    SaveResult res;
    if (!makeDirs(folder)) {
        res.error = "cannot create the folder " + folder;
        return res;
    }
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
        res.error = "cannot write the file (disk full?)";
        return res;
    }
    // "name_2.ext" before the extension (a name without one gets the suffix at its end).
    const size_t dot = name.find_last_of('.');
    const bool hasExt = dot != std::string::npos && dot > 0;
    const std::string stem = hasExt ? name.substr(0, dot) : name, ext = hasExt ? name.substr(dot) : std::string();
    for (int n = 1; n <= 99; ++n) {
        const std::string candidate = n == 1 ? name : stem + "_" + std::to_string(n) + ext;
        const std::string path = joinPath(folder, candidate);
        std::string err;
        const Place p = placeNew(tmp, path, err);
        if (p == Place::Ok) {
            res.ok = true;
            res.path = path;
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

SaveResult save(const std::string& folder, const Record& record, std::time_t when) {
    SaveResult res = saveFile(folder, fileName(record, when ? when : std::time(nullptr)), chess::pgn::write(record));
    if (res.ok) LOGI("archive: game saved as %s", res.path.c_str());
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

std::vector<Entry> list(const std::string& folder, ListStats* stats, const std::atomic<bool>* cancel) {
    ListStats st;
    std::vector<FileInfo> files;
    const int r = listFolder(folder, files, st.error);
    files.erase(std::remove_if(files.begin(), files.end(), [](const FileInfo& f) { return !hasPgnExtension(f.name) || f.name[0] == '.'; }),
                files.end());
    st.files = int(files.size());
    // The files written last first: when the folder holds more than kMaxListed games, the oldest
    // files are the ones left out.
    std::sort(files.begin(), files.end(), [](const FileInfo& a, const FileInfo& b) {
        return a.timeMs != b.timeMs ? a.timeMs > b.timeMs : a.name < b.name;
    });
    Cache& c = cache();
    std::vector<std::shared_ptr<const CachedFile>> parts;
    std::set<std::string> seen;
    size_t total = 0;
    for (const FileInfo& f : files) {
        if (cancel && cancel->load()) {
            st.cancelled = true;
            break;
        }
        if (total >= size_t(kMaxListed)) {
            st.truncated = true;
            break;
        }
        const std::string path = joinPath(folder, f.name);
        seen.insert(path);
        std::shared_ptr<const CachedFile> part;
        {
            std::lock_guard<std::mutex> lock(c.lock);
            auto it = c.files.find(path);
            if (it != c.files.end() && it->second->size == f.size && it->second->timeMs == f.timeMs) part = it->second;
        }
        if (part) {
            ++st.cached;
        } else {
            // Read and scanned without the lock: a slow file never holds up remove() on the UI thread.
            auto fresh = std::make_shared<CachedFile>();
            fresh->size = f.size;
            fresh->timeMs = f.timeMs;
            fresh->games = scanFile(folder, f);
            part = fresh;
            std::lock_guard<std::mutex> lock(c.lock);
            c.files[path] = part;
            ++st.read;
        }
        total += part->games.size();
        parts.push_back(std::move(part));
    }
    if (!st.cancelled) {
        // Files of this folder that are gone (or left out): forgotten.
        const std::string prefix = joinPath(folder, "");
        std::lock_guard<std::mutex> lock(c.lock);
        for (auto it = c.files.begin(); it != c.files.end();) {
            if (r != 2 && it->first.compare(0, prefix.size(), prefix) == 0 && !seen.count(it->first) &&
                it->first.find_first_of("/\\", prefix.size()) == std::string::npos)
                it = c.files.erase(it);
            else
                ++it;
        }
    }
    std::vector<Entry> out;
    out.reserve(std::min(total, size_t(kMaxListed)));
    for (const auto& part : parts) {
        const size_t room = size_t(kMaxListed) - out.size();
        if (part->games.size() > room) st.truncated = true;
        out.insert(out.end(), part->games.begin(), part->games.begin() + long(std::min(room, part->games.size())));
    }
    std::vector<SortKey> keys(out.size());
    std::vector<size_t> order(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        keys[i].date = sortableDate(out[i].date());
        keys[i].time = out[i].time();
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return newerFirst(out[a], keys[a], out[b], keys[b]); });
    std::vector<Entry> sorted;
    sorted.reserve(out.size());
    for (size_t i : order) sorted.push_back(std::move(out[i]));
    out = std::move(sorted);
    if (r == 2) LOGW("archive: %s: %s", folder.c_str(), st.error.c_str());
    if (st.truncated && st.read > 0)  // once per change, not at every listing of the library's page
        LOGW("archive: %s holds more than %d games: the oldest files are not listed", folder.c_str(), kMaxListed);
    if (stats) *stats = st;
    return out;
}

void clearCache() {
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.lock);
    c.files.clear();
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
    size_t offset = 0, length = 0;
    chess::pgn::Origin origin;
    if (listed && listed->fileSize == now.size && listed->fileTimeMs == now.timeMs && listed->offset + listed->length <= now.size) {
        // The file as listed: only the game's bytes are read (the library loads a game per
        // selection and per row of a big export).
        if (!readSlice(path, listed->offset, listed->length, text, err)) {
            res.error = err;
            return res;
        }
        length = text.size();
        origin = chess::pgn::Origin{listed->line, listed->column};
    } else {
        if (!readWhole(path, text, limits.maxBytes, err)) {
            res.error = err;
            return res;
        }
        // Unknown or changed file: find the game by its index (only its place is needed).
        chess::pgn::Limits placesOnly = limits;
        placesOnly.summaryTag = [](const std::string&) { return false; };
        chess::pgn::Result<chess::pgn::Summary> sc = chess::pgn::scan(text, placesOnly);
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
    chess::pgn::Result<chess::pgn::ParsedGame> rd =
        chess::pgn::read(offset == 0 && length == text.size() ? text : text.substr(offset, length), limits, origin);
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
        Cache& c = cache();
        std::lock_guard<std::mutex> lock(c.lock);
        c.files.erase(entry.path);
    }
    LOGI("archive: %s deleted", entry.path.c_str());
    res.status = RemoveStatus::Removed;
    return res;
}

// ---- Dates ------------------------------------------------------------------------------------------------

bool localTime(std::time_t t, std::tm& out) {
#ifdef _WIN32
    return localtime_s(&out, &t) == 0;
#else
    return localtime_r(&t, &out) != nullptr;
#endif
}

int digitsAt(const std::string& s, size_t at, size_t n) {
    if (s.size() < at + n) return -1;
    int v = 0;
    for (size_t i = at; i < at + n; ++i) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

int64_t daysFromCivil(int64_t y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// ---- Games of an online server ----------------------------------------------------------------------------

namespace {

// "2026.09.28" and "14:03:07", in UTC, as a time_t; false unless both are complete and valid.
bool utcInstant(const std::string& date, const std::string& time, std::time_t& out) {
    if (date.size() != 10 || time.size() < 8) return false;
    const int y = digitsAt(date, 0, 4), mo = digitsAt(date, 5, 2), d = digitsAt(date, 8, 2);
    const int h = digitsAt(time, 0, 2), mi = digitsAt(time, 3, 2), se = digitsAt(time, 6, 2);
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 60) return false;
    out = std::time_t(daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se);
    return true;
}

std::string lowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// The start of a server's game: UTCDate (else Date, which the server writes in UTC too) and UTCTime.
bool serverStart(const Record& r, std::time_t& out) {
    const std::string date = r.findTag("UTCDate") ? r.tag("UTCDate") : r.tag("Date");
    return utcInstant(date, r.tag("UTCTime"), out);
}

}  // namespace

bool serverRecord(const std::string& pgnText, const ServerGame& game, Record& out, std::string& error) {
    if (pgnText.size() > kMaxServerPgnBytes) {
        error = "the PGN is too large";
        return false;
    }
    chess::pgn::Limits limits;
    limits.maxBytes = kMaxServerPgnBytes;
    limits.maxGames = 2;  // one game is expected: a second one is refused
    chess::pgn::Result<chess::pgn::ParsedGame> r = chess::pgn::read(pgnText, limits);
    if (!r.error.empty()) {
        error = r.error;
        return false;
    }
    if (r.games.size() != 1) {
        error = r.games.empty() ? "no game in the PGN" : "more than one game in the PGN";
        return false;
    }
    const chess::pgn::ParsedGame& g = r.games[0];
    if (!g.ok()) {
        error = g.error.text();
        return false;
    }
    Record rec = g.record;
    const std::string id = std::to_string(game.gameId);
    if (const std::string* given = rec.findTag("ScacelithGameId")) {
        if (*given != id) {
            error = "the PGN is the one of game " + *given + ", not " + id;
            return false;
        }
    }
    rec.setTag("ScacelithMode", modeName(Mode::Server));
    rec.setTag("ScacelithServer", game.server);
    rec.setTag("ScacelithGameId", id);
    std::string end = game.endKey;
    if (end.compare(0, 7, "reason.") == 0) end = end.substr(7);
    if (!end.empty() && rec.result != "*") rec.setTag("ScacelithEnd", end);
    // Local date and time, as the folder's own games (the listing sorts and shows them by these).
    std::time_t start = 0;
    std::tm tm{};
    if (serverStart(rec, start) && localTime(start, tm)) {
        if (!rec.findTag("UTCDate")) rec.setTag("UTCDate", rec.tag("Date"));
        rec.setTag("Date", std::to_string(tm.tm_year + 1900) + "." + twoDigits(tm.tm_mon + 1) + "." + twoDigits(tm.tm_mday));
        rec.setTag("Time", twoDigits(tm.tm_hour) + ":" + twoDigits(tm.tm_min) + ":" + twoDigits(tm.tm_sec));
    }
    out = std::move(rec);
    return true;
}

const Entry* findServerGame(const std::vector<Entry>& entries, const std::string& server, uint64_t gameId) {
    const std::string id = std::to_string(gameId), origin = lowerAscii(server);
    for (const Entry& e : entries)
        if (e.mode == Mode::Server && e.tag("ScacelithGameId") == id && lowerAscii(e.tag("ScacelithServer")) == origin) return &e;
    return nullptr;
}

ServerSaveResult saveServerGame(const std::string& folder, const ServerGame& game, const std::string& pgnText) {
    ServerSaveResult res;
    const std::vector<Entry> entries = list(folder);
    if (const Entry* e = findServerGame(entries, game.server, game.gameId)) {
        res.status = ServerSaveStatus::AlreadySaved;
        res.path = e->path;
        res.index = e->index;
        return res;
    }
    if (pgnText.empty()) {
        res.status = ServerSaveStatus::NeedsText;
        return res;
    }
    Record rec;
    if (!serverRecord(pgnText, game, rec, res.error)) {
        res.status = ServerSaveStatus::Invalid;
        LOGW("archive: game %llu of %s cannot be saved: %s", (unsigned long long)game.gameId, game.server.c_str(), res.error.c_str());
        return res;
    }
    std::time_t start = 0;
    serverStart(rec, start);
    const SaveResult saved = save(folder, rec, start);
    if (!saved.ok) {
        res.status = ServerSaveStatus::Failed;
        res.error = saved.error;
        LOGW("archive: game %llu of %s cannot be saved: %s", (unsigned long long)game.gameId, game.server.c_str(), res.error.c_str());
        return res;
    }
    res.status = ServerSaveStatus::Saved;
    res.path = saved.path;
    res.index = 0;
    return res;
}

}  // namespace archive
}  // namespace game
