// Tests for the Analysis mode's comments on a first review (audit N01): the review's scheduler
// (analysis::GameReview::nextRequest and its urgent positions), the commentator's needs and
// analysis::CommentWait, driven together as the scene drives them, by a fake engine that answers
// one search at a time after a simulated delay (no Stockfish, no wall clock). A comment asked on a
// position is kept while that position stays on the board, decided from final evaluations only and
// said exactly once; its evaluations are searched before the rest of the review; Play waits for it;
// the end of a long game is commented without waiting for the whole review (each side's accuracy
// follows once the review is complete); a step elsewhere drops the comment waited for. The bench
// of the audit (80 plies, the comment of the position after 1.f3 e5 2.g4) is kept as a regression.
#include "test.h"

#include "ai/analysis.h"
#include "analysis/commentary.h"
#include "analysis/review.h"
#include "chess/chess.h"

#include <algorithm>
#include <random>
#include <string>
#include <vector>

using namespace chess;
using analysis::Comment;
using analysis::Commentator;
using analysis::CommentWait;
using analysis::GameInfo;
using analysis::GameReview;

namespace {

// The moves of a game: the given opening (SAN), then random legal moves (seeded) up to 'plies'
// moves or the end of the game.
std::vector<Move> gameOf(std::initializer_list<const char*> opening, int plies, unsigned seed) {
    Position p;
    std::vector<Move> moves;
    for (const char* san : opening) {
        const Move m = p.parseSAN(san);
        CHECK(m.valid());
        if (!m.valid()) return moves;
        moves.push_back(m);
        p.makeMove(m);
    }
    std::mt19937 rng(seed);
    while (int(moves.size()) < plies) {
        const std::vector<Move> legal = p.legalMoves();
        if (legal.empty()) break;
        const Move m = legal[rng() % legal.size()];
        moves.push_back(m);
        p.makeMove(m);
    }
    return moves;
}

// What the fake engine answers for position i at a depth: legal, the same at any time. 'swings':
// deep scores that move from position to position (mistakes and blunders to comment on) and a quick
// pass that differs from them (another best move, a level score), so that a comment decided from a
// provisional evaluation shows. Without it, every position is level with its first legal move,
// but for the bench's: Black mates at once after 1.f3 e5 2.g4.
ai::Analysis answer(const GameReview& r, int i, int depth, bool swings) {
    const Position& p = r.positionAt(i);
    const std::vector<Move> legal = p.legalMoves();
    ai::Analysis a;
    a.ok = true;
    a.whiteToMove = p.sideToMove() == White;
    a.depth = depth;
    ai::PvLine l;
    l.multipv = 1;
    l.depth = depth;
    const bool deep = depth >= r.settings().deepDepth;
    if (legal.empty()) return a;
    if (swings) {
        l.pv = {p.toUCI(legal[deep ? (size_t(i) * 31u) % legal.size() : 0])};
        l.score.cp = deep ? int((unsigned(i) * 7919u + 13u) % 601u) - 300 : 0;
    } else if (i == 3 && p.parseUCI("d8h4").valid()) {
        l.score.mate = 1;
        l.pv = {"d8h4"};
    } else {
        l.pv = {p.toUCI(legal.front())};
    }
    a.bestMove = l.pv.front();
    a.lines.push_back(l);
    return a;
}

// The scene's loop around one game (game_scene_analysis.cpp's updateAnalysis): one search at a
// time from the fake engine, the board, Play, the comment waited for, the comments said (a
// second a line).
struct Table {
    GameReview review;
    Commentator commentator;
    CommentWait wait;
    int quickMs = 200, deepMs = 500;
    bool swings = false;
    int board = 0;
    bool playing = false;
    long nowMs = 0, busyUntil = 0, speakingUntil = 0;
    int reqPos = -1, reqDepth = 0;
    int requests = 0, quickRequests = 0, firstDeep = 0;   // firstDeep: its request's number
    std::vector<int> handedOut;                           // the positions searched, in order
    std::vector<Comment> said;

    Table(const std::vector<Move>& moves, const GameInfo& info, int quick, int deep, bool sw)
        : quickMs(quick), deepMs(deep), swings(sw) {
        review.reset(Position(), moves);
        commentator.reset(info);
    }
    int plies() const { return review.plies(); }

