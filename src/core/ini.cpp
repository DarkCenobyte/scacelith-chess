#include "ini.h"
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// One line per entry: a CR or LF in a value (a server's string, say) would start a line of its
// own, read back as a key of its own.
static std::string oneLine(std::string s) {
    for (char& c : s)
        if (c == '\r' || c == '\n') c = ' ';
    return s;
}

// Writes text to path as std::ofstream does (text mode: CRLF line ends on Windows). 'created':
// the file could be opened, whether the writes succeeded or not.
static bool writeText(const std::string& path, const std::string& text, bool& created) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::trunc);
    created = bool(f);
    f << text;
    f.close();
    return created && !f.fail();
}

// Renames 'from' over 'to'.
static bool replaceFile(const std::string& from, const std::string& to) {
#ifdef _WIN32
    return MoveFileExW(std::filesystem::u8path(from).c_str(), std::filesystem::u8path(to).c_str(),
                       MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// The paths are UTF-8: opened as wide paths on Windows (u8path), whatever the process code page.
bool IniFile::load(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path));
    if (!f) return false;
    std::string line, section;
    bool first = true;
    while (std::getline(f, line)) {
        // A UTF-8 byte order mark (an editor's "UTF-8 with BOM") would hide a first [section].
        if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        first = false;
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            size_t e = line.find(']');
            section = trim(line.substr(1, e == std::string::npos ? std::string::npos : e - 1));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        set(section.empty() ? k : section + "." + k, v);
    }
    return true;
}

bool IniFile::save(const std::string& path) const {
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> bySection;
    std::vector<std::string> order;
    for (auto& e : entries_) {
        size_t dot = e.first.find('.');
        std::string sec = dot == std::string::npos ? "" : e.first.substr(0, dot);
        std::string key = dot == std::string::npos ? e.first : e.first.substr(dot + 1);
        if (!bySection.count(sec)) order.push_back(sec);
        bySection[sec].push_back({key, e.second});
    }
    std::string text = "; Scacelith settings\n";
    for (auto& sec : order) {
        if (!sec.empty()) text += "\n[" + oneLine(sec) + "]\n";
        for (auto& kv : bySection[sec]) text += oneLine(kv.first) + " = " + oneLine(kv.second) + "\n";
    }
    // Written whole under path.tmp, then renamed over path: a crash or a kill midway leaves the old
    // file intact. Straight to path, as before, when that fails (a folder that takes no new file,
    // the file held open by another program, a disk too full for a second copy): not atomic.
    const std::string tmp = path + ".tmp";
    bool created = false;
    if (writeText(tmp, text, created) && replaceFile(tmp, path)) return true;
    if (created) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(tmp), ec);
    }
    return writeText(path, text, created);
}

bool IniFile::writable(const std::string& path) {
    const std::filesystem::path p = std::filesystem::u8path(path);
#ifdef _WIN32
    // MoveFileExW does not replace a read-only file, and it cannot be written in place.
    DWORD attr = GetFileAttributesW(p.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY)) return false;
    if (attr == INVALID_FILE_ATTRIBUTES) {
        // No file yet: save() makes one where the folder lets a file be added, which a handle to
        // the folder asking for that right tells without adding any.
        const std::filesystem::path dir = p.has_parent_path() ? p.parent_path() : std::filesystem::path(L".");
        HANDLE h = CreateFileW(dir.c_str(), FILE_ADD_FILE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        CloseHandle(h);
        return true;
    }
#else
    // The new file renamed over the old one replaces it, read-only or not, when the folder takes
    // new files.
    const std::string dir = p.has_parent_path() ? p.parent_path().string() : std::string(".");
    if (access(dir.c_str(), W_OK | X_OK) == 0) return true;
#endif
    // Written in place, as save() does when no new file can be made: opened, not truncated.
    std::ofstream f(p, std::ios::in | std::ios::out);
    return bool(f);
}

std::string IniFile::getString(const std::string& key, const std::string& def) const {
    for (auto& e : entries_)
        if (e.first == key) return e.second;
    return def;
}
int IniFile::getInt(const std::string& key, int def) const {
    std::string s = getString(key);
    if (s.empty()) return def;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    // Saturated to the int range, as strtol does where long is 32-bit (Windows): never wrapped.
    return end == s.c_str() ? def : int(std::clamp(v, long(INT_MIN), long(INT_MAX)));
}
float IniFile::getFloat(const std::string& key, float def) const {
    std::string s = getString(key);
    if (s.empty()) return def;
    char* end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    return end == s.c_str() || !std::isfinite(v) ? def : v;   // "nan", "inf", "1e999": def
}
bool IniFile::getBool(const std::string& key, bool def) const {
    std::string s = getString(key);
    if (s.empty()) return def;
    // Any case: a hand-edited "True" or "YES" is true too.
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s == "1" || s == "true" || s == "yes" || s == "on";
}
void IniFile::set(const std::string& key, const std::string& value) {
    for (auto& e : entries_)
        if (e.first == key) { e.second = value; return; }
    entries_.push_back({key, value});
}
void IniFile::setFloat(const std::string& key, float v) {
    char b[64];
    std::snprintf(b, sizeof(b), "%g", v);
    set(key, b);
}
bool IniFile::has(const std::string& key) const {
    for (auto& e : entries_)
        if (e.first == key) return true;
    return false;
}
