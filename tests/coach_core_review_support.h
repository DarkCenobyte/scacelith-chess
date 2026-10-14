// Helpers of the coach_core_review_* tests, which drive the coach's review (coach/review.h), its
// explanations, praise and verification, the end-of-game appraisal and the Analysis mode's
// commentary without the game scene: games from SAN moves, engine lines made up by hand (or frozen
// from the embedded engine's own analyses, said where), and lookups in the scripts they give. Every
// script is also checked against the English lines: each key exists, each placeholder of each
// phrasing has its argument, each gesture or mark is anchored on a placeholder, a demonstration is
// taken back move for move.
#pragma once
#include "test.h"

#include "ai/analysis.h"
#include "chess/chess.h"
#include "coach/review.h"
#include "coach/script.h"
#include "core/embedded.h"
#include "i18n/i18n.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace corereview {

// ---- Games and analyses -------------------------------------------------------------------------

// A game from 'fen' (nullptr: the standard start) and SAN moves.
inline chess::Game gameOf(const char* fen, std::initializer_list<const char*> sans) {
    chess::Game g;
    if (fen) CHECK(g.resetFromFEN(fen));
    for (const char* s : sans) {
        const chess::Move m = g.position().parseSAN(s);
        if (!m.valid()) std::fprintf(stderr, "  bad SAN in test: %s\n", s);
        CHECK(m.valid());
        if (!m.valid()) break;
        g.play(m);
    }
    return g;
}

// A line of the engine: its score (the side to move's view) and its moves in UCI.
inline ai::PvLine pvl(int cp, const char* pv, int mate = 0) {
    ai::PvLine l;
    l.depth = 16;
    l.score.cp = cp;
    l.score.mate = mate;
    std::istringstream in(pv);
    std::string m;
    while (in >> m) l.pv.push_back(m);
    return l;
}

// The same with the engine's win/draw/loss chances (per mille, the side to move's).
inline ai::PvLine pvlWdl(int cp, const char* pv, int win, int draw, int loss) {
    ai::PvLine l = pvl(cp, pv);
    l.score.hasWdl = true;
    l.score.win = win;
    l.score.draw = draw;
    l.score.loss = loss;
    return l;
}

// A finished analysis of these lines (MultiPV order), every line searched to 'depth' (16 unless said).
inline ai::Analysis analysisOf(std::vector<ai::PvLine> lines, int depth = 16) {
    ai::Analysis a;
    a.ok = true;
    a.depth = depth;
    for (size_t i = 0; i < lines.size(); ++i) {
        lines[i].multipv = int(i + 1);
        lines[i].depth = depth;
    }
    a.lines = lines;
    if (!lines.empty() && !lines[0].pv.empty()) a.bestMove = lines[0].pv[0];
    return a;
}

// The review of the game's last move from A0 alone (no A1, A2, A3, no verification).
inline coach::Review reviewOf(coach::Reviewer& rv, const chess::Game& g, const ai::Analysis& a0, bool inBook = false) {
    coach::ReviewInput in;
    in.game = &g;
    in.before = &a0;
    in.inBook = inBook;
    return rv.review(in);
}

// ---- Scripts ------------------------------------------------------------------------------------

inline bool hasKey(const coach::Script& s, const std::string& key) {
    for (const coach::Beat& b : s)
        if (b.line.key == key) return true;
    return false;
}

inline bool hasPrefix(const coach::Script& s, const std::string& prefix) {
    for (const coach::Beat& b : s)
        if (b.line.key.rfind(prefix, 0) == 0) return true;
    return false;
}

inline const coach::Beat* beatWithKey(const coach::Script& s, const std::string& key) {
    for (const coach::Beat& b : s)
        if (b.line.key == key) return &b;
    return nullptr;
}

inline int demoMoves(const coach::Script& s) {
    int n = 0;
    for (const coach::Beat& b : s) n += b.kind == coach::BeatKind::DemoMove ? 1 : 0;
    return n;
}

inline bool pointsAt(const coach::Beat& b, coach::GestureKind kind, chess::Square sq) {
    for (const coach::Gesture& g : b.gestures)
        if (g.kind == kind && g.square == sq) return true;
    return false;
}

