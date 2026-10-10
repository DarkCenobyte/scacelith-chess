// Tests for the Analysis mode's review (src/analysis/review.h), commentary (commentary.h) and
// evaluation cache (cache.h): symbols and their colours, the classification of hand-built analyses
// (each symbol, book moves, recaptures and forced replies kept from !/!!, only moves, mates), the
// order of the searches (quick pass before deep, the board's surroundings first, pending positions,
// forget, fail, positions without moves), the evaluation bar, the summary's lichess accuracy, the
// key, restore and the cache files (round trip, damaged / foreign / oversized files, the 200-file
// trim), the comments (a piece left hanging, a mate allowed or missed, the opening left, the end of
// the game, determinism, silence on quiet moves, every key in the English catalog, every mark
// anchored on its line), and one short game reviewed by the embedded engine.
#include "test.h"

#include "ai/analysis.h"
#include "ai/engine.h"
#include "analysis/cache.h"
#include "analysis/commentary.h"
#include "analysis/review.h"
#include "chess/chess.h"
#include "coach/appraisal.h"
#include "coach/catalog.h"
#include "core/embedded.h"
#include "i18n/i18n.h"
#include "net/net_sys.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace chess;
using analysis::Comment;
using analysis::Commentator;
using analysis::EvalBar;
using analysis::GameInfo;
using analysis::GameReview;
using analysis::Nag;
using analysis::PositionEval;
using analysis::Verdict;

namespace {

namespace fs = std::filesystem;

// ---- Games and hand-built analyses ----------------------------------------------------------------

struct Record {
    Position start;
    std::vector<Move> moves;
};

Record recordOf(const char* fen, std::initializer_list<const char*> sans) {
    Record r;
    if (fen) CHECK(r.start.setFEN(fen));
    Position p = r.start;
    for (const char* s : sans) {
        const Move m = p.parseSAN(s);
        if (!m.valid()) std::fprintf(stderr, "  bad SAN in test: %s\n", s);
        CHECK(m.valid());
        if (!m.valid()) break;
        r.moves.push_back(m);
        p.makeMove(m);
    }
    return r;
}

GameReview reviewOf(const Record& r, const analysis::Settings& s = analysis::Settings()) {
    GameReview g;
    g.reset(r.start, r.moves, s);
    return g;
}

struct L {
    int cp = 0;
    const char* pv = "";
    int mate = 0;
};

// An analysis of position i of the review, as the engine reports it (scores from the side to move).
ai::Analysis an(const GameReview& r, int i, std::vector<L> lines, int depth = 18) {
    ai::Analysis a;
    a.ok = true;
    a.whiteToMove = r.positionAt(i).sideToMove() == White;
    a.depth = depth;
    for (size_t k = 0; k < lines.size(); ++k) {
        ai::PvLine l;
        l.multipv = int(k + 1);
        l.depth = depth;
        l.score.cp = lines[k].cp;
        l.score.mate = lines[k].mate;
        std::istringstream in(lines[k].pv);
        std::string m;
        while (in >> m) l.pv.push_back(m);
        a.lines.push_back(l);
    }
    if (!a.lines.empty() && !a.lines[0].pv.empty()) a.bestMove = a.lines[0].pv[0];
    return a;
}

// Every position not analysed yet marked failed: the commentary then has its board facts only.
void failRest(GameReview& r) {
    for (int i = 0; i < r.positions(); ++i)
        if (r.position(i).depth == 0 && !r.position(i).terminal) r.fail(i);
}

// A legal move of position i other than 'not' (for evaluations whose best move is not the one played).
std::string otherMove(const GameReview& r, int i, const std::string& notUci) {
    const Position& p = r.positionAt(i);
    for (const Move& m : p.legalMoves())
        if (p.toUCI(m) != notUci) return p.toUCI(m);
    return notUci;
}

const double kEps = 1e-9;

// ---- The English lines -------------------------------------------------------------------------

std::map<std::string, std::vector<std::string>> englishVariants() {
    std::map<std::string, std::vector<std::string>> out;
    std::vector<std::pair<std::string, std::string>> entries;
    std::string err;
    const bool ok = i18n::parse(embedded::text("assets/coach/speech/en/analysis.lang"), entries, &err);
    if (!ok) std::fprintf(stderr, "  analysis.lang: %s\n", err.c_str());
    CHECK(ok);
    for (const auto& e : entries) {
        std::string k = e.first;
        const size_t dot = k.rfind('.');
        if (dot != std::string::npos && dot + 1 < k.size() &&
            std::all_of(k.begin() + long(dot) + 1, k.end(), [](char c) { return c >= '0' && c <= '9'; }))
            k = k.substr(0, dot);
        if (k.size() > 7 && k.compare(k.size() - 7, 7, ".spoken") == 0) continue;
        out[k].push_back(e.second);
    }
    return out;
}

std::set<std::string> placeholders(const std::string& text) {
    std::set<std::string> out;
    static const std::regex re("\\{([A-Za-z0-9_@]+)(?::[A-Za-z0-9_]+)?\\}");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it)
        out.insert((*it)[1].str());
    return out;
}

// The rules every comment follows: its keys exist, each placeholder of every variant has its
// argument, each mark is anchored on a placeholder of exactly one of its lines (in every variant),
// and it renders in English, written and spoken, without a placeholder left.
void checkComment(const Comment& c, const char* where) {
    static const std::map<std::string, std::vector<std::string>> en = englishVariants();
    for (const coach::Line& l : c.lines) {
        auto it = en.find(l.key);
        if (it == en.end()) {
            std::fprintf(stderr, "  %s: key %s missing from analysis.lang\n", where, l.key.c_str());
            CHECK(false);
            continue;
        }
        for (const std::string& text : it->second)
            for (const std::string& p : placeholders(text))
                if (!l.arg(p)) {
                    std::fprintf(stderr, "  %s: %s \"%s\": no argument {%s}\n", where, l.key.c_str(), text.c_str(), p.c_str());
                    CHECK(false);
                }
        for (bool spoken : {false, true}) {
            const std::string text = coach::Catalog::shared().renderVariant(l, "en", spoken, 1).text;
            CHECK(!text.empty());
            CHECK(text.find('{') == std::string::npos);
        }
    }
    for (const coach::Mark& m : c.marks) {
        CHECK(!m.anchor.empty());
        int lines = 0;
        for (const coach::Line& l : c.lines) {
            auto it = en.find(l.key);
            if (it == en.end()) continue;
            int with = 0;
            for (const std::string& text : it->second) with += placeholders(text).count(m.anchor) ? 1 : 0;
            if (with > 0) {
                ++lines;
                CHECK_EQ(with, int(it->second.size()));   // in every variant
            }
        }
        if (lines != 1) std::fprintf(stderr, "  %s: mark anchor {%s} on %d lines\n", where, m.anchor.c_str(), lines);
        CHECK_EQ(lines, 1);
    }
}

bool hasLine(const Comment& c, const std::string& key) {
    for (const coach::Line& l : c.lines)
        if (l.key == key) return true;
    return false;
}

const coach::Line* lineOf(const Comment& c, const std::string& key) {
    for (const coach::Line& l : c.lines)
        if (l.key == key) return &l;
    return nullptr;
}

bool hasMark(const Comment& c, coach::Mark::Kind kind, Square a, Square b = NoSquare) {
    for (const coach::Mark& m : c.marks) {
        if (m.kind != kind) continue;
        if (kind == coach::Mark::Kind::Arrow && m.from == a && m.to == b) return true;
        if (kind != coach::Mark::Kind::Arrow && m.square == a) return true;
    }
    return false;
}

Square sq(const char* s) { return parseSquare(s); }

// A knight left en prise: 1.Nd5?? exd5 (kHangFen of the coach's tests).
const char* kHangFen = "6k1/5ppp/4p3/8/8/2N5/5PPP/6K1 w - - 0 1";
// Rd8 mates (the back rank).
const char* kBackRankWhite = "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1";
const char* kBackRankBlack = "6k1/p4ppp/8/8/8/8/5PPP/3R2K1 b - - 0 1";
// Italian structure with d3: nothing of White's hangs, Bxf7+ gives a bishop for a pawn.
const char* kSacFen = "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/3P1N2/PPP2PPP/RNBQK2R w KQkq - 0 4";

// ---- Files ---------------------------------------------------------------------------------------

