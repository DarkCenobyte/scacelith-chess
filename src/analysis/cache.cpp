// The evaluation cache of the Analysis mode (see cache.h). One text file per game:
//
//   scacelith-analysis 1
//   key 0123456789abcdef
//   positions 41
//   e <i> <depth> <final> <cp> <mate> <best> <second: 0 | 1 <cp> <mate> <uci>> <pv length> <pv...>
//
// one "e" line per analysed position (a position without legal moves needs none: the review knows
// it). Files are untrusted: anything that does not parse exactly, names another key or another
// number of positions, or is larger than kMaxBytes is ignored; the moves are checked again on the
// board by GameReview::restore(). Paths are UTF-8 (wide on Windows through net::sys and
// std::filesystem::u8path).
#include "analysis/cache.h"

#include "core/log.h"
#include "net/net_sys.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <system_error>

namespace analysis {

namespace {

constexpr const char* kMagic = "scacelith-analysis 1";
constexpr size_t kMaxBytes = size_t(2) << 20;   // ~200 bytes a position: games of thousands of plies fit
constexpr int kMaxPositions = 6000;
constexpr size_t kMaxPv = 64;
constexpr size_t kKeep = 200;                    // games kept in the folder

namespace fs = std::filesystem;

// A key names a file: letters and digits only (GameReview::key() is hexadecimal).
bool validKey(const std::string& key) {
    if (key.empty() || key.size() > 64) return false;
    for (const char c : key)
        if (!std::isalnum((unsigned char)c)) return false;
    return true;
}

// "e2e4", "e7e8q": the shape of a UCI move (its legality is the review's to check).
bool uciShape(const std::string& s) {
    if (s.size() != 4 && s.size() != 5) return false;
    if (s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8' || s[2] < 'a' || s[2] > 'h' || s[3] < '1' || s[3] > '8')
        return false;
    return s.size() == 4 || s[4] == 'q' || s[4] == 'r' || s[4] == 'b' || s[4] == 'n';
}

// A whole decimal integer within [lo, hi]: no sign games, no trailing characters.
bool readInt(std::istringstream& in, long lo, long hi, int& out) {
    std::string t;
    if (!(in >> t) || t.empty() || t.size() > 7) return false;
    size_t i = t[0] == '-' ? 1 : 0;
    if (i == t.size()) return false;
    long v = 0;
    for (; i < t.size(); ++i) {
        if (t[i] < '0' || t[i] > '9') return false;
        v = v * 10 + (t[i] - '0');
    }
    if (t[0] == '-') v = -v;
    if (v < lo || v > hi) return false;
    out = int(v);
    return true;
}

bool readScore(std::istringstream& in, ai::Score& s) {
    s = ai::Score();
    return readInt(in, -100000, 100000, s.cp) && readInt(in, -500, 500, s.mate);
}

bool readMove(std::istringstream& in, std::string& uci) { return bool(in >> uci) && uciShape(uci); }

bool parseEntry(const std::string& line, int positions, int& index, PositionEval& e) {
    std::istringstream in(line);
    std::string tag;
    int final = 0, hasSecond = 0, pvLength = 0;
    if (!(in >> tag) || tag != "e") return false;
    if (!readInt(in, 0, positions - 1, index) || !readInt(in, 1, 245, e.depth) || !readInt(in, 0, 1, final)) return false;
    e.final = final == 1;
    if (!readScore(in, e.best) || !readMove(in, e.bestUci) || !readInt(in, 0, 1, hasSecond)) return false;
    e.hasSecond = hasSecond == 1;
    if (e.hasSecond && (!readScore(in, e.second) || !readMove(in, e.secondUci))) return false;
    if (!readInt(in, 0, long(kMaxPv), pvLength)) return false;
    e.pv.clear();
    for (int i = 0; i < pvLength; ++i) {
        std::string m;
        if (!readMove(in, m)) return false;
        e.pv.push_back(m);
    }
    std::string extra;
    return !(in >> extra);
}

bool parse(const std::string& text, const std::string& key, int positions, std::vector<PositionEval>& out) {
    std::istringstream in(text);
    std::string line;
    auto next = [&](std::string& l) {
        if (!std::getline(in, l)) return false;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        return true;
    };
    if (!next(line) || line != kMagic) return false;
    if (!next(line) || line != "key " + key) return false;
    if (!next(line) || line != "positions " + std::to_string(positions)) return false;
    std::vector<PositionEval> evals(static_cast<size_t>(positions));
    std::vector<bool> seen(static_cast<size_t>(positions), false);
    while (next(line)) {
        if (line.empty()) continue;
        int index = -1;
        PositionEval e;
        if (!parseEntry(line, positions, index, e) || seen[size_t(index)]) return false;
        seen[size_t(index)] = true;
        evals[size_t(index)] = e;
    }
    out.swap(evals);
    return true;
}

std::string format(const std::string& key, const std::vector<PositionEval>& evals) {
    std::ostringstream o;
    o << kMagic << "\nkey " << key << "\npositions " << evals.size() << "\n";
    for (size_t i = 0; i < evals.size(); ++i) {
        const PositionEval& e = evals[i];
        if (e.failed || e.terminal || e.depth < 1 || !uciShape(e.bestUci)) continue;
        o << "e " << i << ' ' << std::min(e.depth, 245) << ' ' << (e.final ? 1 : 0) << ' ' << e.best.cp << ' '
          << e.best.mate << ' ' << e.bestUci;
        if (e.hasSecond && uciShape(e.secondUci)) o << " 1 " << e.second.cp << ' ' << e.second.mate << ' ' << e.secondUci;
        else o << " 0";
        std::vector<std::string> pv;
        for (const std::string& m : e.pv) {
            if (pv.size() >= kMaxPv || !uciShape(m)) break;
            pv.push_back(m);
        }
        o << ' ' << pv.size();
        for (const std::string& m : pv) o << ' ' << m;
        o << '\n';
    }
    return o.str();
}

// The cache files of the folder ("<key>.txt"), most recently used first (ties: by name).
std::vector<fs::path> cacheFiles(const std::string& folder) {
    struct File {
        fs::path path;
        fs::file_time_type time;
    };
    std::vector<File> files;
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(folder), ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        const std::string name = p.filename().u8string();
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".txt") != 0) continue;
        if (!validKey(name.substr(0, name.size() - 4))) continue;
        std::error_code e2;
        if (!it->is_regular_file(e2) || e2) continue;
        const fs::file_time_type t = fs::last_write_time(p, e2);
        if (e2) continue;
        files.push_back({p, t});
    }
    std::sort(files.begin(), files.end(), [](const File& a, const File& b) {
        if (a.time != b.time) return a.time > b.time;
        return a.path.filename() < b.path.filename();
    });
    std::vector<fs::path> out;
    for (const File& f : files) out.push_back(f.path);
    return out;
}

}  // namespace