inline bool marks(const coach::Beat& b, coach::Mark::Kind kind, chess::Square sq) {
    for (const coach::Mark& m : b.marks)
        if (m.kind == kind && (m.square == sq || (kind == coach::Mark::Kind::Arrow && m.to == sq))) return true;
    return false;
}

// The keys of a script, one per line, for a failure's message.
inline void dumpKeys(const coach::Script& s, const char* where) {
    std::fprintf(stderr, "  %s:", where);
    for (const coach::Beat& b : s) std::fprintf(stderr, " %s", b.line.key.empty() ? "-" : b.line.key.c_str());
    std::fprintf(stderr, "\n");
}

// "ex.fork.b2.2" -> "ex.fork.b2": the variants of a message share its key.
inline std::string messageKey(const std::string& key) {
    const size_t dot = key.rfind('.');
    if (dot == std::string::npos || dot + 1 >= key.size()) return key;
    for (size_t i = dot + 1; i < key.size(); ++i)
        if (key[i] < '0' || key[i] > '9') return key;
    return std::atoi(key.c_str() + dot + 1) >= 2 ? key.substr(0, dot) : key;
}

// The English phrasings of the review's and the appraisal's lines, by key.
inline const std::map<std::string, std::vector<std::string>>& englishLines() {
    static const std::map<std::string, std::vector<std::string>> lines = [] {
        std::map<std::string, std::vector<std::string>> out;
        for (const char* file : {"assets/coach/speech/en/review.lang", "assets/coach/speech/en/appraisal.lang"}) {
            std::vector<std::pair<std::string, std::string>> entries;
            std::string err;
            const bool ok = i18n::parse(embedded::text(file), entries, &err);
            if (!ok) std::fprintf(stderr, "  %s: %s\n", file, err.c_str());
            CHECK(ok);
            for (const auto& e : entries) out[messageKey(e.first)].push_back(e.second);
        }
        return out;
    }();
    return lines;
}

inline std::set<std::string> placeholders(const std::string& text) {
    std::set<std::string> out;
    size_t p = 0;
    while ((p = text.find('{', p)) != std::string::npos) {
        const size_t q = text.find('}', p);
        if (q == std::string::npos) break;
        std::string name = text.substr(p + 1, q - p - 1);
        const size_t colon = name.find(':');
        if (colon != std::string::npos) name = name.substr(0, colon);
        out.insert(name);
        p = q + 1;
    }
    return out;
}

// The rules every script follows (as the review's own tests check them).
inline void checkScript(const coach::Script& s, const char* where) {
    const auto& en = englishLines();
    int pendingDemo = 0;
    for (const coach::Beat& b : s) {
        if (b.kind == coach::BeatKind::DemoMove) ++pendingDemo;
        if (b.kind == coach::BeatKind::Rewind) {
            CHECK_EQ(b.count, pendingDemo);
            pendingDemo = 0;
        }
        if (b.line.key.empty()) continue;
        const auto it = en.find(b.line.key);
        if (it == en.end()) {
            std::fprintf(stderr, "  %s: key %s missing from the English lines\n", where, b.line.key.c_str());
            CHECK(false);
            continue;
        }
        for (const std::string& text : it->second) {
            const std::set<std::string> ph = placeholders(text);
            for (const std::string& p : ph)
                if (!b.line.arg(p)) {
                    std::fprintf(stderr, "  %s: %s \"%s\": no argument for {%s}\n", where, b.line.key.c_str(),
                                 text.c_str(), p.c_str());
                    CHECK(false);
                }
            for (const coach::Gesture& g : b.gestures)
                if (!g.anchor.empty() && !ph.count(g.anchor)) {
                    std::fprintf(stderr, "  %s: %s: gesture anchor {%s} not in \"%s\"\n", where, b.line.key.c_str(),
                                 g.anchor.c_str(), text.c_str());
                    CHECK(false);
                }
            for (const coach::Mark& m : b.marks)
                if (!m.anchor.empty() && !ph.count(m.anchor)) {
                    std::fprintf(stderr, "  %s: %s: mark anchor {%s} not in \"%s\"\n", where, b.line.key.c_str(),
                                 m.anchor.c_str(), text.c_str());
                    CHECK(false);
                }
        }
    }
    CHECK_EQ(pendingDemo, 0);
}

}  // namespace corereview
