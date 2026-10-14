// Tests for parts of the coach's end-of-game appraisal (coach/appraisal.h) that the appraisal tests
// leave aside: the reason a draw is named by (insufficient material, also after a flag fall, a
// resignation or a forfeit; repetition, claimed or the fivefold; the fifty and seventy-five move
// rules), the opener of a loss that came late (levels 3-4), a good capture as the best moment, the
// takebacks counted when one fixed a move, the level-2 count of top moves after many mistakes, and
// the recurring theme said instead of (level 1) or beside (level 4) the critical moment. The
// verdicts are made up as the review would store them.
#include "coach_core_review_support.h"

#include "coach/appraisal.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace chess;
using namespace coach;
using namespace corereview;

namespace {

// A verdict for the move at 'ply' of 'g', as the review stores it (the evaluations White's view).
Review verdictAt(const Game& g, int ply, MoveClass cls, int cpBefore, int cpAfter, ExType type = ExType::None) {
    Review r;
    PlyVerdict& v = r.verdict;
    const Position& p0 = g.positionAt(size_t(ply));
    v.ply = ply;
    v.mover = p0.sideToMove();
    v.human = true;
    v.uci = p0.toUCI(g.moves()[size_t(ply)]);
    v.san = g.sanMoves()[size_t(ply)];
    v.cls = cls;
    const int sign = v.mover == White ? 1 : -1;
    v.wBest = winPercent(sign * cpBefore);
    v.wPlayed = winPercent(sign * cpAfter);
    v.delta = (std::max)(0.0, v.wBest - v.wPlayed);
    v.accuracy = moveAccuracy(v.wBest, v.wPlayed);
    v.cpWhiteBefore = cpBefore;
    v.hasEvalBefore = true;
    v.cpWhiteAfter = cpAfter;
    v.hasEvalAfter = true;
    v.bestUci = v.uci;
    v.bestSan = v.san;
    v.exType = type;
    v.check = g.positionAt(size_t(ply) + 1).inCheck();
    return r;
}

// K+R against K: the rook walks a1-f1-f6 while the black king shuffles, then Rf8 mates (11 White
// moves), from move 'number' on.
Game rookGame(int number = 1) {
    const std::string fen = "7k/8/6K1/8/8/8/8/R7 w - - 0 " + std::to_string(number);
    return gameOf(fen.c_str(), {"Rb1", "Kg8", "Rc1", "Kh8", "Rd1", "Kg8", "Re1", "Kh8", "Rf1", "Kg8", "Rf2", "Kh8",
                                "Rf3", "Kg8", "Rf4", "Kh8", "Rf5", "Kg8", "Rf6", "Kh8", "Rf8#"});
}

// The appraisal's own rules on top of the English lines: every text argument names a line.
void checkAppraisal(const Script& s, const char* where) {
    CHECK(!s.empty());
    checkScript(s, where);
    const auto& en = englishLines();
    for (const Beat& b : s) {
        CHECK(b.skippable);
        for (const auto& a : b.line.args)
            if (a.second.kind == Arg::Kind::Text && !en.count(a.second.text)) {
                std::fprintf(stderr, "  %s: %s: text key %s missing\n", where, b.line.key.c_str(), a.second.text.c_str());
                CHECK(false);
            }
    }
}

const Beat* beatWithPrefix(const Script& s, const std::string& prefix) {
    for (const Beat& b : s)
        if (b.line.key.rfind(prefix, 0) == 0) return &b;
    return nullptr;
}

}  // namespace

