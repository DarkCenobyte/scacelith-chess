// Endings of a game in the game scene (src/game/game_scene.cpp) reached from a position set up on
// the command line (--moves: the moves made at once, already on the scoresheets): a draw claimed
// by threefold repetition from the Esc menu, a stalemate, a promotion to a knight chosen on the
// promotion picker. Each is a hot-seat game without a clock on the headless game (headless.h),
// played by hand through the player's eyes, so no engine is needed; the saved game says how it
// ended.
#include "game_scene_testkit.h"

using namespace gstest;

namespace {

// A hot-seat game between Ann and Ben without a clock, the view cut over at once, from the
// position the moves give.
std::vector<std::string> setUpArgs(const std::string& moves) {
    return {"--no-intro", "--start",    "--hotseat", "--white-name", "Ann", "--black-name", "Ben",
            "--tc",       "0",          "--handover", "0",           "--moves", moves};
}

// Forty moves that fill the first page of the scoresheets: eight times a pawn step each, then the
// knights out and back twice. The last pawn steps' position stands for the third time at the end.
std::vector<std::string> fortyMoves() {
    static const char* const white[8] = {"a2a3", "b2b3", "c2c3", "d2d3", "e2e3", "h2h3", "a3a4", "b3b4"};
    static const char* const black[8] = {"a7a6", "b7b6", "c7c6", "d7d6", "e7e6", "h7h6", "a6a5", "b6b5"};
    std::vector<std::string> out;
    for (int i = 0; i < 8; ++i) {
        out.push_back(white[i]);
        out.push_back(black[i]);
        for (int k = 0; k < 2; ++k)
            for (const char* u : {"g1f3", "g8f6", "f3g1", "f6g8"}) out.push_back(u);
    }
    return out;
}

std::string joined(const std::vector<std::string>& moves) {
    std::string s;
    for (const std::string& m : moves) s += (s.empty() ? "" : ",") + m;
    return s;
}

bool onlyNewGame(const std::string& folder, const std::set<std::string>& before, chess::pgn::Record& out) {
    const std::vector<chess::pgn::Record> games = newGames(folder, before);
    if (games.size() != 1) return false;
    out = games[0];
    return true;
}

}  // namespace

// Forty moves set up (the first page of both scoresheets full), then White's 41st move by hand: each
// player turns the page of their sheet to write it. That position stands for the third time:
// Black claims the draw from the Esc menu before moving. Drawn, saved with the moves set up
// (without clocks).
TEST(game_scene_endings_page_turned_then_threefold_claimed) {
    LogTail log;
    const std::vector<std::string> moves = fortyMoves();
    headless::App* app = headless::shared(setUpArgs(joined(moves)));
    REQUIRE(app);
    const std::string folder = pgnFolder(*app);
    const std::set<std::string> before = pgnFiles(folder);
    REQUIRE(waitLog(*app, log, "New game (hot-seat): Ann (White) vs Ben (Black), Unlimited, friendly"));
    CHECK(!log.has("--moves: "));   // every move set up
    app->frames(200);
    chess::Game g;
    for (const std::string& u : moves) REQUIRE(g.play(g.position().parseUCI(u)));
    REQUIRE(g.status() == chess::GameStatus::Ongoing);
    const size_t mark = log.mark();
    playMove(*app, g.position(), "g1f3");
    REQUIRE(waitLog(*app, log, "] move 81: Nf3 (White", mark));
    REQUIRE(waitLog(*app, log, "scoresheet 0: page turned", mark));
    REQUIRE(waitLog(*app, log, "hot-seat: the view is in Ben's eyes", mark));
    REQUIRE(waitLog(*app, log, "scoresheet 1: page turned", mark));
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Claim draw", 30));
    REQUIRE(app->clickLabel("Claim draw"));
    REQUIRE(waitLog(*app, log, "Game over: \xC2\xBD-\xC2\xBD (Threefold repetition (claimed))", mark));
    chess::pgn::Record r;
    REQUIRE(onlyNewGame(folder, before, r));
    CHECK_EQ(r.result, std::string("1/2-1/2"));
    CHECK_EQ(r.tag("ScacelithEnd"), std::string("threefold_claim"));
    REQUIRE(r.plies.size() == 81u);
    CHECK_EQ(r.plies[80].san, std::string("Nf3"));
    CHECK(r.plies[79].clockMs < 0);
    REQUIRE(app->waitForLabel("Rematch", 1200));
    REQUIRE(toTitle(*app));
}

