// Analysis value types and the parser of Stockfish 19's "info" lines (see analysis.h).
//
// What Stockfish 19 prints for each principal variation (UCIEngine::on_update_full):
//   info depth D seldepth S multipv K score (cp X | mate N) [lowerbound|upperbound] [wdl W D L]
//        nodes N nps N hashfull N tbhits N time MS pv m1 m2 ...
// "wdl" only with UCI_ShowWDL (the Engine always sets it). A position without legal moves gets a
// single "info depth 0 score mate 0" (checkmated) or "info depth 0 score cp 0" (stalemate).
// Everything else starting with "info" ("info string ...", "info depth D currmove M
// currmovenumber N") carries no score and is rejected.
#include "ai/analysis.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <sstream>
#include <utility>

namespace ai {
namespace {

// Strict decimal integer (optional sign), false on anything else or on overflow: the numbers come
// from another program's output and are never trusted blindly.
bool toInt64(const std::string& s, int64_t& out) {
    size_t i = 0;
    bool negative = false;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
        negative = s[0] == '-';
        i = 1;
    }
    if (i >= s.size()) return false;
    int64_t v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        if (v > (INT64_MAX - 9) / 10) return false;
        v = v * 10 + (s[i] - '0');
    }
    out = negative ? -v : v;
    return true;
}

bool toInt(const std::string& s, int& out, int lo, int hi) {
    int64_t v = 0;
    if (!toInt64(s, v)) return false;
    out = int(std::clamp<int64_t>(v, lo, hi));
    return true;
}

// Bounds of what we keep: far beyond anything Stockfish prints (cp within +-20000 even for
// tablebase wins, mates within 128 moves), small enough that negating or sorting never overflows.
constexpr int kMaxCp = 1000000;
constexpr int kMaxMate = 10000;
constexpr int kMateKey = 2 * kMaxCp;  // sortKey(): mates above (and mated below) any cp

}  // namespace

Score Score::flipped() const {
    Score s = *this;
    s.cp = -cp;
    s.mate = -mate;
    s.matedNow = matesNow;
    s.matesNow = matedNow;
    std::swap(s.win, s.loss);
    if (bound == Bound::Lower) s.bound = Bound::Upper;
    else if (bound == Bound::Upper) s.bound = Bound::Lower;
    return s;
}

double Score::expected() const {
    if (matesNow || mate > 0) return 1.0;
    if (matedNow || mate < 0) return 0.0;
    if (hasWdl) return std::clamp((win + 0.5 * draw) / 1000.0, 0.0, 1.0);
    // lichess: win% = 50 + 50 * (2 / (1 + exp(-0.00368208 * cp)) - 1), i.e. this logistic.
    return 1.0 / (1.0 + std::exp(-0.00368208 * double(std::clamp(cp, -kMaxCp, kMaxCp))));
}

int Score::sortKey() const {
    if (matesNow) return kMateKey + kMaxMate;
    if (matedNow) return -kMateKey - kMaxMate;
    if (mate > 0) return kMateKey + kMaxMate - std::min(mate, kMaxMate);   // mate in 1 highest
    if (mate < 0) return -kMateKey - kMaxMate + std::min(-mate, kMaxMate); // mated in 1 lowest
    return std::clamp(cp, -kMaxCp, kMaxCp);
}

std::string Score::text() const {
    if (matesNow) return "M0";
    if (matedNow) return "-M0";
    if (mate > 0) return "M" + std::to_string(mate);
    if (mate < 0) return "-M" + std::to_string(-mate);
    if (cp == 0) return "0.00";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%+.2f", cp / 100.0);
    return buf;
}

const PvLine* Analysis::line(const std::string& firstMove) const {
    for (const PvLine& l : lines)
        if (!l.pv.empty() && l.pv.front() == firstMove) return &l;
    return nullptr;
}

bool parseInfoLine(const std::string& line, PvLine& out, int64_t* nodes, int* timeMs) {
    std::istringstream is(line);
    std::vector<std::string> t;
    for (std::string w; is >> w;) t.push_back(std::move(w));
    if (t.size() < 2 || t[0] != "info" || t[1] == "string") return false;

    PvLine l;
    bool hasScore = false;
    int64_t n = -1;
    int ms = -1;
    for (size_t i = 1; i < t.size(); ++i) {
        const std::string& k = t[i];
        const bool hasValue = i + 1 < t.size();
        if (k == "pv") {
            l.pv.assign(t.begin() + std::ptrdiff_t(i + 1), t.end());
            break;
        } else if (k == "currmove" || k == "currmovenumber" || k == "string") {
            return false;  // search progress / text, never a score
        } else if (k == "depth") {
            if (!hasValue || !toInt(t[++i], l.depth, 0, 1000)) return false;
        } else if (k == "seldepth") {
            if (!hasValue || !toInt(t[++i], l.seldepth, 0, 1000)) return false;
        } else if (k == "multipv") {
            if (!hasValue || !toInt(t[++i], l.multipv, 0, 1000) || l.multipv < 1) return false;
        } else if (k == "score") {
            if (i + 2 >= t.size()) return false;
            const std::string& kind = t[i + 1];
            int v = 0;
            if (kind == "cp") {
                if (!toInt(t[i + 2], v, -kMaxCp, kMaxCp)) return false;
                l.score.cp = v;
            } else if (kind == "mate") {
                if (!toInt(t[i + 2], v, -kMaxMate, kMaxMate)) return false;
                l.score.mate = v;
                l.score.matedNow = v == 0;
            } else {
                return false;
            }
            hasScore = true;
            i += 2;
        } else if (k == "lowerbound") {
            l.score.bound = Score::Bound::Lower;
        } else if (k == "upperbound") {
            l.score.bound = Score::Bound::Upper;
        } else if (k == "wdl") {
            if (i + 3 >= t.size()) return false;
            int w = 0, d = 0, lo = 0;
            if (!toInt(t[i + 1], w, 0, 1000) || !toInt(t[i + 2], d, 0, 1000) || !toInt(t[i + 3], lo, 0, 1000))
                return false;
            l.score.hasWdl = true;
            l.score.win = w;
            l.score.draw = d;
            l.score.loss = lo;
            i += 3;
        } else if (k == "nodes") {
            if (!hasValue || !toInt64(t[++i], n) || n < 0) return false;
        } else if (k == "time") {
            if (!hasValue || !toInt(t[++i], ms, 0, INT_MAX)) return false;
        } else if (k == "nps" || k == "hashfull" || k == "tbhits") {
            ++i;  // value not needed
        }
        // anything else: a token a later Stockfish may add, skipped
    }
    if (!hasScore) return false;
    out = std::move(l);
    if (nodes && n >= 0) *nodes = n;
    if (timeMs && ms >= 0) *timeMs = ms;
    return true;
}

}  // namespace ai