TEST(coach_core_review_appraisal_draw_reasons) {
    struct Case {
        const char* name;
        Game game;
        GameEndReason reason;
        const char* key;
    };
    const char* rookVsKing = "4k3/8/8/8/8/8/8/4KR2 w - - 0 1";
    std::vector<Case> cases;
    cases.push_back({"bare kings", gameOf("4k3/8/8/8/8/8/3r4/4K3 w - - 0 1", {"Kxd2"}), GameEndReason::InsufficientMaterial,
                     "appraisal.reason.material"});
    {
        Game g = gameOf(rookVsKing, {"Rf2"});
        g.flagFall(White);   // Black's bare king cannot mate
        cases.push_back({"flag fall", g, GameEndReason::TimeoutVsInsufficient, "appraisal.reason.material"});
    }
    {
        Game g = gameOf(rookVsKing, {"Rf2"});
        g.forfeitIllegal(White);
        cases.push_back({"illegal moves", g, GameEndReason::IllegalMovesVsInsufficient, "appraisal.reason.material"});
    }
    {
        Game g = gameOf(rookVsKing, {"Rf2"});
        g.resign(White);
        cases.push_back({"resignation", g, GameEndReason::ResignationVsInsufficient, "appraisal.reason.material"});
    }
    {
        Game g = gameOf(nullptr, {"Nf3", "Nf6", "Ng1", "Ng8", "Nf3", "Nf6", "Ng1", "Ng8"});
        CHECK(g.canClaimThreefold());
        g.claimDraw();
        cases.push_back({"threefold", g, GameEndReason::ThreefoldClaim, "appraisal.reason.repetition"});
    }
    cases.push_back({"fivefold",
                     gameOf(nullptr, {"Nf3", "Nf6", "Ng1", "Ng8", "Nf3", "Nf6", "Ng1", "Ng8", "Nf3", "Nf6", "Ng1", "Ng8",
                                      "Nf3", "Nf6", "Ng1", "Ng8"}),
                     GameEndReason::FivefoldRepetition, "appraisal.reason.repetition"});
    {
        Game g = gameOf("4k3/8/8/8/8/8/4P3/R3K3 w - - 99 80", {"Ra2"});
        CHECK(g.canClaimFiftyMove());
        g.claimDraw();
        cases.push_back({"fifty", g, GameEndReason::FiftyMoveClaim, "appraisal.reason.fifty"});
    }
    cases.push_back({"seventy-five", gameOf("4k3/8/8/8/8/8/4P3/R3K3 w - - 149 120", {"Ra2"}),
                     GameEndReason::SeventyFiveMoves, "appraisal.reason.fifty"});
    for (const Case& k : cases) {
        CHECK(k.game.isOver());
        CHECK(k.game.status() == GameStatus::Draw);
        CHECK(k.game.endReason() == k.reason);
        for (int level : {1, 4}) {
            Appraisal a;
            a.reset(level, White);
            a.add(verdictAt(k.game, 0, MoveClass::Best, 0, 0));
            const Script s = a.script(k.game, AppraisalContext{});
            const std::string where = std::string(k.name) + " b" + std::to_string(level);
            checkAppraisal(s, where.c_str());
            CHECK(!s.empty() && s.front().line.key == "appraisal.open.draw.b" + std::to_string(level));
            const Arg* text = s.empty() ? nullptr : s.front().line.arg("text");
            CHECK(text && text->text == k.key);
        }
    }
}

TEST(coach_core_review_appraisal_loss_that_came_late) {
    // The human (Black) is mated; the blunder that turned the game came on move 32 (the game set up
    // from move 30). Levels 3-4 say the game was close for long; the others, a plain loss.
    for (int number : {30, 1}) {
        const Game g = rookGame(number);
        for (int level = 1; level <= 6; ++level) {
            Appraisal a;
            a.reset(level, Black);
            for (int ply = 1; ply < 21; ply += 2) {
                if (ply == 5) a.add(verdictAt(g, ply, MoveClass::Blunder, 0, 600, ExType::Hanging));
                else a.add(verdictAt(g, ply, ply < 5 ? MoveClass::Best : MoveClass::Good, ply < 5 ? 0 : 600, ply < 5 ? 0 : 600));
            }
            CHECK_EQ(a.stats(g).turningPly, 5);
            const Script s = a.script(g, AppraisalContext{});
            const std::string where = "late loss b" + std::to_string(level) + " from move " + std::to_string(number);
            checkAppraisal(s, where.c_str());
            const bool close = (level == 3 || level == 4) && number == 30;
            const std::string opener = (close ? "appraisal.open.loss_close.b" : "appraisal.open.loss.b") + std::to_string(level);
            CHECK(!s.empty() && s.front().line.key == opener);
            // Resigned instead, the resignation opener wins over the close loss.
            AppraisalContext resigned;
            resigned.humanResigned = true;
            const Script r = a.script(g, resigned);
            CHECK(!r.empty() && r.front().line.key == "appraisal.open.resigned.b" + std::to_string(level));
        }
    }
}

TEST(coach_core_review_appraisal_good_capture_highlight) {
    // No brilliant, great or only move, no material won by a best move, no streak of five: the good
    // capture at move 6 (ply 10) is the best moment.
    const Game g = rookGame();
    for (int level = 1; level <= 4; ++level) {
        Appraisal a;
        a.reset(level, White);
        for (int ply = 0; ply < 21; ply += 2) {
            Review r = verdictAt(g, ply, ply == 10 ? MoveClass::Best : MoveClass::Good, 600, ply == 20 ? 1000 : 600);
            r.verdict.goodCapture = ply == 10;
            a.add(r);
        }
        const AppraisalStats st = a.stats(g);
        CHECK(st.bestMoment == BestMoment::GoodCapture);
        CHECK_EQ(st.bestPly, 10);
        const Script s = a.script(g, AppraisalContext{});
        const std::string where = "good capture b" + std::to_string(level);
        checkAppraisal(s, where.c_str());
        const Beat* hl = beatWithKey(s, "appraisal.best.capture.b" + std::to_string(level));
        CHECK(hl != nullptr);
        if (hl) {
            const Arg* move = hl->line.arg("move");
            CHECK(move && move->uci == "f1f2");
            const Arg* n = hl->line.arg("n");
            CHECK(n && n->number == 6);
        }
    }
    // A goodCapture flag on a move that was not a top move is no best moment.
    Appraisal a;
    a.reset(2, White);
    for (int ply = 0; ply < 21; ply += 2) {
        Review r = verdictAt(g, ply, MoveClass::Good, 600, 600);
        r.verdict.goodCapture = ply == 10;
        a.add(r);
    }
    CHECK(a.stats(g).bestMoment != BestMoment::GoodCapture);
}