    // One frame of 'dtMs'.
    void frame(long dtMs = 100) {
        if (reqPos < 0) {
            std::vector<int> urgent = wait.urgent(review, commentator);
            if (playing && board < plies())
                for (const int i : commentator.needs(review, board + 1))
                    if (std::find(urgent.begin(), urgent.end(), i) == urgent.end()) urgent.push_back(i);
            ai::AnalysisRequest q;
            int pos = -1;
            if (review.nextRequest(board, q, pos, urgent)) {
                ++requests;
                const bool deep = q.depth == review.settings().deepDepth;
                if (!deep) ++quickRequests;
                if (deep && !firstDeep) firstDeep = requests;
                reqPos = pos;
                reqDepth = q.depth;
                busyUntil = nowMs + (deep ? deepMs : quickMs);
                handedOut.push_back(pos);
            }
        }
        nowMs += dtMs;
        if (reqPos >= 0 && nowMs >= busyUntil) {
            review.accept(reqPos, answer(review, reqPos, reqDepth, swings));
            reqPos = -1;
        }
        Comment c;
        if (wait.waiting() && wait.poll(review, commentator, board, nowMs < speakingUntil, c)) {
            said.push_back(c);
            speakingUntil = nowMs + 1000L * long(c.lines.size());
        }
        if (playing) {
            if (board >= plies()) playing = false;
            else if (!wait.waitingComment() && nowMs >= speakingUntil) forward();
        }
    }
    void run(long ms) {
        const long end = nowMs + ms;
        while (nowMs < end) frame();
    }
    // Until 'done' or 'ms' of simulated time; whether 'done' came.
    template <class F> bool runUntil(F done, long ms) {
        const long end = nowMs + ms;
        while (!done() && nowMs < end) frame();
        return done();
    }

    // The player's steps (analysisGoTo, analysisStepDone): forward asks for the comment, a step
    // back or a jump drops the one waited for (and the speech).
    void forward() {
        ++board;
        speakingUntil = nowMs;
        wait.clear();
        wait.ask(board);
    }
    void back() {
        --board;
        speakingUntil = nowMs;
        wait.clear();
    }
    void jump(int p) {
        board = p;
        speakingUntil = nowMs;
        wait.clear();
    }
    int saidAt(int p) const {
        int n = 0;
        for (const Comment& c : said) n += c.position == p ? 1 : 0;
        return n;
    }
};

std::vector<std::string> keysOf(const Comment& c) {
    std::vector<std::string> out;
    for (const coach::Line& l : c.lines) out.push_back(l.key);
    return out;
}

bool hasKey(const Comment& c, const std::string& key) {
    for (const coach::Line& l : c.lines)
        if (l.key == key) return true;
    return false;
}

bool isAccount(const std::string& key) {
    return key == "an.end.accuracy" || key.compare(0, 10, "an.end.cle") == 0 || key.compare(0, 10, "an.end.mis") == 0 ||
           key.compare(0, 10, "an.end.blu") == 0 || key.compare(0, 10, "an.end.err") == 0;
}

// The comments of a whole review, every position final (the reference: what a comment says when
// nothing is provisional).
std::vector<Comment> reference(const std::vector<Move>& moves, const GameInfo& info, bool swings) {
    GameReview r;
    r.reset(Position(), moves);
    for (int i = 0; i < r.positions(); ++i)
        if (!r.position(i).final) r.accept(i, answer(r, i, r.settings().deepDepth, swings));
    CHECK(r.complete());
    Commentator c;
    c.reset(info);
    std::vector<Comment> out;
    for (int p = 0; p <= r.plies(); ++p) out.push_back(c.commentAt(r, p));
    return out;
}

GameInfo unfinished() {
    GameInfo info;
    info.result = "*";
    return info;
}

}  // namespace

// The audit's bench (N01), with the scene's comment wait: the comment of the position after
// 1.f3 e5 2.g4 in an 80-ply game reviewed without a cache. The old order ran 81 quick searches
// before the first deep one and the comment was ready after 88 searches (19.7 s at 200 ms a quick
// search, 500 ms a deep one), long after the scene had left it out.
TEST(analysis_comment_wait_audit_bench) {
    const std::vector<Move> moves = gameOf({"f3", "e5", "g4", "Nc6"}, 80, 11);
    REQUIRE(moves.size() == 80);
    Table t(moves, unfinished(), 200, 500, false);
    t.board = 3;
    t.wait.ask(3);
    CHECK(t.wait.urgent(t.review, t.commentator) == std::vector<int>({3, 2, 1, 0}));
    CHECK(!t.commentator.ready(t.review, 3));
    REQUIRE(t.runUntil([&] { return !t.said.empty(); }, 60000));
    // Four quick searches (the bar and the symbols), then the four deep ones the comment reads.
    CHECK_EQ(t.firstDeep, 5);
    CHECK_EQ(t.requests, 8);
    CHECK(t.nowMs <= 2900);
    CHECK(t.review.progress() < 0.06f);
    const Comment& c = t.said[0];
    CHECK_EQ(c.position, 3);
    CHECK(keysOf(c) == std::vector<std::string>({"an.blunder", "an.mate_allowed.one", "an.better"}));
    CHECK(!t.wait.waiting());
    // The rest of the review changes nothing: said once.
    REQUIRE(t.runUntil([&] { return t.review.complete(); }, 600000));
    t.run(2000);
    CHECK_EQ(t.saidAt(3), 1);
    CHECK_EQ(int(t.said.size()), 1);
}

