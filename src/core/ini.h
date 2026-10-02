// Minimal INI file: [section] / key = value / ; comments. Keys are addressed as "section.key".
#pragma once
#include <string>
#include <vector>
#include <utility>

class IniFile {
public:
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    // Whether save(path) can write there, found without writing anything: a read-only file (the
    // read-only attribute on Windows, which save()'s rename cannot replace either), a read-only
    // share or folder say no. No file yet: whether the folder takes a new one. Settings::load then
    // reads the copy Settings::save wrote instead.
    static bool writable(const std::string& path);

    std::string getString(const std::string& key, const std::string& def = "") const;
    int getInt(const std::string& key, int def = 0) const;
    float getFloat(const std::string& key, float def = 0.0f) const;
    bool getBool(const std::string& key, bool def = false) const;

    void set(const std::string& key, const std::string& value);
    void setInt(const std::string& key, int v) { set(key, std::to_string(v)); }
    void setFloat(const std::string& key, float v);
    void setBool(const std::string& key, bool v) { set(key, v ? "true" : "false"); }
    bool has(const std::string& key) const;

private:
    std::vector<std::pair<std::string, std::string>> entries_;  // ordered "section.key" -> value
};