std::string cachePath(const std::string& folder, const std::string& key) { return folder + key + ".txt"; }

bool loadCache(const std::string& folder, const std::string& key, int positions, std::vector<PositionEval>& out) {
    if (!validKey(key) || positions < 1 || positions > kMaxPositions) return false;
    const std::string path = cachePath(folder, key);
    uint64_t size = 0;
    if (!net::sys::fileSize(path, size) || size > kMaxBytes) return false;
    std::string text;
    if (!net::sys::readFile(path, text, kMaxBytes)) return false;
    std::vector<PositionEval> evals;
    if (!parse(text, key, positions, evals)) {
        LOGW("analysis cache: ignoring %s (damaged, or another game)", path.c_str());
        return false;
    }
    // Used again: the newest for the folder's trim.
    std::error_code ec;
    fs::last_write_time(fs::u8path(path), fs::file_time_type::clock::now(), ec);
    out.swap(evals);
    return true;
}

bool saveCache(const std::string& folder, const std::string& key, const std::vector<PositionEval>& evals) {
    if (!validKey(key) || evals.empty() || evals.size() > size_t(kMaxPositions) || folder.empty()) return false;
    if (!net::sys::makeDirectories(folder)) return false;
    const std::string path = cachePath(folder, key);
    // Written whole under a temporary name, then renamed over the old file (net::sys).
    if (!net::sys::writeFileAtomic(path, format(key, evals), false)) {
        LOGW("analysis cache: cannot write %s", path.c_str());
        return false;
    }
    std::error_code ec;
    fs::last_write_time(fs::u8path(path), fs::file_time_type::clock::now(), ec);
    // The folder keeps the most recently used games; the one just written stays whatever the clock.
    const std::vector<fs::path> files = cacheFiles(folder);
    size_t kept = 0;
    const fs::path self = fs::u8path(path).filename();
    for (const fs::path& f : files) {
        if (f.filename() == self || kept < kKeep - 1) {
            if (f.filename() != self) ++kept;
            continue;
        }
        fs::remove(f, ec);
    }
    return true;
}

}  // namespace analysis