// Sam Loyd's stalemate in ten moves: the first nine set up, the last one played by hand (the queen
// from c8 to e6). Black has no legal move and is not in check: a draw.
TEST(game_scene_endings_stalemate_by_hand) {
    LogTail log;
    headless::App* app = headless::shared(setUpArgs(
        "e2e3,a7a5,d1h5,a8a6,h5a5,h7h5,h2h4,a6h6,a5c7,f7f6,c7d7,e8f7,d7b7,d8d3,b7b8,d3h7,b8c8,f7g6"));
    REQUIRE(app);
    const std::string folder = pgnFolder(*app);
    const std::set<std::string> before = pgnFiles(folder);
    REQUIRE(waitLog(*app, log, "New game (hot-seat): Ann (White) vs Ben (Black)"));
    app->frames(200);
    chess::Game g;
    for (const char* u : {"e2e3", "a7a5", "d1h5", "a8a6", "h5a5", "h7h5", "h2h4", "a6h6", "a5c7", "f7f6", "c7d7", "e8f7",
                          "d7b7", "d8d3", "b7b8", "d3h7", "b8c8", "f7g6"})
        REQUIRE(g.play(g.position().parseUCI(u)));
    playMove(*app, g.position(), "c8e6");
    REQUIRE(waitLog(*app, log, "Game over: \xC2\xBD-\xC2\xBD (Stalemate)"));
    CHECK(log.has("] move 19: Qe6 (White"));
    chess::pgn::Record r;
    REQUIRE(onlyNewGame(folder, before, r));
    CHECK_EQ(r.result, std::string("1/2-1/2"));
    CHECK_EQ(r.tag("ScacelithEnd"), std::string("stalemate"));
    CHECK_EQ(r.plies.size(), size_t(19));
    REQUIRE(app->waitForLabel("Rematch", 1200));
    REQUIRE(toTitle(*app));
}

// A pawn takes on the last rank (b7xa8): the promotion picker asks for the new piece (Esc does not
// open the menu meanwhile) and N chooses a knight, swapped in by the hand. The game is then left
// unfinished from Black's Esc menu: saved with the promotion.
TEST(game_scene_endings_promotion_to_a_knight) {
    LogTail log;
    headless::App* app = headless::shared(setUpArgs("e2e4,d7d5,e4d5,c7c6,d5c6,d8b6,c6b7,b6b2"));
    REQUIRE(app);
    const std::string folder = pgnFolder(*app);
    const std::set<std::string> before = pgnFiles(folder);
    REQUIRE(waitLog(*app, log, "New game (hot-seat): Ann (White) vs Ben (Black)"));
    app->frames(200);
    chess::Game g;
    for (const char* u : {"e2e4", "d7d5", "e4d5", "c7c6", "d5c6", "d8b6", "c6b7", "b6b2"})
        REQUIRE(g.play(g.position().parseUCI(u)));
    const size_t mark = log.mark();
    playMove(*app, g.position(), "b7a8");
    app->frames(200);   // the rook taken off, the pawn put down: the picker
    app->key(plat::KEY_ESCAPE);
    CHECK(!app->hasLabel("Main menu"));
    // N on the picker (pressed again until the hand is done, as a player would).
    for (int i = 0; i < 30 && !log.has("] move 9: ", mark); ++i) {
        app->key('N');
        app->frames(20);
    }
    REQUIRE(waitLog(*app, log, "] move 9: bxa8=N (White", mark));
    REQUIRE(waitLog(*app, log, "hot-seat: the view is in Ben's eyes", mark));
    app->frames(30);
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel("Main menu", 30));
    REQUIRE(app->clickLabel("Main menu"));
    REQUIRE(app->waitForLabel("Leave", 30));
    REQUIRE(clickLast(*app, "Leave"));
    REQUIRE(app->waitForLabel("New Game", 600));
    chess::pgn::Record r;
    REQUIRE(onlyNewGame(folder, before, r));
    CHECK_EQ(r.result, std::string("*"));
    REQUIRE(r.plies.size() == 9u);
    CHECK_EQ(r.plies[8].san, std::string("bxa8=N"));
    headless::stopShared();   // the next file's tests start their own App
}