// A slow engine (the configured caps: 800 ms a quick search, 6 s a deep one): the comment waits as
// long as its position stays on the board, far beyond the old 2.5 s (stepping) and 12 s (Play), and
// is said once, from final evaluations.
TEST(analysis_comment_wait_slow_engine_keeps_the_comment) {
    const std::vector<Move> moves = gameOf({"f3", "e5", "g4", "Nc6"}, 80, 11);
    Table t(moves, unfinished(), 800, 6000, false);
    t.jump(2);
    t.run(300);   // a quick search runs for the board when the step comes
    t.forward();
    CHECK_EQ(t.board, 3);
    t.run(12500);
    CHECK(t.said.empty());   // not decided yet ...
    CHECK(t.wait.waitingComment());   // ... and still waited for
    REQUIRE(t.runUntil([&] { return !t.said.empty(); }, 60000));
    CHECK(t.nowMs < 30000);
    CHECK_EQ(t.said[0].position, 3);
    CHECK(hasKey(t.said[0], "an.mate_allowed.one"));
    for (int i = 0; i <= 3; ++i) CHECK(t.review.position(i).final);
    REQUIRE(t.runUntil([&] { return t.review.complete(); }, 3600000));
    t.run(2000);
    CHECK_EQ(int(t.said.size()), 1);
}

// The board moves while a comment waits: a step forward asks for the next one and drops this one
// (never said later, on another position), a step back or a jump drops it; the searches follow the
// new comment at once.
TEST(analysis_comment_wait_position_changed) {
    const std::vector<Move> moves = gameOf({"f3", "e5", "g4", "Nc6"}, 80, 11);
    Table t(moves, unfinished(), 200, 3000, false);
    t.jump(2);
    t.forward();   // position 3 asked for
    t.run(1500);   // its quick searches done, a deep one running
    CHECK(t.said.empty());
    t.forward();   // position 4 before the comment of 3 was decided
    CHECK_EQ(t.wait.position(), 4);
    CHECK(t.wait.urgent(t.review, t.commentator) == t.commentator.needs(t.review, 4));
    // The next searches handed out are the comment of 4's.
    const size_t from = t.handedOut.size();
    REQUIRE(t.runUntil([&] { return !t.said.empty(); }, 120000));
    for (size_t k = from; k < t.handedOut.size(); ++k) CHECK(t.handedOut[k] >= 1 && t.handedOut[k] <= 4);
    CHECK_EQ(t.said[0].position, 4);
    CHECK_EQ(t.saidAt(3), 0);
    // Back to 3, by a step back: no comment (only forward steps comment), nothing waited for.
    t.back();
    CHECK(!t.wait.waiting());
    // Forward to 4 again: a new step, its comment again (decided already: at once).
    t.forward();
    t.frame();
    CHECK_EQ(t.saidAt(4), 2);
    // To 5, and back before its comment is decided: nothing said there, ever.
    CHECK(!t.commentator.ready(t.review, 5));
    t.forward();
    t.frame();
    t.back();
    CHECK(!t.wait.waiting());
    REQUIRE(t.runUntil([&] { return t.review.complete(); }, 3600000));
    t.run(2000);
    CHECK_EQ(int(t.said.size()), 2);
    CHECK_EQ(t.saidAt(5), 0);
    CHECK_EQ(t.saidAt(3), 0);
}

