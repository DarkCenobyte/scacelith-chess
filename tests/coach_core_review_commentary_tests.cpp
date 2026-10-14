// Tests for parts of the Analysis mode's commentary (analysis/commentary.h) and of its evaluation
// cache (analysis/cache.h) that the game review tests leave aside: a pawn lost along the engine's
// line (said when the loss holds, also where the mover could take a pawn back at once but the line
// shows it never does; not when it is won back at once), a mate in two allowed and missed, a fork
// missed by a move that took one of its targets, a brilliant queen sacrifice that mates, the only
// move that holds a worse position, the end of the game for each way a game ends, the wait for a
// comment ended by a step elsewhere; the cache's trim between files used at the same moment, and a
// cache file that cannot be written. The engine's evaluations are made up for each position.
#include "coach_core_review_support.h"

#include "analysis/cache.h"
#include "analysis/commentary.h"
#include "analysis/review.h"
#include "core/log.h"
#include "net/net_sys.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace chess;
using analysis::Comment;
using analysis::Commentator;
using analysis::CommentWait;
using analysis::GameInfo;
using analysis::GameReview;
using analysis::Nag;
using analysis::PositionEval;

namespace {

namespace fs = std::filesystem;

Square sq(const char* s) { return parseSquare(s); }

// A review of these moves from 'fen' (nullptr: the standard start), nothing analysed yet.
GameReview reviewOf(const char* fen, std::initializer_list<const char*> sans) {
    Position start;
    if (fen) CHECK(start.setFEN(fen));
    Position p = start;
    std::vector<Move> moves;
    for (const char* s : sans) {
        const Move m = p.parseSAN(s);
        if (!m.valid()) std::fprintf(stderr, "  bad SAN in test: %s\n", s);
        CHECK(m.valid());
        if (!m.valid()) break;
        moves.push_back(m);
        p.makeMove(m);
    }
    GameReview r;
    r.reset(start, moves, analysis::Settings());
    return r;
}

struct L {
    int cp = 0;
    const char* pv = "";
    int mate = 0;
};

// A final analysis of position i of the review (scores from the side to move).
ai::Analysis an(const GameReview& r, int i, std::vector<L> lines) {
    ai::Analysis a;
    a.ok = true;
    a.whiteToMove = r.positionAt(i).sideToMove() == White;
    a.depth = 18;
    for (size_t k = 0; k < lines.size(); ++k) {
        ai::PvLine l;
        l.multipv = int(k + 1);
        l.depth = 18;
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

// The English lines of the commentary, by key (the spoken variants aside).
const std::map<std::string, std::vector<std::string>>& analysisLines() {
    static const std::map<std::string, std::vector<std::string>> lines = [] {
        std::map<std::string, std::vector<std::string>> out;
        std::vector<std::pair<std::string, std::string>> entries;
        std::string err;
        const bool ok = i18n::parse(embedded::text("assets/coach/speech/en/analysis.lang"), entries, &err);
        if (!ok) std::fprintf(stderr, "  analysis.lang: %s\n", err.c_str());
        CHECK(ok);
        for (const auto& e : entries) {
            const std::string k = corereview::messageKey(e.first);
            if (k.size() > 7 && k.compare(k.size() - 7, 7, ".spoken") == 0) continue;
            out[k].push_back(e.second);
        }
        return out;
    }();
    return lines;
}

// Every key exists, every placeholder of every variant has its argument, every mark is anchored on
// a placeholder of one of the lines.
void checkComment(const Comment& c, const char* where) {
    const auto& en = analysisLines();
    std::set<std::string> anchors;
    for (const coach::Line& l : c.lines) {
        const auto it = en.find(l.key);
        if (it == en.end()) {
            std::fprintf(stderr, "  %s: key %s missing from analysis.lang\n", where, l.key.c_str());
            CHECK(false);
            continue;
        }
        for (const std::string& text : it->second)
            for (const std::string& p : corereview::placeholders(text)) {
                anchors.insert(p);
                if (!l.arg(p)) {
                    std::fprintf(stderr, "  %s: %s \"%s\": no argument {%s}\n", where, l.key.c_str(), text.c_str(), p.c_str());
                    CHECK(false);
                }
            }
    }
    for (const coach::Mark& m : c.marks) CHECK(anchors.count(m.anchor) == 1);
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

void dumpLines(const Comment& c, const char* where) {
    std::fprintf(stderr, "  %s:", where);
    for (const coach::Line& l : c.lines) std::fprintf(stderr, " %s", l.key.c_str());
    std::fprintf(stderr, "\n");
}

}  // namespace

// ---- Material along the line ---------------------------------------------------------------------

TEST(coach_core_review_comment_pawn_lost_along_the_line) {
    // 1.Nb5? leaves e4 to the knight: 1...Nxe4. The mover could take a pawn back at once (Nxc7 or
    // Nxa7), but the line shows it never does (2.Kf1 c6 3.Nd4 Nd6, still a pawn down): the pawn is
    // lost, said with the line that wins it.
    const char* fen = "6k1/ppp2ppp/5n2/8/4P3/2N5/5PPP/6K1 w - - 0 1";
    {
        GameReview r = reviewOf(fen, {"Nb5"});
        r.accept(0, an(r, 0, {{30, "f2f3 g8f8"}}));
        r.accept(1, an(r, 1, {{90, "f6e4 g1f1 c7c6 b5d4 e4d6"}}));
        Commentator c;
        c.reset(GameInfo());
        CHECK(c.ready(r, 1));
        const Comment k = c.commentAt(r, 1);
        checkComment(k, "pawn lost");
        CHECK_EQ(r.verdict(0).nag, Nag::Mistake);
        REQUIRE(!k.lines.empty());
        CHECK_EQ(k.lines[0].key, std::string("an.mistake"));
        const coach::Line* m = lineOf(k, "an.material");
        CHECK(m != nullptr);
        if (!m) dumpLines(k, "pawn lost");
        if (m) {
            CHECK_EQ(m->arg("pts")->number, 1);
            CHECK_EQ(m->arg("line")->san, std::string("Nxe4"));
        }
        CHECK(hasMark(k, coach::Mark::Kind::Arrow, sq("f6"), sq("e4")));
        CHECK(lineOf(k, "an.hanging") == nullptr);   // a pawn is not "left hanging"
        const coach::Line* better = lineOf(k, "an.better");
        CHECK(better && better->arg("best")->san == "f3");
    }
    // The same capture answered at once by 2.Nxc7: a pawn for a pawn, no material lost to speak of.
    {
        GameReview r = reviewOf(fen, {"Nb5"});
        r.accept(0, an(r, 0, {{30, "f2f3 g8f8"}}));
        r.accept(1, an(r, 1, {{90, "f6e4 b5c7 g8f8"}}));
        Commentator c;
        c.reset(GameInfo());
        const Comment k = c.commentAt(r, 1);
        checkComment(k, "pawn traded");
        CHECK(lineOf(k, "an.mistake") != nullptr);
        CHECK(lineOf(k, "an.material") == nullptr);
        CHECK(lineOf(k, "an.better") != nullptr);
    }
}

// ---- Mates in two -------------------------------------------------------------------------------

TEST(coach_core_review_comment_mate_in_two_allowed_and_missed) {
    // 1...a6?? lets White mate in two: 2.Rd8 Rxd8 3.Rxd8#.
    {
        GameReview r = reviewOf("4r1k1/p4ppp/8/8/8/8/3R1PPP/3R2K1 b - - 0 1", {"a6", "Rd8", "Rxd8", "Rxd8#"});
        r.accept(0, an(r, 0, {{-300, "h7h6 d2d7"}}));
        r.accept(1, an(r, 1, {{0, "d2d8 e8d8 d1d8", 2}}));
        r.accept(2, an(r, 2, {{0, "e8d8 d1d8", -1}}));
        r.accept(3, an(r, 3, {{0, "d1d8", 1}}));
        Commentator c;
        c.reset(GameInfo());
        const Comment k = c.commentAt(r, 1);
        checkComment(k, "mate in two allowed");
        CHECK(lineOf(k, "an.blunder") != nullptr);
        const coach::Line* m = lineOf(k, "an.mate_allowed");
        CHECK(m != nullptr);
        if (!m) dumpLines(k, "mate in two allowed");
        if (m) {
            CHECK_EQ(m->arg("m")->number, 2);
            CHECK_EQ(m->arg("reply")->san, std::string("Rd8"));
        }
        CHECK(lineOf(k, "an.mate_allowed.one") == nullptr);
        CHECK(hasMark(k, coach::Mark::Kind::Arrow, sq("d2"), sq("d8")));
        // The mate is said: the next position does not announce it again.
        CHECK(lineOf(c.commentAt(r, 2), "an.mate_on.white") == nullptr);
    }
    // White to move misses it: 1.h3?.
    {
        GameReview r = reviewOf("4r1k1/p4ppp/8/8/8/8/3R1PPP/3R2K1 w - - 0 1", {"h3"});
        r.accept(0, an(r, 0, {{0, "d2d8 e8d8 d1d8", 2}, {300, "h2h3 h7h6"}}));
        r.accept(1, an(r, 1, {{-300, "h7h6 d2d7"}}));
        Commentator c;
        c.reset(GameInfo());
        const Comment k = c.commentAt(r, 1);
        checkComment(k, "mate in two missed");
        const coach::Line* m = lineOf(k, "an.mate_missed");
        CHECK(m != nullptr);
        if (!m) dumpLines(k, "mate in two missed");
        if (m) {
            CHECK_EQ(m->arg("m")->number, 2);
            CHECK_EQ(m->arg("best")->san, std::string("Rd8"));
        }
        CHECK(lineOf(k, "an.better") == nullptr);   // the mate named the better move
    }
}

// ---- A fork missed ------------------------------------------------------------------------------

TEST(coach_core_review_comment_fork_missed_target_taken) {
    // 1.Qxc8? takes the queen that 1.Ne7+ would have won with the king forked: the engine's line
    // wins far more. The king is marked where it stands; c8, where the queen stood, as a square.
    GameReview r = reviewOf("r1q2rk1/5ppp/8/3N4/8/7P/2Q2PP1/3R2K1 w - - 0 1", {"Qxc8"});
    r.accept(0, an(r, 0, {{708, "d5e7 g8h8 e7c8 a8c8 c2f5 c8c7"}, {-13, "c2c8 a8c8 d5e7 g8h8"}}));
    r.accept(1, an(r, 1, {{13, "a8c8 d5e7 g8h8 e7c8 f8c8"}}));
    Commentator c;
    c.reset(GameInfo());
    const Comment k = c.commentAt(r, 1);
    checkComment(k, "fork missed");
    CHECK_EQ(r.verdict(0).nag, Nag::Blunder);
    const coach::Line* f = lineOf(k, "an.missed_fork");
    CHECK(f != nullptr);
    if (!f) {
        dumpLines(k, "fork missed");
        return;
    }
    CHECK_EQ(f->arg("best")->san, std::string("Ne7+"));
    CHECK_EQ(f->arg("t1")->piece, King);
    CHECK_EQ(f->arg("t1")->color, Black);
    CHECK(f->arg("t1")->bySide);
    CHECK_EQ(f->arg("t2")->piece, Queen);
    CHECK_EQ(f->arg("t2")->square, sq("c8"));
    CHECK(hasMark(k, coach::Mark::Kind::Arrow, sq("d5"), sq("e7")));
    CHECK(hasMark(k, coach::Mark::Kind::Piece, sq("g8")));
    CHECK(hasMark(k, coach::Mark::Kind::Square, sq("c8")));
    CHECK(!hasMark(k, coach::Mark::Kind::Piece, sq("c8")));
    CHECK(lineOf(k, "an.better") == nullptr);
}

// ---- Good moves ---------------------------------------------------------------------------------

TEST(coach_core_review_comment_brilliant_sacrifice_that_mates) {
    // 1.Qg8+!! Rxg8 2.Nf7#: the quick search before the move saw +4 only, the one after it the mate.
    GameReview r = reviewOf("5r1k/p5pp/7N/3Q4/8/8/PP4PP/6K1 w - - 0 1", {"Qg8+", "Rxg8", "Nf7#"});
    r.accept(0, an(r, 0, {{400, "d5g8 f8g8 h6f7"}, {200, "h6f7 f8f7"}}));
    r.accept(1, an(r, 1, {{0, "f8g8 h6f7", -1}}));
    r.accept(2, an(r, 2, {{0, "h6f7", 1}}));
    CHECK_EQ(r.verdict(0).nag, Nag::Brilliant);
    Commentator c;
    c.reset(GameInfo());
    const Comment k = c.commentAt(r, 1);
    checkComment(k, "brilliant mate");
    const coach::Line* b = lineOf(k, "an.brilliant");
    CHECK(b != nullptr);
    if (b) {
        CHECK_EQ(b->arg("piece")->piece, Queen);
        CHECK_EQ(b->arg("piece")->square, sq("g8"));
    }
    const coach::Line* m = lineOf(k, "an.brilliant.mate");
    CHECK(m != nullptr);
    if (m) CHECK_EQ(m->arg("m")->number, 1);
    CHECK(lineOf(k, "an.mate_on.white") == nullptr);   // said once
    CHECK_EQ(k.weight, 3);
}

TEST(coach_core_review_comment_only_move_that_holds) {
    // Worse already, 1.h3 is the only move that keeps the game: the second best loses far more.
    GameReview r = reviewOf("6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1", {"h3"});
    r.accept(0, an(r, 0, {{-150, "h2h3 g8f8"}, {-600, "g1f1 g8f8"}}));
    r.accept(1, an(r, 1, {{150, "g8f8"}}));
    CHECK_EQ(r.verdict(0).nag, Nag::Good);
    Commentator c;
    c.reset(GameInfo());
    const Comment k = c.commentAt(r, 1);
    checkComment(k, "only defence");
    const coach::Line* o = lineOf(k, "an.only.defend");
    CHECK(o != nullptr);
    if (o) CHECK_EQ(o->arg("move")->san, std::string("h3"));
    if (!o) dumpLines(k, "only defence");
}

// ---- The end of the game -------------------------------------------------------------------------

TEST(coach_core_review_comment_end_of_the_game) {
    // The board first: a stalemate on the board is said whatever the record says.
    {
        GameReview r = reviewOf("7k/5K2/8/6Q1/8/8/8/8 w - - 0 1", {"Qg6"});
        GameInfo info;
        info.result = "1/2-1/2";
        info.endReasonKey = chess::endReasonKey(GameEndReason::Stalemate);
        Commentator c;
        c.reset(info);
        const Comment k = c.commentAt(r, 1);
        checkComment(k, "stalemate");
        REQUIRE(!k.lines.empty());
        CHECK_EQ(k.lines[0].key, std::string("an.end.stalemate"));
    }
    // Otherwise the record's result and reason (the chess module's own keys).
    struct Case {
        const char* result;
        GameEndReason reason;
        const char* key;
    };
    const Case cases[] = {
        {"1-0", GameEndReason::Timeout, "an.end.timeout.white"},
        {"0-1", GameEndReason::Timeout, "an.end.timeout.black"},
        {"0-1", GameEndReason::IllegalMoves, "an.end.win.black"},
        {"1-0", GameEndReason::None, "an.end.win.white"},
        {"1/2-1/2", GameEndReason::Agreement, "an.end.draw.agreement"},
        {"1/2-1/2", GameEndReason::ThreefoldClaim, "an.end.draw.repetition"},
        {"1/2-1/2", GameEndReason::FivefoldRepetition, "an.end.draw.repetition"},
        {"1/2-1/2", GameEndReason::FiftyMoveClaim, "an.end.draw.fifty"},
        {"1/2-1/2", GameEndReason::SeventyFiveMoves, "an.end.draw.seventy_five"},
        {"1/2-1/2", GameEndReason::InsufficientMaterial, "an.end.draw.material"},
        {"1/2-1/2", GameEndReason::TimeoutVsInsufficient, "an.end.draw"},
        {"1/2-1/2", GameEndReason::None, "an.end.draw"},
    };
    GameReview r = reviewOf(nullptr, {"e4", "e5"});
    for (const Case& k : cases) {
        GameInfo info;
        info.result = k.result;
        info.endReasonKey = chess::endReasonKey(k.reason);
        Commentator c;
        c.reset(info);
        const Comment end = c.commentAt(r, 2);
        checkComment(end, k.key);
        REQUIRE(!end.lines.empty());
        CHECK_EQ(end.lines.back().key, std::string(k.key));
        CHECK_EQ(end.weight, 3);
    }
}

TEST(coach_core_review_comment_wait_ends_when_the_board_moves) {
    GameReview r = reviewOf(nullptr, {"e4", "e5", "Nf3"});
    Commentator c;
    c.reset(GameInfo());
    CommentWait wait;
    wait.ask(2);
    CHECK(wait.waitingComment());
    CHECK(!wait.urgent(r, c).empty());   // its positions are not analysed yet
    Comment out;
    // The board stepped on to position 3 before the comment of position 2 was decided.
    CHECK(!wait.poll(r, c, 3, false, out));
    CHECK(!wait.waiting());
    CHECK_EQ(wait.position(), -1);
    CHECK(out.empty());
    CHECK(wait.urgent(r, c).empty());
    // Nor does a position beyond the game wait.
    wait.ask(9);
    CHECK(!wait.poll(r, c, 9, false, out));
    CHECK(!wait.waiting());
}

// ---- The cache ------------------------------------------------------------------------------------

namespace {

struct TempFolder {
    fs::path path;
    explicit TempFolder(const char* tag) {
#ifdef _WIN32
        const unsigned pid = unsigned(_getpid());
#else
        const unsigned pid = unsigned(getpid());
#endif
        path = fs::u8path(net::sys::exeDirectory()) / ("core-review-cache-" + std::string(tag) + "-" + std::to_string(pid));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
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

std::string cacheName(int i) {
    char name[32];
    std::snprintf(name, sizeof name, "%016x.txt", i);
    return name;
}

}  // namespace

TEST(coach_core_review_cache_trim_ties_by_name) {
    // 205 games last used at the same moment: the trim keeps the newest file and the 199 that come
    // first by name, whatever order the folder lists them in.
    TempFolder tmp("ties");
    const auto when = fs::file_time_type::clock::now() - std::chrono::hours(100);
    for (int i = 205; i >= 1; --i) {
        const fs::path p = tmp.path / cacheName(i);
        std::ofstream(p, std::ios::binary) << "old";
        std::error_code ec;
        fs::last_write_time(p, when, ec);
        CHECK(!ec);
    }
    GameReview r = reviewOf(nullptr, {"e4"});
    r.accept(0, an(r, 0, {{20, "e2e4"}}));
    CHECK(analysis::saveCache(tmp.folder(), r.key(), r.evaluations()));
    const std::vector<std::string> names = tmp.names();
    CHECK_EQ(int(names.size()), 200);
    CHECK(std::binary_search(names.begin(), names.end(), r.key() + ".txt"));
    for (int i = 1; i <= 205; ++i)
        CHECK_EQ(std::binary_search(names.begin(), names.end(), cacheName(i)), i <= 199);
}

TEST(coach_core_review_cache_cannot_write) {
    // A folder stands where the game's file would go: the save fails, says so in the log, and
    // leaves the folder as it was.
    TempFolder tmp("blocked");
    GameReview r = reviewOf(nullptr, {"e4"});
    r.accept(0, an(r, 0, {{20, "e2e4"}}));
    const std::string key = r.key();
    const fs::path blocker = tmp.path / (key + ".txt");
    fs::create_directories(blocker / "inside");
    const fs::path log = tmp.path / "save.log";
    CHECK(logx::init(log.u8string().c_str()));
    const bool saved = analysis::saveCache(tmp.folder(), key, r.evaluations());
    logx::shutdown();
    CHECK(!saved);
    CHECK(fs::is_directory(blocker / "inside"));
    std::ifstream in(log, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    CHECK(text.str().find("analysis cache: cannot write") != std::string::npos);
    std::vector<PositionEval> back;
    CHECK(!analysis::loadCache(tmp.folder(), key, r.positions(), back));
}