struct TempFolder {
    fs::path path;
    explicit TempFolder(const char* tag) {
#ifdef _WIN32
        const unsigned pid = unsigned(GetCurrentProcessId());
#else
        const unsigned pid = unsigned(getpid());
#endif
        path = fs::u8path(net::sys::exeDirectory()) / ("analysis-cache-test-" + std::string(tag) + "-" + std::to_string(pid));
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ~TempFolder() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string folder() const { return path.u8string() + "/"; }
    std::vector<std::string> names() const {
        std::vector<std::string> out;
        std::error_code ec;
        for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
            out.push_back(it->path().filename().u8string());
        std::sort(out.begin(), out.end());
        return out;
    }
};

void writeFile(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

}  // namespace

// ---- Symbols and colours -------------------------------------------------------------------------

TEST(game_review_symbols_and_colours) {
    CHECK_EQ(std::string(analysis::nagSymbol(Nag::None)), std::string(""));
    CHECK_EQ(std::string(analysis::nagSymbol(Nag::Good)), std::string("!"));
    CHECK_EQ(std::string(analysis::nagSymbol(Nag::Mistake)), std::string("?"));
    CHECK_EQ(std::string(analysis::nagSymbol(Nag::Brilliant)), std::string("!!"));
    CHECK_EQ(std::string(analysis::nagSymbol(Nag::Blunder)), std::string("??"));
    CHECK_EQ(std::string(analysis::nagSymbol(Nag::Interesting)), std::string("!?"));
    CHECK_EQ(std::string(analysis::nagSymbol(Nag::Dubious)), std::string("?!"));
    // Linear RGB of the shared sRGB colours.
    auto lin = [](int v) {
        const double c = v / 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    auto same = [&](analysis::Rgb c, int hex) {
        return std::fabs(c.r - lin((hex >> 16) & 0xFF)) < 1e-5 && std::fabs(c.g - lin((hex >> 8) & 0xFF)) < 1e-5 &&
               std::fabs(c.b - lin(hex & 0xFF)) < 1e-5;
    };
    CHECK(same(analysis::nagColor(Nag::Blunder), 0xCA3431));
    CHECK(same(analysis::nagColor(Nag::Mistake), 0xE58F2A));
    CHECK(same(analysis::nagColor(Nag::Dubious), 0xF7C045));
    CHECK(same(analysis::nagColor(Nag::Interesting), 0xB57FD6));
    CHECK(same(analysis::nagColor(Nag::Good), 0x5C8BB0));
    CHECK(same(analysis::nagColor(Nag::Brilliant), 0x1BACA6));
    CHECK(same(analysis::nagColor(Nag::None), 0x9A9A9A));
    CHECK(same(analysis::betterMoveColor(), 0x81B64C));
    CHECK(std::fabs(analysis::nagColor(Nag::None).r - 0.3231f) < 1e-3);   // #9A in linear light
}

// ---- Positions, passes, scheduling ------------------------------------------------------------------

TEST(game_review_positions_and_terminal) {
    // Fool's mate: the final position needs no search.
    GameReview r = reviewOf(recordOf(nullptr, {"f3", "e5", "g4", "Qh4#"}));
    CHECK_EQ(r.plies(), 4);
    CHECK_EQ(r.positions(), 5);
    CHECK(r.positionAt(4).isCheckmate());
    CHECK_EQ(r.game().sanMoves().back(), std::string("Qh4#"));
    const PositionEval& last = r.position(4);
    CHECK(last.terminal && last.final && !last.failed);
    CHECK(last.best.matedNow);
    CHECK(last.bestUci.empty());
    CHECK_EQ(last.legalMoves, 0);
    CHECK_EQ(r.position(0).legalMoves, 20);
    CHECK(!r.position(0).terminal && !r.position(0).final && r.position(0).depth == 0);
    const EvalBar b = r.bar(4);
    CHECK(b.known);
    CHECK_EQ(b.text, std::string("0-1"));
    CHECK_EQ(b.white, 0.0f);
    CHECK(!r.bar(3).known);
    // The terminal position is never handed out.
    ai::AnalysisRequest q;
    int pos = -1;
    std::set<int> handed;
    while (r.nextRequest(4, q, pos)) handed.insert(pos);
    CHECK(handed == std::set<int>({0, 1, 2, 3}));
    CHECK(std::fabs(r.progress() - 0.2f) < 1e-6);
    CHECK(!r.complete());

    // A stalemate at the end: drawn, "½-½".
    GameReview s = reviewOf(recordOf("7k/8/6Q1/8/8/8/8/6K1 w - - 0 1", {"Qf7"}));
    CHECK(s.positionAt(1).isStalemate());
    CHECK(s.position(1).terminal);
    CHECK(!s.position(1).best.matedNow);
    CHECK_EQ(s.bar(1).text, std::string("\xC2\xBD-\xC2\xBD"));
    CHECK_EQ(s.bar(1).white, 0.5f);

    // A record that goes on past an automatic ending (a dead position) is kept whole.
    GameReview d = reviewOf(recordOf("8/8/4k3/8/8/4K3/8/8 w - - 0 1", {"Kd3", "Kd5", "Ke3"}));
    CHECK_EQ(d.plies(), 3);
    // An illegal move ends the record there.
    Record bad = recordOf(nullptr, {"e4", "e5"});
    bad.moves.push_back(bad.moves[0]);   // e2e4 again: illegal
    GameReview t = reviewOf(bad);
    CHECK_EQ(t.plies(), 2);
    // Cleared: nothing to search.
    t.clear();
    CHECK(!t.nextRequest(0, q, pos));
    CHECK(t.complete());
    CHECK_EQ(t.plies(), 0);
}

TEST(game_review_scheduling_quick_before_deep_focus_first) {
    analysis::Settings s;
    s.quickDepth = 10;
    s.deepDepth = 18;
    s.multiPV = 2;
    GameReview r = reviewOf(recordOf(nullptr, {"e4", "e5", "Nf3", "Nc6", "Bb5", "a6", "Ba4", "Nf6", "O-O", "Be7"}), s);
    CHECK_EQ(r.positions(), 11);
    ai::AnalysisRequest q;
    int pos = -1;
    // Quick pass: the board's position, its neighbours, then the rest from the start; a position
    // handed out is not handed out again.
    std::vector<int> order;
    std::vector<ai::AnalysisRequest> reqs(11);
    while (r.nextRequest(5, q, pos)) {
        order.push_back(pos);
        reqs[size_t(pos)] = q;
        CHECK_EQ(q.depth, 10);
        CHECK_EQ(q.multiPV, 2);
        CHECK_EQ(int(q.moves.size()), pos);
        CHECK(q.startFen.empty());
        CHECK(q.priority < 0);
        CHECK_EQ(q.moveTimeMs, s.quickMoveTimeMs);
    }
    CHECK(order == std::vector<int>({5, 6, 4, 7, 3, 8, 2, 0, 1, 9, 10}));
    CHECK_EQ(reqs[3].moves[2], std::string("g1f3"));
    // A dropped request comes back.
    r.forget(7);
    CHECK(r.nextRequest(0, q, pos));
    CHECK_EQ(pos, 7);
    CHECK(!r.nextRequest(0, q, pos));
    // Quick results: every verdict known, none final; then the deep pass, from the new focus.
    for (int i = 0; i < 11; ++i) {
        const Position& p = r.positionAt(i);
        r.accept(i, an(r, i, {{20, p.toUCI(p.legalMoves()[0]).c_str()}}, 10));
        CHECK(!r.position(i).final);
        CHECK_EQ(r.position(i).depth, 10);
    }
    CHECK(r.verdict(3).known && !r.verdict(3).final);
    CHECK(r.bar(3).known);
    CHECK_EQ(r.progress(), 0.0f);
    order.clear();
    while (r.nextRequest(0, q, pos)) {
        order.push_back(pos);
        CHECK_EQ(q.depth, 18);
        CHECK_EQ(q.moveTimeMs, s.deepMoveTimeMs);
    }
    CHECK(order == std::vector<int>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
    for (int i = 0; i < 11; ++i) {
        const Position& p = r.positionAt(i);
        r.accept(i, an(r, i, {{25, p.toUCI(p.legalMoves()[0]).c_str()}}, 18));
        CHECK(r.position(i).final);
    }
    CHECK(r.verdict(3).final);
    CHECK(r.complete());
    CHECK_EQ(r.progress(), 1.0f);
    CHECK(!r.nextRequest(5, q, pos));
    // A late answer for a final position changes nothing.
    r.accept(4, an(r, 4, {{-900, r.position(4).bestUci.c_str()}}, 30));
    CHECK_EQ(r.position(4).best.cp, 25);

    // A deep result shallower than the quick one (a search cut short) keeps the quick one.
    GameReview c = reviewOf(recordOf(nullptr, {"e4"}), s);
    c.accept(0, an(c, 0, {{30, "e2e4"}}, 12));
    CHECK(!c.position(0).final);
    c.accept(0, an(c, 0, {{-50, "d2d4"}}, 9));
    CHECK(c.position(0).final);
    CHECK_EQ(c.position(0).best.cp, 30);
    // A quick search deep enough for the deep pass is final at once.
    c.accept(1, an(c, 1, {{-30, "e7e5"}}, 20));
    CHECK(c.position(1).final);
}

TEST(game_review_fail_and_wrong_answers) {
    GameReview r = reviewOf(recordOf(nullptr, {"e4", "e5", "Nf3"}));
    ai::AnalysisRequest q;
    int pos = -1;
    CHECK(r.nextRequest(0, q, pos));
    CHECK_EQ(pos, 0);
    ai::Analysis refused;
    refused.ok = false;
    r.accept(0, refused);   // the engine refused it: never handed out again
    CHECK(r.position(0).failed);
    CHECK(!r.position(0).final);
    std::set<int> handed;
    while (r.nextRequest(0, q, pos)) handed.insert(pos);
    CHECK(!handed.count(0));
    // An answer for the other side to move, or with an illegal first move, is no answer for it.
    GameReview w = reviewOf(recordOf(nullptr, {"e4"}));
    ai::Analysis other = an(w, 0, {{10, "e2e4"}});
    other.whiteToMove = false;
    w.accept(0, other);
    CHECK(w.position(0).failed);
    w.accept(1, an(w, 1, {{10, "e2e4"}}));   // White's move in Black's position
    CHECK(w.position(1).failed);
    CHECK(w.complete());
    CHECK_EQ(w.progress(), 1.0f);   // failed positions count as done
    CHECK(!w.verdict(0).known);
    // The deep pass failing keeps the quick result as the last word.
    GameReview f = reviewOf(recordOf(nullptr, {"e4"}));
    f.accept(0, an(f, 0, {{30, "e2e4 e7e5"}}, 10));
    f.fail(0);
    CHECK(f.position(0).final && !f.position(0).failed);
    CHECK_EQ(f.position(0).bestUci, std::string("e2e4"));
    // The line kept is its legal prefix.
    f.accept(1, an(f, 1, {{-30, "e7e5 g1f3 b8c6 a1a8"}}, 18));
    CHECK(f.position(1).pv == std::vector<std::string>({"e7e5", "g1f3", "b8c6"}));
}

// ---- Classification ----------------------------------------------------------------------------------

TEST(game_review_blunder_mistake_inaccuracy) {
    // 1.Nd5?? exd5: a knight for nothing.
    GameReview r = reviewOf(recordOf(kHangFen, {"Nd5"}));
    r.accept(0, an(r, 0, {{20, "g1f1 g8f8"}, {10, "h2h3 g8f8"}}));
    r.accept(1, an(r, 1, {{330, "e6d5 g1f1"}, {0, "g8f8"}}));
    Verdict v = r.verdict(0);
    CHECK(v.known && v.final);
    CHECK_EQ(v.mover, White);
    CHECK_EQ(v.san, std::string("Nd5"));
    CHECK_EQ(v.uci, std::string("c3d5"));
    CHECK_EQ(v.cls, coach::MoveClass::Blunder);
    CHECK_EQ(v.nag, Nag::Blunder);
    CHECK(!v.playedBest);
    CHECK_EQ(v.betterUci, std::string("g1f1"));
    CHECK_EQ(v.betterSan, std::string("Kf1"));
    CHECK(std::fabs(v.wBest - coach::winPercent(20)) < kEps);
    CHECK(std::fabs(v.wPlayed - coach::winPercent(-330)) < kEps);
    CHECK(std::fabs(v.loss - (v.wBest - v.wPlayed)) < kEps);
    CHECK_EQ(v.whiteCpAfter, -330);
    CHECK(v.sacrifice);   // a knight given away (for nothing: no !! for a losing move)
    // Mistake and inaccuracy: the same move judged with smaller losses.
    GameReview m = reviewOf(recordOf(kHangFen, {"h3"}));
    m.accept(0, an(m, 0, {{0, "g1f1"}}));
    m.accept(1, an(m, 1, {{150, "g8f8"}}));
    CHECK_EQ(m.verdict(0).nag, Nag::Mistake);
    m = reviewOf(recordOf(kHangFen, {"h3"}));
    m.accept(0, an(m, 0, {{0, "g1f1"}}));
    m.accept(1, an(m, 1, {{60, "g8f8"}}));
    CHECK_EQ(m.verdict(0).cls, coach::MoveClass::Inaccuracy);
    CHECK_EQ(m.verdict(0).nag, Nag::Dubious);
    m = reviewOf(recordOf(kHangFen, {"h3"}));
    m.accept(0, an(m, 0, {{0, "g1f1"}}));
    m.accept(1, an(m, 1, {{30, "g8f8"}}));
    CHECK_EQ(m.verdict(0).nag, Nag::None);
    // The engine's own move is judged against itself, whatever the next search says.
    m = reviewOf(recordOf(kHangFen, {"h3"}));
    m.accept(0, an(m, 0, {{0, "h2h3"}}));
    m.accept(1, an(m, 1, {{400, "g8f8"}}));
    CHECK(m.verdict(0).playedBest);
    CHECK_EQ(m.verdict(0).loss, 0.0);
    CHECK_EQ(m.verdict(0).nag, Nag::None);
    CHECK(m.verdict(0).betterUci.empty());
}

TEST(game_review_mates) {
    // A mate in one missed: Rd8# was there, h3 is played.
    GameReview r = reviewOf(recordOf(kBackRankWhite, {"h3"}));
    r.accept(0, an(r, 0, {{0, "d1d8", 1}, {300, "d1d7"}}));
    r.accept(1, an(r, 1, {{-500, "g8f8"}}));
    Verdict v = r.verdict(0);
    CHECK_EQ(v.nag, Nag::Blunder);
    CHECK_EQ(v.betterSan, std::string("Rd8#"));
    CHECK_EQ(r.bar(0).text, std::string("M1"));
    CHECK_EQ(r.bar(0).mateWhite, 1);
    // A mate allowed: 1...a6?? Rd8#.
    GameReview a = reviewOf(recordOf(kBackRankBlack, {"a6"}));
    a.accept(0, an(a, 0, {{-500, "g7g6 d1d8 g8g7"}}));
    a.accept(1, an(a, 1, {{0, "d1d8", 1}}));
    v = a.verdict(0);
    CHECK_EQ(v.mover, Black);
    CHECK_EQ(v.nag, Nag::Blunder);
    CHECK_EQ(v.whiteCpAfter, 10000);
    CHECK_EQ(a.bar(1).text, std::string("M1"));
    // Already lost: a mate allowed from -12 is softened (lichess).
    a = reviewOf(recordOf(kBackRankBlack, {"a6"}));
    a.accept(0, an(a, 0, {{-1200, "g7g6"}}));
    a.accept(1, an(a, 1, {{0, "d1d8", 1}}));
    CHECK_EQ(a.verdict(0).nag, Nag::Dubious);
    // Mating: the move that mates is the best, no symbol; the final position says 1-0.
    GameReview k = reviewOf(recordOf(kBackRankWhite, {"Rd8#"}));
    k.accept(0, an(k, 0, {{0, "d1d8", 1}, {300, "d1d7"}}));
    v = k.verdict(0);
    CHECK(v.known && v.final);
    CHECK(v.playedBest);
    CHECK_EQ(v.nag, Nag::None);
    CHECK_EQ(v.whiteCpAfter, 10000);
    CHECK_EQ(k.bar(1).text, std::string("1-0"));
    CHECK_EQ(k.bar(1).white, 1.0f);
}

TEST(game_review_only_moves) {
    // The best move, the second line 22 W% points worse: !.
    GameReview r = reviewOf(recordOf(kHangFen, {"Kf1"}));
    r.accept(0, an(r, 0, {{50, "g1f1 g8f8"}, {-200, "c3d5"}}));
    r.accept(1, an(r, 1, {{-50, "g8f8"}}));
    Verdict v = r.verdict(0);
    CHECK(v.onlyMove);
    CHECK_EQ(v.nag, Nag::Good);
    // The second line close behind: nothing special.
    r = reviewOf(recordOf(kHangFen, {"Kf1"}));
    r.accept(0, an(r, 0, {{50, "g1f1 g8f8"}, {0, "h2h3"}}));
    r.accept(1, an(r, 1, {{-50, "g8f8"}}));
    CHECK(!r.verdict(0).onlyMove);
    CHECK_EQ(r.verdict(0).nag, Nag::None);
    // A game already decided: no !.
    r = reviewOf(recordOf(kHangFen, {"Kf1"}));
    r.accept(0, an(r, 0, {{900, "g1f1 g8f8"}, {300, "h2h3"}}));
    r.accept(1, an(r, 1, {{-900, "g8f8"}}));
    CHECK_EQ(r.verdict(0).nag, Nag::None);
    // Without a second line (MultiPV 1): no !.
    r = reviewOf(recordOf(kHangFen, {"Kf1"}));
    r.accept(0, an(r, 0, {{50, "g1f1 g8f8"}}));
    r.accept(1, an(r, 1, {{-50, "g8f8"}}));
    CHECK_EQ(r.verdict(0).nag, Nag::None);
    // A recapture is never !: 1...Rxd1 2.Rxd1 (the second line, leaving the rook, far worse).
    GameReview y = reviewOf(recordOf("3r2k1/5ppp/8/8/8/8/5PPP/3R1RK1 b - - 0 1", {"Rxd1", "Rxd1"}));
    y.accept(1, an(y, 1, {{0, "f1d1 g8f8"}, {-500, "h2h3"}}));
    y.accept(2, an(y, 2, {{0, "g8f8"}}));
    CHECK(y.verdict(1).playedBest);
    CHECK(!y.verdict(1).onlyMove);
    CHECK_EQ(y.verdict(1).nag, Nag::None);
    // Nor is taking what the previous move gave away: 1.Nd5?? exd5.
    GameReview g = reviewOf(recordOf(kHangFen, {"Nd5", "exd5"}));
    g.accept(1, an(g, 1, {{330, "e6d5 g1f1"}, {0, "g8f8"}}));
    g.accept(2, an(g, 2, {{-330, "g1f1"}}));
    CHECK(g.verdict(1).playedBest);
    CHECK(!g.verdict(1).onlyMove);
    CHECK_EQ(g.verdict(1).nag, Nag::None);
    // A single legal move: forced, no symbol.
    GameReview f = reviewOf(recordOf("7k/8/5QK1/8/8/8/8/8 b - - 0 1", {"Kg8"}));
    CHECK_EQ(f.position(0).legalMoves, 1);
    f.accept(0, an(f, 0, {{-900, "h8g8"}}));
    f.accept(1, an(f, 1, {{0, "f6d8", 1}}));
    CHECK_EQ(f.verdict(0).cls, coach::MoveClass::Forced);
    CHECK_EQ(f.verdict(0).nag, Nag::None);
    // A reply to a check with one sensible answer is no !: Kh7, the other move (Re8) loses a rook.
    GameReview c = reviewOf(recordOf("3R2k1/5pp1/8/8/8/8/4rPPP/6K1 b - - 0 1", {"Kh7"}));
    CHECK_EQ(c.position(0).legalMoves, 2);
    c.accept(0, an(c, 0, {{-300, "g8h7 f2f3"}, {-900, "e2e8"}}));
    c.accept(1, an(c, 1, {{300, "f2f3"}}));
    CHECK(c.verdict(0).playedBest);
    CHECK(!c.verdict(0).onlyMove);
    CHECK_EQ(c.verdict(0).nag, Nag::None);
}

TEST(game_review_brilliant_and_interesting) {
    // 4.Bxf7+: a bishop for a pawn, and the engine's best move.
    GameReview r = reviewOf(recordOf(kSacFen, {"Bxf7+"}));
    CHECK(!r.verdict(0).book);
    r.accept(0, an(r, 0, {{80, "c4f7 e8f7 f3g5"}, {30, "e1g1"}}));
    r.accept(1, an(r, 1, {{-80, "e8f7 f3g5 f7g8"}}));
    Verdict v = r.verdict(0);
    CHECK(v.sacrifice);
    CHECK(v.playedBest);
    CHECK_EQ(v.nag, Nag::Brilliant);
    // The same move when the game was already won: not brilliant.
    r = reviewOf(recordOf(kSacFen, {"Bxf7+"}));
    r.accept(0, an(r, 0, {{800, "c4f7 e8f7"}, {700, "e1g1"}}));
    r.accept(1, an(r, 1, {{-800, "e8f7"}}));
    CHECK_EQ(r.verdict(0).nag, Nag::None);
    // ... or when it loses: no symbol of praise (a mistake here).
    r = reviewOf(recordOf(kSacFen, {"Bxf7+"}));
    r.accept(0, an(r, 0, {{30, "e1g1"}, {20, "c4b3"}}));
    r.accept(1, an(r, 1, {{100, "e8f7"}}));
    CHECK_EQ(r.verdict(0).nag, Nag::Mistake);
    // Not the best, within 5 W% points of it: !?.
    r = reviewOf(recordOf(kSacFen, {"Bxf7+"}));
    r.accept(0, an(r, 0, {{100, "e1g1"}, {80, "c4f7"}}));
    r.accept(1, an(r, 1, {{-60, "e8f7"}}));
    v = r.verdict(0);
    CHECK(!v.playedBest);
    CHECK(v.loss > 2.0 && v.loss < 5.0);
    CHECK_EQ(v.nag, Nag::Interesting);
    // A quiet developing move gives nothing away.
    r = reviewOf(recordOf(kSacFen, {"O-O"}));
    r.accept(0, an(r, 0, {{30, "e1g1"}}));
    r.accept(1, an(r, 1, {{-30, "f8c5"}}));
    CHECK(!r.verdict(0).sacrifice);
    CHECK_EQ(r.verdict(0).nag, Nag::None);
    // Recaptures give nothing away either: 1...Rxd8 after 1.Rxd8+.
    GameReview t = reviewOf(recordOf("3r2k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1", {"Rxd8+"}));
    CHECK(!t.verdict(0).sacrifice);
}

TEST(game_review_book_moves) {
    // 1.f3 is in the book: no symbol for a loss of 8 W% points (it would be ?!).
    GameReview r = reviewOf(recordOf(nullptr, {"f3", "e5", "g4", "Qh4#"}));
    CHECK(r.verdict(0).book && r.verdict(1).book && r.verdict(2).book && r.verdict(3).book);
    r.accept(0, an(r, 0, {{30, "e2e4"}}));
    r.accept(1, an(r, 1, {{60, "e7e5"}}));
    r.accept(2, an(r, 2, {{-50, "g1h3"}}));
    r.accept(3, an(r, 3, {{0, "d8h4", 1}}));
    CHECK_EQ(r.verdict(0).cls, coach::MoveClass::Book);
    CHECK_EQ(r.verdict(0).nag, Nag::None);
    // 2.g4?? is in the book too (Fool's Mate), and stays a blunder.
    CHECK_EQ(r.verdict(2).cls, coach::MoveClass::Blunder);
    CHECK_EQ(r.verdict(2).nag, Nag::Blunder);
    // Book moves count as perfect in the summary.
    const analysis::SideSummary w = r.summary(White);
    CHECK_EQ(w.moves, 2);
    CHECK_EQ(w.count[int(Nag::Blunder)], 1);
    CHECK_EQ(w.count[int(Nag::None)], 1);
    const analysis::SideSummary b = r.summary(Black);
    CHECK_EQ(b.moves, 2);
    CHECK_EQ(b.count[int(Nag::None)], 2);
    CHECK(std::fabs(b.accuracy - 100.0) < 1e-9);
    CHECK(w.accuracy < 100.0);
}

// ---- The bar ---------------------------------------------------------------------------------------

TEST(game_review_eval_bar) {
    GameReview r = reviewOf(recordOf(nullptr, {"e4", "e5", "Nf3", "Nc6", "Bc4"}));
    CHECK(!r.bar(0).known);
    CHECK_EQ(r.bar(0).white, 0.5f);
    r.accept(0, an(r, 0, {{125, "e2e4"}}));
    EvalBar b = r.bar(0);
    CHECK(b.known);
    CHECK_EQ(b.text, std::string("+1.3"));
    CHECK(std::fabs(b.white - float(coach::winPercent(125) / 100.0)) < 1e-6);
    CHECK_EQ(b.mateWhite, 0);
    r.accept(1, an(r, 1, {{40, "e7e5"}}));   // Black to move, Black's view: White -0.4
    CHECK_EQ(r.bar(1).text, std::string("-0.4"));
    CHECK(r.bar(1).white < 0.5f);
    r.accept(2, an(r, 2, {{-4, "g1f3"}}));
    CHECK_EQ(r.bar(2).text, std::string("0.0"));
    r.accept(3, an(r, 3, {{0, "b8c6", 2}}));   // Black mates in 2
    b = r.bar(3);
    CHECK_EQ(b.text, std::string("-M2"));
    CHECK_EQ(b.mateWhite, -2);
    CHECK_EQ(b.white, 0.0f);
    r.accept(4, an(r, 4, {{0, "f1c4", 3}}));   // White to move, mates in 3
    b = r.bar(4);
    CHECK_EQ(b.text, std::string("M3"));
    CHECK_EQ(b.mateWhite, 3);
    CHECK_EQ(b.white, 1.0f);
    r.accept(5, an(r, 5, {{-1000, "g8f6"}}));   // Black's view -10: White +10, clamped chances
    CHECK_EQ(r.bar(5).text, std::string("+10.0"));
    CHECK(std::fabs(r.bar(5).white - float(coach::winPercent(1000) / 100.0)) < 1e-6);
    CHECK_EQ(r.bar(-1).known, false);
    CHECK_EQ(r.bar(6).known, false);
}

// ---- Summary and accuracy ------------------------------------------------------------------------

TEST(game_review_summary_lichess_accuracy) {
    // A rook ending, no book: every move judged from the evaluations below (White's view).
    const Record rec = recordOf("4k3/8/8/8/8/8/R7/4K3 w - - 0 1", {"Ra3", "Kd7", "Ra4", "Kd6", "Ra5", "Kd7", "Ra6", "Ke7"});
    GameReview r = reviewOf(rec);
    // Every move loses something, a little or a lot, for both sides.
    const std::vector<int> cpWhite = {30, -60, 80, -150, 120, -300, 200, 50, 400};
    for (int i = 0; i <= 8; ++i) {
        const bool wtm = r.positionAt(i).sideToMove() == White;
        const std::string played = i < 8 ? r.positionAt(i).toUCI(rec.moves[size_t(i)]) : std::string();
        const std::string best = otherMove(r, i, played);
        r.accept(i, an(r, i, {{wtm ? cpWhite[size_t(i)] : -cpWhite[size_t(i)], best.c_str()}}));
    }
    for (int k = 0; k < 8; ++k) CHECK(!r.verdict(k).book && !r.verdict(k).playedBest);
    const std::vector<int> after(cpWhite.begin() + 1, cpWhite.end());
    const coach::SideAccuracy want = coach::gameAccuracy(after, White, cpWhite[0]);
    const analysis::SideSummary w = r.summary(White), b = r.summary(Black);
    CHECK_EQ(w.moves, 4);
    CHECK_EQ(b.moves, 4);
    std::fprintf(stderr, "  accuracy White %.3f Black %.3f (lichess %.3f %.3f)\n", w.accuracy, b.accuracy, want.white,
                 want.black);
    CHECK(std::fabs(w.accuracy - want.white) < 1e-9);
    CHECK(std::fabs(b.accuracy - want.black) < 1e-9);
    // The counts follow the verdicts.
    int counted[7] = {0, 0, 0, 0, 0, 0, 0};
    for (int k = 0; k < 8; k += 2) ++counted[int(r.verdict(k).nag)];
    for (int i = 0; i < 7; ++i) CHECK_EQ(w.count[i], counted[i]);
    // Over the plies known so far only.
    GameReview part = reviewOf(rec);
    part.accept(0, an(part, 0, {{30, "a2b2"}}));
    part.accept(1, an(part, 1, {{-40, "e8f7"}}));
    CHECK_EQ(part.summary(White).moves, 1);
    CHECK_EQ(part.summary(Black).moves, 0);
    CHECK_EQ(part.summary(Black).accuracy, 0.0);
    CHECK(std::fabs(part.summary(White).accuracy - 100.0) < 1e-9);   // 30 -> 40: nothing lost
}

// ---- Key, restore, cache ----------------------------------------------------------------------------

TEST(game_review_key_is_stable) {
    const Record a = recordOf(nullptr, {"e4", "e5", "Nf3"});
    GameReview r1 = reviewOf(a), r2 = reviewOf(a);
    CHECK_EQ(r1.key(), r2.key());
    CHECK_EQ(r1.key().size(), size_t(16));
    CHECK(std::all_of(r1.key().begin(), r1.key().end(), [](char c) { return std::isxdigit((unsigned char)c) && !std::isupper((unsigned char)c); }));
    // The key does not depend on the analysis or the settings.
    analysis::Settings s;
    s.deepDepth = 22;
    GameReview r3 = reviewOf(a, s);
    r3.accept(0, an(r3, 0, {{10, "e2e4"}}));
    CHECK_EQ(r3.key(), r1.key());
    // Another move, another start: another key.
    CHECK(reviewOf(recordOf(nullptr, {"e4", "e5", "Nc3"})).key() != r1.key());
    CHECK(reviewOf(recordOf(nullptr, {"e4", "e5"})).key() != r1.key());
    CHECK(reviewOf(recordOf("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 2", {"e4", "e5", "Nf3"})).key() !=
          r1.key());
    // Pinned: a saved review is found again by later versions.
    CHECK_EQ(reviewOf(recordOf(nullptr, {})).key(), std::string("a0e6e2cddc00b552"));
    CHECK_EQ(r1.key(), std::string("b5e5159118f0da5d"));
}

TEST(game_review_restore) {
    analysis::Settings s;
    s.quickDepth = 10;
    s.deepDepth = 18;
    const Record rec = recordOf(nullptr, {"e4", "e5", "Nf3", "Nc6"});
    GameReview done = reviewOf(rec, s);
    for (int i = 0; i < done.positions(); ++i) {
        const Position& p = done.positionAt(i);
        const std::string m = p.toUCI(p.legalMoves()[0]), m2 = p.toUCI(p.legalMoves()[1]);
        done.accept(i, an(done, i, {{10 * i, (m + " ").c_str()}, {5 * i - 40, m2.c_str()}}, 18));
    }
    CHECK(done.complete());
    // The same game again: every position final at once, nothing to search.
    GameReview again = reviewOf(rec, s);
    again.restore(done.evaluations());
    CHECK(again.complete());
    ai::AnalysisRequest q;
    int pos = -1;
    CHECK(!again.nextRequest(0, q, pos));
    for (int i = 0; i < again.positions(); ++i) {
        CHECK_EQ(again.position(i).best.cp, done.position(i).best.cp);
        CHECK_EQ(again.position(i).secondUci, done.position(i).secondUci);
        CHECK_EQ(again.bar(i).text, done.bar(i).text);
    }
    for (int k = 0; k < again.plies(); ++k) CHECK_EQ(int(again.verdict(k).nag), int(done.verdict(k).nag));
    // Deeper settings: the saved evaluations count as the quick pass only.
    analysis::Settings deeper = s;
    deeper.deepDepth = 22;
    GameReview d = reviewOf(rec, deeper);
    d.restore(done.evaluations());
    CHECK(!d.complete());
    CHECK(d.position(2).depth == 18 && !d.position(2).final);
    CHECK(d.nextRequest(2, q, pos));
    CHECK_EQ(q.depth, 22);
    CHECK_EQ(pos, 2);
    // Shallower than the quick pass: ignored. Another game's moves, another size: ignored.
    analysis::Settings high = s;
    high.quickDepth = 19;
    high.deepDepth = 24;
    GameReview h = reviewOf(rec, high);
    h.restore(done.evaluations());
    CHECK_EQ(h.position(0).depth, 0);
    std::vector<PositionEval> foreign = done.evaluations();
    foreign[1].bestUci = "e2e4";   // White's move in a position where Black is to move
    foreign[2].pv = {"g1f3", "zz"};
    GameReview g = reviewOf(rec, s);
    g.restore(foreign);
    CHECK_EQ(g.position(1).depth, 0);
    CHECK(g.position(0).final);
    CHECK(g.position(2).pv == std::vector<std::string>({g.position(2).bestUci}) || g.position(2).pv[0] == g.position(2).bestUci);
    foreign.pop_back();
    GameReview z = reviewOf(rec, s);
    z.restore(foreign);
    CHECK_EQ(z.position(0).depth, 0);
}

TEST(game_review_cache_round_trip_and_bad_files) {
    TempFolder tmp("io");
    const std::string folder = tmp.folder();
    const Record rec = recordOf(nullptr, {"e4", "e5", "Qh5", "Nc6", "Bc4", "Nf6", "Qxf7#"});
    GameReview r = reviewOf(rec);
    for (int i = 0; i < r.positions() - 1; ++i) {
        const Position& p = r.positionAt(i);
        const std::string m = p.toUCI(p.legalMoves()[0]), m2 = p.toUCI(p.legalMoves()[1]);
        if (i == 6) r.accept(i, an(r, i, {{0, "h5f7", 1}, {200, (m2).c_str()}}, 18));
        else if (i % 2 == 0) r.accept(i, an(r, i, {{10 * i - 30, (m + " " + "x").c_str()}, {-20, m2.c_str()}}, 18));
        else if (i == 3) r.accept(i, an(r, i, {{-35, m.c_str()}}, 12));
    }
    const std::string key = r.key();
    CHECK(analysis::saveCache(folder, key, r.evaluations()));
    CHECK_EQ(analysis::cachePath(folder, key), folder + key + ".txt");
    CHECK(tmp.names() == std::vector<std::string>({key + ".txt"}));   // no temporary file left
    std::vector<PositionEval> back;
    CHECK(analysis::loadCache(folder, key, r.positions(), back));
    REQUIRE(int(back.size()) == r.positions());
    for (int i = 0; i < r.positions(); ++i) {
        const PositionEval& a = r.position(i);
        const PositionEval& b = back[size_t(i)];
        if (a.terminal || a.depth == 0) {
            CHECK_EQ(b.depth, 0);   // not saved: known without it, or never analysed
            continue;
        }
        CHECK_EQ(b.depth, a.depth);
        CHECK_EQ(b.final, a.final);
        CHECK_EQ(b.best.cp, a.best.cp);
        CHECK_EQ(b.best.mate, a.best.mate);
        CHECK_EQ(b.bestUci, a.bestUci);
        CHECK(b.pv == a.pv);
        CHECK_EQ(b.hasSecond, a.hasSecond);
        CHECK_EQ(b.second.cp, a.second.cp);
        CHECK_EQ(b.secondUci, a.secondUci);
    }
    GameReview again = reviewOf(rec);
    again.restore(back);
    for (int i = 0; i < r.positions(); ++i) CHECK_EQ(again.bar(i).text, r.bar(i).text);
    CHECK_EQ(again.position(3).final, false);   // the quick result stays quick

    // Missing, another number of positions, another key inside the file.
    CHECK(!analysis::loadCache(folder, "0000000000000000", r.positions(), back));
    CHECK(!analysis::loadCache(folder, key, r.positions() + 1, back));
    std::string text;
    CHECK(net::sys::readFile(analysis::cachePath(folder, key), text, 1 << 20));
    const std::string other = "0123456789abcdef";
    writeFile(fs::u8path(analysis::cachePath(folder, other)), text);
    CHECK(!analysis::loadCache(folder, other, r.positions(), back));
    // Damaged files: garbage, a cut line, an index out of range, a duplicate, a bad move.
    auto rejects = [&](const std::string& body) {
        writeFile(fs::u8path(analysis::cachePath(folder, other)), body);
        std::vector<PositionEval> out;
        return !analysis::loadCache(folder, other, 8, out);
    };
    const std::string head = "scacelith-analysis 1\nkey " + other + "\npositions 8\n";
    CHECK(!rejects(head + "e 0 18 1 20 0 e2e4 0 1 e2e4\n"));   // the well-formed control
    CHECK(rejects(std::string("\x89PNG\r\n\x1a\n\0\0", 10)));
    CHECK(rejects("scacelith-analysis 2\nkey " + other + "\npositions 8\n"));
    CHECK(rejects(head + "e 0 18 1 20 0 e2e4 0 1\n"));
    CHECK(rejects(head + "e 8 18 1 20 0 e2e4 0 0\n"));
    CHECK(rejects(head + "e 0 18 1 20 0 e2e4 0 0\ne 0 18 1 20 0 e2e4 0 0\n"));
    CHECK(rejects(head + "e 0 18 1 20 0 e2e9 0 0\n"));
    CHECK(rejects(head + "e 0 999 1 20 0 e2e4 0 0\n"));
    CHECK(rejects(head + "e 0 18 1 20x 0 e2e4 0 0\n"));
    CHECK(rejects(head + "e 0 18 1 20 0 e2e4 0 0 extra\n"));
    // Oversized: refused before it is read.
    std::string big = head;
    while (big.size() < (size_t(2) << 20) + 10) big += "# padding padding padding padding padding padding padding\n";
    CHECK(rejects(big));
    // Keys that are no file names.
    CHECK(!analysis::saveCache(folder, "../escape", r.evaluations()));
    CHECK(!analysis::loadCache(folder, "../escape", r.positions(), back));
    CHECK(!analysis::saveCache(folder, "", r.evaluations()));
    // A folder created on the way, its name in UTF-8.
    TempFolder deep("deep");
    const std::string named = deep.folder() + "Parties \xC3\xA9tudi\xC3\xA9" "es \xE2\x99\x9E/a/";
    CHECK(analysis::saveCache(named, key, r.evaluations()));
    CHECK(analysis::loadCache(named, key, r.positions(), back));
    CHECK(fs::exists(fs::u8path(named + key + ".txt")));
}

TEST(game_review_cache_keeps_the_newest_200) {
    TempFolder tmp("trim");
    fs::create_directories(tmp.path);
    const auto now = fs::file_time_type::clock::now();
    for (int i = 0; i < 205; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "%016x.txt", i + 1);
        const fs::path p = tmp.path / name;
        writeFile(p, "old");
        fs::last_write_time(p, now - std::chrono::hours(1000 - i));   // the higher i, the newer
    }
    writeFile(tmp.path / "README.md", "not a cache file");
    GameReview r = reviewOf(recordOf(nullptr, {"e4"}));
    r.accept(0, an(r, 0, {{20, "e2e4"}}));
    CHECK(analysis::saveCache(tmp.folder(), r.key(), r.evaluations()));
    std::vector<std::string> names = tmp.names();
    CHECK_EQ(int(names.size()), 201);   // 200 games and the other file
    CHECK(std::binary_search(names.begin(), names.end(), r.key() + ".txt"));
    CHECK(std::binary_search(names.begin(), names.end(), std::string("README.md")));
    for (int i = 0; i < 6; ++i) {   // the six oldest are gone
        char name[32];
        std::snprintf(name, sizeof name, "%016x.txt", i + 1);
        CHECK(!std::binary_search(names.begin(), names.end(), std::string(name)));
    }
    CHECK(std::binary_search(names.begin(), names.end(), std::string("00000000000000cd.txt")));   // the newest old one
    // Loading a game makes it the most recent: the next trim spares it.
    std::vector<PositionEval> out;
    const std::string oldest = "0000000000000007";
    writeFile(tmp.path / (oldest + ".txt"), "scacelith-analysis 1\nkey " + oldest + "\npositions 2\n");
    fs::last_write_time(tmp.path / (oldest + ".txt"), now - std::chrono::hours(5000));
    CHECK(analysis::loadCache(tmp.folder(), oldest, 2, out));
    GameReview r2 = reviewOf(recordOf(nullptr, {"d4"}));
    CHECK(analysis::saveCache(tmp.folder(), r2.key(), r2.evaluations()));
    names = tmp.names();
    CHECK(std::binary_search(names.begin(), names.end(), oldest + ".txt"));
    CHECK_EQ(int(names.size()), 201);
}

// ---- Commentary -----------------------------------------------------------------------------------------

TEST(game_review_comment_hanging_piece_and_better_move) {
    GameReview r = reviewOf(recordOf(kHangFen, {"Nd5", "exd5"}));
    r.accept(0, an(r, 0, {{20, "g1f1 g8f8"}, {10, "h2h3 g8f8"}}));
    r.accept(1, an(r, 1, {{330, "e6d5 g1f1"}, {0, "g8f8"}}));
    r.accept(2, an(r, 2, {{-330, "g1f1 g8f8"}}));
    Commentator c;
    c.reset(GameInfo());
    CHECK(c.ready(r, 1));
    const Comment k = c.commentAt(r, 1);
    CHECK_EQ(k.position, 1);
    CHECK_EQ(k.weight, 3);
    checkComment(k, "hanging");
    REQUIRE(k.lines.size() == 3u);
    CHECK_EQ(k.lines[0].key, std::string("an.blunder"));
    CHECK_EQ(k.lines[1].key, std::string("an.hanging"));
    CHECK_EQ(k.lines[2].key, std::string("an.better"));
    const coach::Arg* piece = k.lines[1].arg("piece");
    REQUIRE(piece != nullptr);
    CHECK(piece->bySide);
    CHECK_EQ(piece->piece, Knight);
    CHECK_EQ(piece->color, White);
    CHECK_EQ(piece->square, sq("d5"));
    CHECK_EQ(k.lines[1].arg("reply")->san, std::string("exd5"));
    CHECK_EQ(k.lines[2].arg("best")->san, std::string("Kf1"));
    CHECK(hasMark(k, coach::Mark::Kind::Piece, sq("d5")));
    CHECK(hasMark(k, coach::Mark::Kind::Arrow, sq("e6"), sq("d5")));
    CHECK(hasMark(k, coach::Mark::Kind::Arrow, sq("g1"), sq("f1")));
    // In English: "Nd5 is a blunder. It leaves White's knight unprotected, and exd5 takes it. Kf1 was stronger."
    const std::string said = coach::Catalog::shared().renderVariant(k.lines[1], "en", false, 1).text;
    CHECK(said.find("White's knight") != std::string::npos);
    // Taking the knight is no feat: nothing on it, only the end of the game.
    CHECK(c.ready(r, 2));
    const Comment end = c.commentAt(r, 2);
    checkComment(end, "after");
    REQUIRE(!end.lines.empty());
    CHECK_EQ(end.lines[0].key, std::string("an.end.unfinished"));
    for (const coach::Line& l : end.lines) CHECK(l.key.compare(0, 7, "an.end.") == 0);
}

TEST(game_review_comment_mates) {
    // 1...a6?? Rd8#: the mate allowed, then the end.
    GameReview a = reviewOf(recordOf(kBackRankBlack, {"a6", "Rd8#"}));
    a.accept(0, an(a, 0, {{-500, "g7g6 d1d8 g8g7"}}));
    a.accept(1, an(a, 1, {{0, "d1d8", 1}}));
    GameInfo info;
    info.result = "1-0";
    info.endReasonKey = "reason.checkmate";
    Commentator c;
    c.reset(info);
    REQUIRE(c.ready(a, 1));
    Comment k = c.commentAt(a, 1);
    checkComment(k, "mate allowed");
    CHECK(hasLine(k, "an.blunder"));
    const coach::Line* m = lineOf(k, "an.mate_allowed.one");
    REQUIRE(m != nullptr);
    CHECK_EQ(m->arg("reply")->san, std::string("Rd8#"));
    CHECK(hasMark(k, coach::Mark::Kind::Arrow, sq("d1"), sq("d8")));
    CHECK(hasLine(k, "an.better"));
    CHECK(!hasLine(k, "an.mate_on.white"));   // already said
    // The final position: White mated; Black's single move was a blunder.
    REQUIRE(c.ready(a, 2));
    k = c.commentAt(a, 2);
    checkComment(k, "end");
    REQUIRE(!k.lines.empty());
    CHECK_EQ(k.lines[0].key, std::string("an.end.mate.white"));
    CHECK(!hasLine(k, "an.end.accuracy"));   // too few moves
    CHECK(hasLine(k, "an.end.clean.white"));
    const coach::Line* bl = lineOf(k, "an.end.blunders.black");
    REQUIRE(bl != nullptr);
    CHECK_EQ(bl->arg("m")->number, 1);

    // A mate in one missed: the better move is the mate itself, said once.
    c.reset(GameInfo());
    GameReview r = reviewOf(recordOf(kBackRankWhite, {"h3"}));
    r.accept(0, an(r, 0, {{0, "d1d8", 1}, {300, "d1d7"}}));
    r.accept(1, an(r, 1, {{-500, "g8f8"}}));
    k = c.commentAt(r, 1);
    checkComment(k, "mate missed");
    const coach::Line* mm = lineOf(k, "an.mate_missed.one");
    REQUIRE(mm != nullptr);
    CHECK_EQ(mm->arg("best")->san, std::string("Rd8#"));
    CHECK(!hasLine(k, "an.better"));
    // A longer mate missed names its length; a mate appearing without a mistake is announced.
    r = reviewOf(recordOf(kBackRankWhite, {"h3", "h6", "Kh2"}));
    r.accept(0, an(r, 0, {{0, "d1d8", 1}, {300, "d1d7"}}));
    r.accept(1, an(r, 1, {{-1200, "h7h6"}}));
    r.accept(2, an(r, 2, {{0, "d1d8", 3}}));
    CHECK_EQ(r.verdict(1).nag, Nag::None);
    k = c.commentAt(r, 2);
    checkComment(k, "mate on");
    const coach::Line* on = lineOf(k, "an.mate_on.white");
    if (!on) for (auto& l : k.lines) std::fprintf(stderr, "    %s\n", l.key.c_str());
    REQUIRE(on != nullptr);
    CHECK_EQ(on->arg("m")->number, 3);
}

TEST(game_review_comment_book_exit_names_the_opening) {
    // The Najdorf, left with 6.a4.
    GameReview r = reviewOf(recordOf(nullptr, {"e4", "c5", "Nf3", "d6", "d4", "cxd4", "Nxd4", "Nf6", "Nc3", "a6", "a4", "e5"}));
    for (int k = 0; k < 10; ++k) CHECK(r.verdict(k).book);
    CHECK(!r.verdict(10).book);
    failRest(r);   // no engine: the opening is a board fact
    Commentator c;
    c.reset(GameInfo());
    REQUIRE(c.ready(r, 11));
    const Comment k = c.commentAt(r, 11);
    checkComment(k, "book exit");
    REQUIRE(k.lines.size() == 2u);
    CHECK_EQ(k.lines[0].key, std::string("an.opening"));
    CHECK_EQ(k.lines[0].arg("opening")->text, std::string("variation:najdorf"));
    CHECK_EQ(k.lines[1].key, std::string("an.book_exit"));
    CHECK_EQ(k.lines[1].arg("move")->san, std::string("a4"));
    CHECK_EQ(k.weight, 2);
    const std::string text = coach::Catalog::shared().renderVariant(k.lines[0], "en", false, 1).text;
    CHECK(text.find("Najdorf") != std::string::npos);
    // Nowhere else.
    for (int p = 1; p < r.positions(); ++p)
        if (p != 11) CHECK(!hasLine(c.commentAt(r, p), "an.book_exit"));
    // The start, and the end of an unfinished game.
    const Comment start = c.commentAt(r, 0);
    CHECK(c.ready(r, 0));
    REQUIRE(start.lines.size() == 1u);
    CHECK_EQ(start.lines[0].key, std::string("an.start"));
    const Comment end = c.commentAt(r, r.plies());
    REQUIRE(!end.lines.empty());
    CHECK_EQ(end.lines.back().key, std::string("an.end.unfinished"));
    // A set-up position has its own opening words and no opening book.
    GameReview s = reviewOf(recordOf(kHangFen, {"Kf1"}));
    CHECK_EQ(c.commentAt(s, 0).lines[0].key, std::string("an.start.setup"));
}

TEST(game_review_comment_good_moves_and_quiet_moves) {
    // A brilliant sacrifice names the piece offered.
    GameReview r = reviewOf(recordOf(kSacFen, {"Bxf7+", "Kxf7", "Ng5+"}));
    r.accept(0, an(r, 0, {{80, "c4f7 e8f7 f3g5"}, {30, "e1g1"}}));
    r.accept(1, an(r, 1, {{-80, "e8f7 f3g5 f7g8"}}));
    r.accept(2, an(r, 2, {{80, "f3g5 f7g8"}, {-200, "e1g1"}}));
    r.accept(3, an(r, 3, {{-80, "f7g8"}}));
    Commentator c;
    c.reset(GameInfo());
    Comment k = c.commentAt(r, 1);
    checkComment(k, "brilliant");
    const coach::Line* b = lineOf(k, "an.brilliant");
    REQUIRE(b != nullptr);
    CHECK_EQ(b->arg("piece")->piece, Bishop);
    CHECK_EQ(b->arg("piece")->square, sq("f7"));
    CHECK_EQ(k.weight, 3);
    // Kxf7 takes back what was offered: quiet.
    CHECK(c.commentAt(r, 2).empty());
    // Ng5+ is the only good move here: said, as by far the strongest (the second line is far
    // behind, but nobody is winning).
    k = c.commentAt(r, 3);
    checkComment(k, "only");
    CHECK(hasLine(k, "an.only.best") || hasLine(k, "an.only.hold") || hasLine(k, "an.only.win"));
    CHECK(k.lines.size() >= 1u);

    // A quiet good move: nothing at all.
    GameReview q = reviewOf(recordOf(kSacFen, {"O-O", "Bc5", "h3"}));
    q.accept(0, an(q, 0, {{30, "e1g1"}, {25, "c2c3"}}));
    q.accept(1, an(q, 1, {{-30, "f8c5"}, {-35, "f8e7"}}));
    q.accept(2, an(q, 2, {{30, "c2c3"}, {25, "h2h3"}}));
    q.accept(3, an(q, 3, {{-25, "d7d6"}}));
    CHECK(c.commentAt(q, 1).empty());
    CHECK(c.commentAt(q, 2).empty());

    // An inaccuracy is said in a quiet stretch only.
    GameReview d = reviewOf(recordOf(kSacFen, {"O-O", "Bc5", "h3", "d6", "a3", "a6"}));
    d.accept(0, an(d, 0, {{30, "e1g1"}, {25, "c2c3"}}));
    d.accept(1, an(d, 1, {{-30, "f8c5"}, {-35, "f8e7"}}));
    d.accept(2, an(d, 2, {{30, "c2c3"}, {25, "h2h3"}}));
    d.accept(3, an(d, 3, {{-30, "d7d6"}}));
    d.accept(4, an(d, 4, {{30, "c2c3"}}));
    d.accept(5, an(d, 5, {{60, "a7a6"}}));   // 4.a3?! (Black's view +0.6)
    d.accept(6, an(d, 6, {{-60, "c2c3"}}));
    CHECK_EQ(d.verdict(4).nag, Nag::Dubious);
    k = c.commentAt(d, 5);
    checkComment(k, "inaccuracy");
    REQUIRE(hasLine(k, "an.inaccuracy"));
    CHECK_EQ(k.weight, 1);
    CHECK_EQ(lineOf(k, "an.inaccuracy")->arg("best")->san, std::string("c3"));
    // ... and dropped right after the opening words.
    GameReview e = reviewOf(recordOf(kSacFen, {"a3", "a6"}));
    e.accept(0, an(e, 0, {{30, "e1g1"}}));
    e.accept(1, an(e, 1, {{60, "a7a6"}}));
    e.accept(2, an(e, 2, {{-60, "c2c3"}}));
    CHECK_EQ(e.verdict(0).nag, Nag::Dubious);
    CHECK(c.commentAt(e, 1).empty());
}

TEST(game_review_comment_chance_missed) {
    // 1.Nd5?? and Black does not take: the chance missed, and the piece it would have won.
    GameReview r = reviewOf(recordOf(kHangFen, {"Nd5", "h6"}));
    r.accept(0, an(r, 0, {{20, "g1f1 g8f8"}}));
    r.accept(1, an(r, 1, {{330, "e6d5 g1f1"}, {0, "g8f8"}}));
    r.accept(2, an(r, 2, {{20, "d5f4"}}));
    Commentator c;
    c.reset(GameInfo());
    const Comment k = c.commentAt(r, 2);
    checkComment(k, "chance");
    REQUIRE(!k.lines.empty());
    CHECK_EQ(k.lines[0].key, std::string("an.missed_chance"));
    const coach::Line* mc = lineOf(k, "an.missed_capture");
    REQUIRE(mc != nullptr);
    CHECK_EQ(mc->arg("best")->san, std::string("exd5"));
    CHECK_EQ(mc->arg("piece")->color, White);
    CHECK_EQ(mc->arg("piece")->piece, Knight);
    CHECK(!hasLine(k, "an.better"));
}

TEST(game_review_comment_fork_allowed) {
    // 1.e4 e5 2.Nf3 Nc6 3.Bc4 Nd4 4.Nxe5?! Qg5: the coach's worked example, here a blunder.
    GameReview r = reviewOf(recordOf(nullptr, {"e4", "e5", "Nf3", "Nc6", "Bc4", "Nd4", "Nxe5", "Qg5"}));
    r.accept(6, an(r, 6, {{40, "f3d4 e5d4 e1g1 g8f6"}, {20, "e1g1 d7d6"}}));
    r.accept(7, an(r, 7, {{350, "d8g5 e1g1 g5e5"}, {-20, "d7d6"}}));
    Commentator c;
    c.reset(GameInfo());
    const Comment k = c.commentAt(r, 7);
    checkComment(k, "fork");
    const coach::Line* f = lineOf(k, "an.fork");
    if (!f) for (auto& l : k.lines) std::fprintf(stderr, "    %s\n", l.key.c_str());
    REQUIRE(f != nullptr);
    CHECK_EQ(f->arg("reply")->san, std::string("Qg5"));
    const std::set<Square> targets = {f->arg("t1")->square, f->arg("t2")->square};
    CHECK(targets.count(sq("e5")));
    CHECK_EQ(f->arg("t1")->color, White);
    CHECK(f->arg("t1")->bySide);
    CHECK(hasMark(k, coach::Mark::Kind::Square, sq("g5")));
    CHECK(hasLine(k, "an.better"));
    CHECK_EQ(lineOf(k, "an.better")->arg("best")->san, std::string("Nxd4"));
}

TEST(game_review_comment_ready_and_deterministic) {
    const Record rec = recordOf(kHangFen, {"Nd5", "exd5", "Kf1", "Kf8"});
    GameReview r = reviewOf(rec);
    Commentator c;
    GameInfo info;
    info.result = "0-1";
    info.endReasonKey = "reason.resignation";
    c.reset(info);
    CHECK(c.ready(r, 0));
    CHECK(!c.ready(r, 1));   // nothing analysed yet
    r.accept(0, an(r, 0, {{20, "g1f1 g8f8"}}, 10));
    r.accept(1, an(r, 1, {{330, "e6d5 g1f1"}}, 10));
    CHECK(!c.ready(r, 1));   // quick only: the symbols may still change
    r.accept(0, an(r, 0, {{20, "g1f1 g8f8"}}));
    r.accept(1, an(r, 1, {{330, "e6d5 g1f1"}}));
    CHECK(c.ready(r, 1));
    CHECK(!c.ready(r, 2));
    r.accept(2, an(r, 2, {{-330, "g1f1"}}));
    r.accept(3, an(r, 3, {{330, "g8f8"}}));
    CHECK(c.ready(r, 3));
    CHECK(!c.ready(r, 4));   // the final position waits for the whole review
    r.accept(4, an(r, 4, {{-330, "f1e2"}}));
    CHECK(c.ready(r, 4));
    CHECK(!c.ready(r, 5));
    CHECK(!c.ready(r, -1));
    // The same comments in any order, from any commentator.
    std::vector<Comment> forward, backward(5);
    for (int p = 0; p <= 4; ++p) forward.push_back(c.commentAt(r, p));
    Commentator other;
    other.reset(info);
    for (int p = 4; p >= 0; --p) backward[size_t(p)] = other.commentAt(r, p);
    for (int p = 0; p <= 4; ++p) {
        CHECK_EQ(forward[size_t(p)].lines.size(), backward[size_t(p)].lines.size());
        for (size_t i = 0; i < forward[size_t(p)].lines.size() && i < backward[size_t(p)].lines.size(); ++i)
            CHECK_EQ(forward[size_t(p)].lines[i].key, backward[size_t(p)].lines[i].key);
        CHECK_EQ(forward[size_t(p)].marks.size(), backward[size_t(p)].marks.size());
        checkComment(forward[size_t(p)], "any");
    }
    // White resigned: Black wins.
    REQUIRE(!forward[4].lines.empty());
    CHECK_EQ(forward[4].lines[0].key, std::string("an.end.resigned.black"));
}

TEST(game_review_comment_keys_in_english) {
    coach::Catalog cat;
    CHECK(cat.load());
    const std::vector<std::string> keys = Commentator::keys();
    CHECK(keys.size() > 40u);
    std::set<std::string> unique(keys.begin(), keys.end());
    CHECK_EQ(unique.size(), keys.size());
    for (const std::string& k : keys) {
        if (!cat.has(k)) std::fprintf(stderr, "  missing English line %s\n", k.c_str());
        CHECK(cat.has(k));
    }
    // Every analysis line of the English file is one of them (no stray key), and renders cleanly:
    // no placeholder left, no bare square in a static line, at most two subtitle lines.
    const std::regex bareSquare("(^|[^A-Za-z0-9])[a-h][1-8]([^A-Za-z0-9]|$)");
    int lines = 0;
    for (const std::string& k : cat.keys("en", "analysis")) {
        if (k.size() > 7 && k.compare(k.size() - 7, 7, ".spoken") == 0) continue;
        const std::string* v = cat.find("en", k);
        REQUIRE(v != nullptr);
        CHECK(!std::regex_search(*v, bareSquare));
        coach::Line l;
        const size_t dot = k.rfind('.');
        const bool variant = dot != std::string::npos && std::isdigit((unsigned char)k[dot + 1]);
        l.key = variant ? k.substr(0, dot) : k;
        CHECK(unique.count(l.key));
        const int n = variant ? std::atoi(k.c_str() + dot + 1) : 1;
        for (const std::string& name : placeholders(*v)) {
            if (name == "move" || name == "best" || name == "reply") l.with(name, coach::Arg::ofMove("Nf3", "g1f3"));
            else if (name == "line") l.with(name, coach::Arg::ofMoves("Nxe5 dxe5 Qg4"));
            else if (name == "piece" || name == "t1" || name == "t2")
                l.with(name, coach::Arg::ofSidePiece(Knight, Black, sq("c6")));
            else if (name == "opening") l.with(name, coach::Arg::ofOpening("family:sicilian"));
            else l.with(name, coach::Arg::ofNumber(3));
        }
        for (bool spoken : {false, true}) {
            const coach::Catalog::Rendered out = cat.renderVariant(l, "en", spoken, n);
            CHECK_EQ(out.variant, n);
            CHECK(!out.text.empty());
            CHECK(out.text.find('{') == std::string::npos);
            if (!spoken && out.text.size() > 140) {
                std::fprintf(stderr, "  too long for two subtitle lines: %s\n", k.c_str());
                CHECK(false);
            }
            if (spoken) CHECK(!std::regex_search(out.text, bareSquare));
        }
        ++lines;
    }
    CHECK(lines > 100);
    // The side's pieces.
    coach::Line l;
    l.key = "an.hanging";
    l.with("piece", coach::Arg::ofSidePiece(Queen, White, sq("d1"))).with("reply", coach::Arg::ofMove("Bxd1", "g4d1"));
    const std::string written = cat.renderVariant(l, "en", false, 1).text;
    const std::string spoken = cat.renderVariant(l, "en", true, 2).text;
    std::fprintf(stderr, "  %s | %s\n", written.c_str(), spoken.c_str());
    CHECK_EQ(written.compare(0, 40, "It leaves White's queen unprotected, and"), 0);
    CHECK_EQ(spoken.compare(0, 41, "White's queen is left without a defender:"), 0);
}

// ---- With the embedded engine -------------------------------------------------------------------------

#if defined(SCACELITH_HAS_STOCKFISH)

TEST(game_review_engine_scholars_mate) {
    const auto t0 = std::chrono::steady_clock::now();
    ai::Engine e;
    CHECK(e.start());
    CHECK(e.waitReady(30000));
    ai::EngineSettings host;
    host.hashMB = 16;
    host.depth = 6;
    e.configure(host);
    e.newGame();
    analysis::Settings s;
    s.quickDepth = 8;
    s.deepDepth = 12;
    s.quickMoveTimeMs = 0;   // depth only: reproducible
    s.deepMoveTimeMs = 0;
    const Record rec = recordOf(nullptr, {"e4", "e5", "Bc4", "Nc6", "Qh5", "Nf6", "Qxf7#"});
    GameReview r = reviewOf(rec, s);
    ai::AnalysisRequest q;
    int pos = -1, searches = 0;
    while (r.nextRequest(0, q, pos)) {
        const uint32_t id = e.requestAnalysis(q);
        ai::Analysis a;
        const auto start = std::chrono::steady_clock::now();
        while (!e.analysisReady(id) && std::chrono::steady_clock::now() - start < std::chrono::seconds(30))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!e.takeAnalysis(id, a)) {
            r.forget(pos);
            break;
        }
        r.accept(pos, a);
        ++searches;
    }
    CHECK(r.complete());
    CHECK_EQ(searches, 14);   // 7 positions with moves, two passes
    const Verdict v = r.verdict(5);
    std::fprintf(stderr, "  3...Nf6: %s %s, W%% %.1f -> %.1f, better %s\n", coach::moveClassName(v.cls),
                 analysis::nagSymbol(v.nag), v.wBest, v.wPlayed, v.betterSan.c_str());
    CHECK_EQ(v.nag, Nag::Blunder);
    CHECK(!v.betterSan.empty());
    CHECK_EQ(r.bar(6).text, std::string("M1"));
    CHECK_EQ(r.bar(7).text, std::string("1-0"));
    for (int k = 0; k < 4; ++k) CHECK(r.verdict(k).nag != Nag::Blunder);
    GameInfo info;
    info.result = "1-0";
    info.endReasonKey = "reason.checkmate";
    Commentator c;
    c.reset(info);
    for (int p = 0; p <= r.plies(); ++p) {
        CHECK(c.ready(r, p));
        const Comment k = c.commentAt(r, p);
        checkComment(k, "engine");
        for (const coach::Line& l : k.lines)
            std::fprintf(stderr, "    %d %s: %s\n", p, l.key.c_str(),
                         coach::Catalog::shared().renderVariant(l, "en", false, 1).text.c_str());
    }
    const Comment blunder = c.commentAt(r, 6);
    const coach::Line* m = lineOf(blunder, "an.mate_allowed.one");
    REQUIRE(m != nullptr);
    CHECK_EQ(m->arg("reply")->san, std::string("Qxf7#"));
    CHECK(!hasLine(blunder, "an.book_exit"));   // a blunder out of the book: named once, as a blunder
    const Comment end = c.commentAt(r, 7);
    REQUIRE(!end.lines.empty());
    CHECK_EQ(end.lines[0].key, std::string("an.end.mate.white"));
    const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    std::fprintf(stderr, "  review of the scholar's mate: %d searches, %d ms\n", searches, ms);
    CHECK(ms < 15000);
}

#endif