// The end of a long game, reached by a jump and a step forward before its review got there: the
// final position's comment (its move, how the game ended) comes after the searches of its last
// four positions, not after the whole review; each side's accuracy and errors, which need the
// whole review, follow once it is complete, after that comment is said, exactly once; together
// they are the comment the complete review gives. Left before the review is complete, the
// accounts are never said.
TEST(analysis_comment_wait_jump_to_the_end_of_a_long_game) {
    const std::vector<Move> moves = gameOf({"e4", "e5"}, 160, 7);
    REQUIRE(moves.size() >= 100);
    const GameInfo info = unfinished();
    const std::vector<Comment> ref = reference(moves, info, true);
    {
        Table t(moves, info, 200, 1500, true);
        const int end = t.plies();
        t.jump(end);
        t.run(1000);
        t.back();
        t.forward();
        REQUIRE(t.runUntil([&] { return !t.said.empty(); }, 60000));
        CHECK(t.requests <= 12);   // the board's quick searches, then the four deep ones
        CHECK(t.nowMs < 10000);
        CHECK(!t.review.complete());
        const Comment main = t.said[0];
        CHECK_EQ(main.position, end);
        REQUIRE(!main.lines.empty());
        CHECK(main.lines.back().key.compare(0, 7, "an.end.") == 0);   // how the game ended
        for (const coach::Line& l : main.lines) CHECK(!isAccount(l.key));
        CHECK(t.wait.waiting() && !t.wait.waitingComment());   // the accounts wait for the review
        CHECK(t.wait.urgent(t.review, t.commentator).empty());
        REQUIRE(t.runUntil([&] { return t.said.size() == 2; }, 3600000));
        CHECK(t.review.complete());
        const Comment& accounts = t.said[1];
        CHECK_EQ(accounts.position, end);
        REQUIRE(!accounts.lines.empty());
        for (const coach::Line& l : accounts.lines) CHECK(isAccount(l.key));
        std::vector<std::string> both = keysOf(main), more = keysOf(accounts);
        both.insert(both.end(), more.begin(), more.end());
        CHECK(both == keysOf(ref[size_t(end)]));
        t.run(5000);
        CHECK_EQ(int(t.said.size()), 2);
        CHECK(!t.wait.waiting());
    }
    {
        // Left for the position before (a step back) once the comment is said: no accounts, even
        // when the step forward comes back after the review is complete (the whole comment then).
        Table t(moves, info, 200, 1500, true);
        const int end = t.plies();
        t.jump(end - 1);
        t.forward();
        REQUIRE(t.runUntil([&] { return !t.said.empty(); }, 60000));
        t.back();
        REQUIRE(t.runUntil([&] { return t.review.complete(); }, 3600000));
        t.run(3000);
        CHECK_EQ(int(t.said.size()), 1);
        t.forward();
        t.run(200);
        REQUIRE(t.said.size() == 2);
        CHECK(keysOf(t.said[1]) == keysOf(ref[size_t(end)]));
    }
}

// Play from the start, without a cache, the deep searches slower than a comment: every comment the
// complete review gives is said once, in order, the same (nothing decided from the quick pass),
// none skipped: Play waits for each.
TEST(analysis_comment_wait_play_says_every_comment_once) {
    const std::vector<Move> moves = gameOf({"d4", "d5", "c4"}, 40, 5);
    const GameInfo info = unfinished();
    const std::vector<Comment> ref = reference(moves, info, true);
    Table t(moves, info, 200, 2500, true);
    t.wait.ask(0);   // the welcome, then the moves
    t.playing = true;
    REQUIRE(t.runUntil([&] { return !t.playing && !t.wait.waiting() && t.nowMs >= t.speakingUntil; }, 3600000));
    CHECK_EQ(t.board, t.plies());
    std::vector<int> expected;
    for (int p = 0; p <= t.plies(); ++p)
        if (!ref[size_t(p)].empty()) expected.push_back(p);
    CHECK(expected.size() >= 8);   // a game with plenty to say
    std::vector<int> got;
    std::vector<std::string> endKeys;
    for (const Comment& c : t.said) {
        if (c.position == t.plies()) {
            const std::vector<std::string> k = keysOf(c);
            endKeys.insert(endKeys.end(), k.begin(), k.end());
            if (!got.empty() && got.back() == c.position) continue;   // the accounts after it
        } else if (c.position >= 0 && c.position < int(ref.size())) {
            CHECK(keysOf(c) == keysOf(ref[size_t(c.position)]));
            CHECK_EQ(c.marks.size(), ref[size_t(c.position)].marks.size());
        }
        got.push_back(c.position);
    }
    CHECK(got == expected);
    CHECK(endKeys == keysOf(ref[size_t(t.plies())]));
}
