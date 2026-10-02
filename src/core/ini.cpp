#include "ini.h"
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
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
    std::ofstream f(std::filesystem::u8path(path), std::ios::trunc);
    if (!f) return false;
    f << "; Scacelith settings\n";
    for (auto& sec : order) {
        if (!sec.empty()) f << "\n[" << sec << "]\n";
        for (auto& kv : bySection[sec]) f << kv.first << " = " << kv.second << "\n";
    }
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
    return end == s.c_str() ? def : v;
}
bool IniFile::getBool(const std::string& key, bool def) const {
    std::string s = getString(key);
    if (s.empty()) return def;
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