TEST(coach_core_review_appraisal_takebacks_counted) {
    // A blunder taken back and replaced by a good move: levels 1-3 count the takebacks that helped.
    const Game g = rookGame();
    for (int level = 1; level <= 4; ++level) {
        Appraisal a;
        a.reset(level, White);
        Review first = verdictAt(g, 0, MoveClass::Blunder, 600, 0);
        first.verdict.offered = true;
        a.add(first);
        a.truncate(0);
        for (int ply = 0; ply < 21; ply += 2) {
            Review r = verdictAt(g, ply, ply % 4 == 0 ? MoveClass::Best : MoveClass::Good, 600, ply == 20 ? 1000 : 600);
            if (ply == 0) {
                r.isRetry = true;
                r.takeback.fixed = true;
            }
            a.add(r);
        }
        const AppraisalStats st = a.stats(g);
        CHECK_EQ(st.takebacks, 1);
        CHECK_EQ(st.fixed, 1);
        const Script s = a.script(g, AppraisalContext{});
        const std::string where = "takebacks b" + std::to_string(level);
        checkAppraisal(s, where.c_str());
        const Beat* b = beatWithKey(s, "appraisal.num.takebacks.b" + std::to_string(level));
        CHECK_EQ(b != nullptr, level <= 3);
        if (b) {
            CHECK_EQ(b->line.arg("n")->number, 1);
            CHECK_EQ(b->line.arg("m")->number, 1);
        }
    }
}

TEST(coach_core_review_appraisal_level_two_counts_top_moves) {
    // Level 2, three mistakes and a blunder: no count of faults, the count of top moves instead.
    const Game g = rookGame();
    Appraisal a;
    a.reset(2, White);
    for (int ply = 0; ply < 21; ply += 2) {
        const bool fault = ply == 2 || ply == 6 || ply == 10 || ply == 14;
        a.add(verdictAt(g, ply, ply == 14 ? MoveClass::Blunder : fault ? MoveClass::Mistake : MoveClass::Best, 600,
                        fault ? 550 : 600, fault ? ExType::Positional : ExType::None));
    }
    const AppraisalStats st = a.stats(g);
    CHECK_EQ(st.human.topMoves, 7);
    const Script s = a.script(g, AppraisalContext{});
    checkAppraisal(s, "top moves b2");
    const Beat* b = beatWithKey(s, "appraisal.num.best.b2");
    CHECK(b != nullptr);
    if (b) CHECK_EQ(b->line.arg("n")->number, 7);
    CHECK(!hasKey(s, "appraisal.num.few.b2"));
}

TEST(coach_core_review_appraisal_recurring_theme) {
    const Game g = rookGame();
    // Level 1 speaks of blunders only: two mistakes of one kind (pins), no blunder. The critical
    // moment is no blunder, so the recurring theme is the improvement.
    {
        Appraisal a;
        a.reset(1, White);
        for (int ply = 0; ply < 21; ply += 2) {
            if (ply == 4 || ply == 12) a.add(verdictAt(g, ply, MoveClass::Mistake, 200, 50, ExType::Pin));
            else a.add(verdictAt(g, ply, MoveClass::Best, 200, 200));
        }
        const AppraisalStats st = a.stats(g);
        CHECK(st.themeRecurring);
        CHECK(st.criticalPly >= 0);
        const Script s = a.script(g, AppraisalContext{});
        checkAppraisal(s, "theme b1");
        const Beat* b = beatWithKey(s, "appraisal.improve.theme.b1");
        CHECK(b != nullptr);
        if (b) CHECK_EQ(b->line.arg("theme")->text, std::string("theme.pin"));
        CHECK(!hasPrefix(s, "appraisal.improve.critical"));
    }
    // Level 4: the critical moment (a piece left hanging) and, since it was about something else,
    // the recurring theme (pins) as well.
    for (bool sameTheme : {false, true}) {
        Appraisal a;
        a.reset(4, White);
        for (int ply = 0; ply < 21; ply += 2) {
            if (ply == 8) a.add(verdictAt(g, ply, MoveClass::Blunder, 600, -200, sameTheme ? ExType::Pin : ExType::Hanging));
            else if (ply == 4 || ply == 12) a.add(verdictAt(g, ply, MoveClass::Mistake, 200, 50, ExType::Pin));
            else a.add(verdictAt(g, ply, MoveClass::Best, 200, 200));
        }
        const AppraisalStats st = a.stats(g);
        CHECK_EQ(st.criticalPly, 8);
        CHECK(st.theme == ExType::Pin);
        const Script s = a.script(g, AppraisalContext{});
        checkAppraisal(s, "theme b4");
        CHECK(hasPrefix(s, "appraisal.improve."));
        const Beat* t = beatWithKey(s, "appraisal.improve.theme.b4");
        CHECK_EQ(t != nullptr, !sameTheme);
        if (t) CHECK_EQ(t->line.arg("theme")->text, std::string("theme.pin"));
        const Beat* improve = beatWithPrefix(s, "appraisal.improve.");
        CHECK(improve && improve->line.key != "appraisal.improve.theme.b4");
    }
}
