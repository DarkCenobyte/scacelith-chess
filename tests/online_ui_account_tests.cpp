// The account API's pages of "Play Online" (src/ui/ui_screens_account.cpp), driven through the
// headless game against the in-process fake server (online_ui_app.h): the game history (its pages,
// its filters by kind and result, an account without games), a game of it (saved to the saved
// games, found saved when opened again, its GIF made by the server and written to the GIF folder,
// or refused by a server that makes none, the opponent reported), the signed-in devices (one signed
// out, Sign out everywhere asked and cancelled), the change of e-mail (a link sent, or the address
// changed at once on a server without confirmation; refused for a wrong password, the same address,
// a wrong two-factor code), the data export written to the account folder, and the account deleted.
// The fake server's history is made from the user name (online_mock.cpp): the tests check what
// holds for any such history (ten rows a page, the filters keeping only the games they name). The
// game page's Analyse is not pressed: the Analysis mode is tested elsewhere.
#include "online_ui_app.h"
#include "game/online_account.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace onlinetest;

namespace {

game::OnlineSession& ses() { return game::onlineSession(); }
const game::HistoryPager& pager() { return ses().accountData().history; }

// From the Play page (signed in): the account page, its account fetched.
bool toAccount(headless::App& app) {
    if (!app.clickLabel("Account") || !onPage(app, "Change password")) return false;
    return answered(app, Kind::AccountResult, 120);
}

// The history's page of the current filter has come (and the page drawn it).
bool historyLoaded(headless::App& app) {
    if (!app.runUntil([&] { return pager().loaded() && !pager().waiting(); }, 300)) return false;
    app.frames(2);
    return true;
}

// The rows of the history shown (games of the page, ten at most).
std::vector<ui::im::TracedItem> rows(headless::App& app) { return bare(app, 48.0f); }

// Moves the selector labelled 'label' by 'steps' (Right, or Left when negative), from the keyboard
// once it has the focus (a click on its label).
void select(headless::App& app, const std::string& label, int steps) {
    const ui::im::TracedItem* it = app.find(label);
    if (!it) return;
    const ui::gfx::Rect r = it->r;
    const float x = ui::im::rtl() ? r.r() - 30.0f : r.x + 30.0f;
    app.click(x * ui::gfx::scale(), r.cy() * ui::gfx::scale());
    for (int i = 0; i < std::abs(steps); ++i) app.key(steps > 0 ? plat::KEY_RIGHT : plat::KEY_LEFT);
}

// The files with 'ext' in the App's folder 'sub' (as written by the game).
std::vector<std::filesystem::path> filesIn(headless::App& app, const std::string& sub, const std::string& ext) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto& f : std::filesystem::directory_iterator(std::filesystem::u8path(app.folder().file(sub)), ec))
        if (f.path().extension() == ext) out.push_back(f.path());
    return out;
}

// On the sign-in page: signs in as 'name', an account with two-factor on (its code page, then a
// code of the app). True once the Play page shows.
bool signInWithCode(headless::App& app, const std::string& name) {
    if (!fill(app, "User name or e-mail", name) || !fill(app, "Password", "correct horse battery")) return false;
    if (!app.clickLabel("Sign in") || !onPage(app, "Verify", 120)) return false;
    if (!fill(app, "Code", "123456", 16) || !app.clickLabel("Verify")) return false;
    return app.runUntil([&] { return ses().signedIn() && app.hasLabel("Find opponent"); }, 300);
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

// The history: ten games a page, Next and Previous; the filters (rated, casual; won, lost, drawn)
// keep only the games they name. A game opened from it: saved to the saved games (and found saved
// when opened again), its moves scrolled, its GIF made and written (Open Folder shows its folder),
// its opponent reported.
TEST(online_ui_history_and_a_game_of_it) {
    headless::App* app = onlineApp();
    REQUIRE(app);
    game::OnlineSession& s = ses();
    REQUIRE(signIn(*app, "Historian"));
    REQUIRE(toAccount(*app));
    REQUIRE(app->clickLabel("Game history"));
    REQUIRE(onPage(*app, "Next"));
    REQUIRE(historyLoaded(*app));
    REQUIRE(pager().pageCount() >= 3);
    const int total = pager().page().total;
    CHECK_EQ(pager().pageIndex(), 0);
    CHECK_EQ(rows(*app).size(), size_t(10));
    CHECK(!enabled(*app, "Previous"));
    REQUIRE(enabled(*app, "Next"));
    const uint64_t newest = pager().page().games[0].id;

    REQUIRE(app->clickLabel("Next"));
    REQUIRE(historyLoaded(*app));
    CHECK_EQ(pager().pageIndex(), 1);
    REQUIRE(!pager().page().games.empty());
    CHECK(pager().page().games[0].id < newest);   // older games
    REQUIRE(enabled(*app, "Previous"));
    REQUIRE(app->clickLabel("Previous"));
    REQUIRE(historyLoaded(*app));
    CHECK_EQ(pager().pageIndex(), 0);
    CHECK_EQ(pager().page().games[0].id, newest);

    // Rated games only, then casual ones only, then all again.
    select(*app, "Games", 1);
    REQUIRE(historyLoaded(*app));
    const int rated = pager().page().total;
    for (const net::GameSummary& g : pager().page().games) CHECK(g.rated);
    select(*app, "Games", 1);
    REQUIRE(historyLoaded(*app));
    const int casual = pager().page().total;
    for (const net::GameSummary& g : pager().page().games) CHECK(!g.rated);
    CHECK_EQ(rated + casual, total);
    select(*app, "Games", -2);
    REQUIRE(historyLoaded(*app));
    CHECK_EQ(pager().page().total, total);
    // Won, lost, drawn: each game of those pages ended that way for the player.
    const game::Outcome outcomes[] = {game::Outcome::Win, game::Outcome::Loss, game::Outcome::Draw};
    int decided = 0;
    for (game::Outcome o : outcomes) {
        select(*app, "Result", 1);
        REQUIRE(historyLoaded(*app));
        decided += pager().page().total;
        for (const net::GameSummary& g : pager().page().games) CHECK(game::outcomeOf(g) == o);
        CHECK_EQ(rows(*app).size(), std::min(pager().page().games.size(), size_t(10)));
    }
    CHECK(decided <= total);                      // aborted games are in none of them
    select(*app, "Result", -3);
    REQUIRE(historyLoaded(*app));
    CHECK_EQ(pager().page().total, total);

    // The newest game.
    std::vector<ui::im::TracedItem> list = rows(*app);
    REQUIRE(!list.empty());
    app->clickItem(list[0]);
    REQUIRE(onPage(*app, "Save as GIF"));
    const game::AccountData& data = s.accountData();
    REQUIRE(app->runUntil([&] { return data.gameLoaded && data.game.id == newest; }, 300));
    app->frames(2);
    REQUIRE(app->hasLabel("Save game"));
    CHECK(app->hasLabel("Replay"));
    CHECK(app->hasLabel("Back"));
    const bool hasMoves = !data.game.moves.empty();
    const size_t pgnsBefore = filesIn(*app, "pgn", ".pgn").size();
    if (hasMoves) {
        REQUIRE(app->runUntil([&] { return enabled(*app, "Save game"); }, 600));   // looked for in the saved games
        REQUIRE(app->clickLabel("Save game"));
        REQUIRE(app->runUntil([&] { return app->hasLabel("Saved"); }, 3000));
        CHECK(!enabled(*app, "Saved"));
        CHECK_EQ(filesIn(*app, "pgn", ".pgn").size(), pgnsBefore + 1);
        app->key(plat::KEY_PAGEDOWN);             // the moves, scrolled
        app->key(plat::KEY_PAGEUP);
    }
    const size_t pgnsSaved = filesIn(*app, "pgn", ".pgn").size();

    // Its GIF, from the player's side: made by the server, written to the GIF folder.
    fakeplat::record().opened.clear();
    const size_t gifsBefore = filesIn(*app, "gif", ".gif").size();
    REQUIRE(enabled(*app, "Save as GIF"));
    REQUIRE(app->clickLabel("Save as GIF"));
    CHECK(s.gif().busy());
    REQUIRE(app->runUntil([&] { return s.gif().stage() == game::GifSaver::Stage::Saved; }, 3000));
    REQUIRE(app->waitForLabel("Open Folder", 60));
    CHECK_EQ(filesIn(*app, "gif", ".gif").size(), gifsBefore + 1);
    CHECK(s.gif().path().find(".gif") != std::string::npos);
    REQUIRE(app->clickLabel("Open Folder"));
    REQUIRE(!fakeplat::record().opened.empty());
    CHECK(fakeplat::record().opened.back().find("gif") != std::string::npos);

    // Its opponent reported, while the server takes reports of it.
    if (app->hasLabel("Report opponent")) {
        REQUIRE(app->clickLabel("Report opponent"));
        REQUIRE(onPage(*app, "Send report", 30));
        REQUIRE(fill(*app, "Comment", "Insulted me in the chat"));
        REQUIRE(app->clickLabel("Send report"));
        REQUIRE(answered(*app, Kind::ReportResult, 120));
        CHECK(!app->hasLabel("Report opponent"));
    }

    // Back to the history, and the same game again: found in the saved games.
    REQUIRE(app->clickLabel("Back"));
    REQUIRE(onPage(*app, "Next"));
    REQUIRE(historyLoaded(*app));
    list = rows(*app);
    REQUIRE(!list.empty());
    app->clickItem(list[0]);
    REQUIRE(onPage(*app, "Save as GIF"));
    REQUIRE(app->runUntil([&] { return data.gameLoaded && data.game.id == newest; }, 300));
    if (hasMoves) CHECK(app->runUntil([&] { return app->hasLabel("Saved"); }, 3000));
    CHECK_EQ(filesIn(*app, "pgn", ".pgn").size(), pgnsSaved);   // not saved twice
    REQUIRE(app->clickLabel("Back"));
    REQUIRE(onPage(*app, "Next"));
    REQUIRE(app->clickLabel("Back"));
    REQUIRE(onPage(*app, "Change password"));
    CHECK(toTitle(*app));
}

// The signed-in devices (another one signed out; Sign out everywhere asked, then cancelled), the
// change of e-mail (refused for a wrong password and for the same address, then a link sent: the
// account shows it waiting), the data export (written as JSON to the account folder, shown with
// Open Folder), and the account deleted (the user name typed to confirm; a wrong password
// refused): signed out, on the sign-in page.
TEST(online_ui_devices_email_export_and_deletion) {
    headless::App* app = onlineApp();
    REQUIRE(app);
    game::OnlineSession& s = ses();
    const game::AccountData& data = s.accountData();
    REQUIRE(signIn(*app, "Leaver"));
    REQUIRE(toAccount(*app));

    // Devices: this one, and three others to sign out.
    REQUIRE(app->clickLabel("Signed-in devices"));
    REQUIRE(onPage(*app, "Sign out everywhere"));
    REQUIRE(app->runUntil([&] { return data.sessionsLoaded; }, 300));
    app->frames(2);
    REQUIRE(data.sessions.size() == 4);
    REQUIRE(app->hasLabel("Sign out"));
    REQUIRE(app->clickLabel("Sign out"));
    REQUIRE(answered(*app, Kind::SessionRevoked, 120));
    CHECK_EQ(data.sessions.size(), size_t(3));
    CHECK(s.signedIn());
    REQUIRE(app->clickLabel("Sign out everywhere"));
    REQUIRE(app->waitForLabel("Cancel", 30));
    app->frames(20);
    REQUIRE(app->clickLabel("Cancel"));
    app->frames(20);
    CHECK(!app->hasLabel("Cancel"));
    CHECK(s.signedIn());
    REQUIRE(app->clickLabel("Back"));
    REQUIRE(onPage(*app, "Change password"));

    // E-mail: a wrong password, the same address, then a new one (a link goes to it).
    REQUIRE(app->clickLabel("Change e-mail"));
    REQUIRE(onPage(*app, "New e-mail"));
    CHECK(!enabled(*app, "Change e-mail"));
    CHECK(!app->hasLabel("Code"));                // two-factor off: the password only
    REQUIRE(fill(*app, "New e-mail", "leaver.new@example.com"));
    REQUIRE(fill(*app, "Password", "wrong"));
    REQUIRE(enabled(*app, "Change e-mail"));
    REQUIRE(app->clickLabel("Change e-mail"));
    REQUIRE(answered(*app, Kind::EmailChangeResult, 120));
    CHECK(app->hasLabel("New e-mail"));           // refused: the form again
    REQUIRE(fill(*app, "New e-mail", "leaver@example.com"));
    REQUIRE(fill(*app, "Password", "correct horse battery"));
    REQUIRE(app->clickLabel("Change e-mail"));
    REQUIRE(answered(*app, Kind::EmailChangeResult, 120));
    CHECK(app->hasLabel("New e-mail"));           // the address in use
    REQUIRE(fill(*app, "New e-mail", "leaver.new@example.com"));
    REQUIRE(fill(*app, "Password", "correct horse battery"));
    REQUIRE(app->clickLabel("Change e-mail"));
    REQUIRE(answered(*app, Kind::EmailChangeResult, 120));
    REQUIRE(onPage(*app, "Done"));                // the link went to the new address
    CHECK(!app->hasLabel("New e-mail"));
    REQUIRE(app->clickLabel("Done"));
    REQUIRE(onPage(*app, "Change password"));
    REQUIRE(answered(*app, Kind::AccountResult, 120));
    CHECK_EQ(s.account().pendingEmail, std::string("leaver.new@example.com"));
    CHECK_EQ(s.account().email, std::string("leaver@example.com"));

    // The data export.
    fakeplat::record().opened.clear();
    const size_t exportsBefore = filesIn(*app, "account", ".json").size();
    REQUIRE(app->clickLabel("Download my data"));
    REQUIRE(onPage(*app, "Download"));
    CHECK(!enabled(*app, "Download"));
    REQUIRE(fill(*app, "Password", "correct horse battery"));
    REQUIRE(enabled(*app, "Download"));
    REQUIRE(app->clickLabel("Download"));
    REQUIRE(answered(*app, Kind::AccountExportResult, 120));
    REQUIRE(app->runUntil([&] { return app->hasLabel("Open Folder"); }, 3000));
    CHECK(!app->hasLabel("Download"));
    const std::vector<std::filesystem::path> exported = filesIn(*app, "account", ".json");
    REQUIRE(exported.size() == exportsBefore + 1);
    std::string json;   // the new file: the one that names this account
    for (const auto& f : exported)
        if (f.filename().u8string().find("Leaver") != std::string::npos) json = readFile(f);
    CHECK(json.find("\"Leaver\"") != std::string::npos);
    CHECK(json.find("\"sessions\"") != std::string::npos);
    REQUIRE(app->clickLabel("Open Folder"));
    REQUIRE(!fakeplat::record().opened.empty());
    CHECK(fakeplat::record().opened.back().find("account") != std::string::npos);
    REQUIRE(app->clickLabel("Back"));
    REQUIRE(onPage(*app, "Change password"));

    // The deletion: the user name as written, the password.
    REQUIRE(app->clickLabel("Delete account"));
    REQUIRE(onPage(*app, "Delete my account"));
    REQUIRE(fill(*app, "Password", "wrong"));
    REQUIRE(fill(*app, "User name", "leaver"));
    CHECK(!enabled(*app, "Delete my account"));   // not the name as written
    REQUIRE(fill(*app, "User name", "Leaver"));
    REQUIRE(enabled(*app, "Delete my account"));
    REQUIRE(app->clickLabel("Delete my account"));
    REQUIRE(answered(*app, Kind::AccountDeleted, 120));
    CHECK(s.signedIn());
    CHECK(app->hasLabel("Delete my account"));    // refused: the page again
    REQUIRE(fill(*app, "Password", "correct horse battery"));
    REQUIRE(app->clickLabel("Delete my account"));
    REQUIRE(answered(*app, Kind::AccountDeleted, 120));
    REQUIRE(onPage(*app, "User name or e-mail"));
    CHECK(!s.signedIn());
    CHECK(toTitle(*app));
}

// A community server without e-mail confirmation nor GIFs, accounts with two-factor on: the
// account pages ask a code with the password (a wrong one refused), the e-mail changes at once,
// the export takes a recovery code, a GIF is refused (the server makes none), and an account
// without games has an empty history.
TEST(online_ui_account_pages_with_two_factor_on_a_community_server) {
    ReleaseAtEnd release;
    headless::App* app = onlineApp();
    REQUIRE(app);
    game::OnlineSession& s = ses();
    game::Settings& gs = game::settings();
    gs.onlineCustomServer = true;
    gs.onlineHost = "noverify-nogif.example";
    s.applyServer();
    REQUIRE(toSignInPage(*app));
    REQUIRE(signInWithCode(*app, "mfa_gifless"));
    REQUIRE(toAccount(*app));
    CHECK(s.account().mfaEnabled);

    // E-mail: the code is asked too; a wrong one refused, then changed at once.
    REQUIRE(app->clickLabel("Change e-mail"));
    REQUIRE(onPage(*app, "New e-mail"));
    REQUIRE(app->hasLabel("Code"));
    REQUIRE(fill(*app, "New e-mail", "gifless@club.example"));
    REQUIRE(fill(*app, "Password", "correct horse battery"));
    CHECK(!enabled(*app, "Change e-mail"));       // no code yet
    REQUIRE(fill(*app, "Code", "000000", 16));
    REQUIRE(enabled(*app, "Change e-mail"));
    REQUIRE(app->clickLabel("Change e-mail"));
    REQUIRE(answered(*app, Kind::EmailChangeResult, 120));
    CHECK(app->hasLabel("New e-mail"));
    REQUIRE(fill(*app, "Password", "correct horse battery"));
    REQUIRE(fill(*app, "Code", "123456", 16));
    REQUIRE(app->clickLabel("Change e-mail"));
    REQUIRE(answered(*app, Kind::EmailChangeResult, 120));
    REQUIRE(onPage(*app, "Change password"));     // changed: the account page
    REQUIRE(answered(*app, Kind::AccountResult, 120));
    CHECK_EQ(s.account().email, std::string("gifless@club.example"));
    CHECK(s.account().pendingEmail.empty());

    // The export with a recovery code.
    const size_t exportsBefore = filesIn(*app, "account", ".json").size();
    REQUIRE(app->clickLabel("Download my data"));
    REQUIRE(onPage(*app, "Download"));
    REQUIRE(fill(*app, "Password", "correct horse battery"));
    CHECK(!enabled(*app, "Download"));
    REQUIRE(fill(*app, "Code", "ABCDE-FGHIJ", 16));
    REQUIRE(enabled(*app, "Download"));
    REQUIRE(app->clickLabel("Download"));
    REQUIRE(answered(*app, Kind::AccountExportResult, 120));
    REQUIRE(app->runUntil([&] { return app->hasLabel("Open Folder"); }, 3000));
    CHECK_EQ(filesIn(*app, "account", ".json").size(), exportsBefore + 1);
    REQUIRE(app->clickLabel("Back"));
    REQUIRE(onPage(*app, "Change password"));

    // A GIF: this server makes none.
    REQUIRE(app->clickLabel("Game history"));
    REQUIRE(onPage(*app, "Next"));
    REQUIRE(historyLoaded(*app));
    std::vector<ui::im::TracedItem> list = rows(*app);
    REQUIRE(!list.empty());
    app->clickItem(list[0]);
    REQUIRE(onPage(*app, "Save as GIF"));
    REQUIRE(app->runUntil([&] { return s.accountData().gameLoaded; }, 300));
    app->frames(2);
    const size_t gifsBefore = filesIn(*app, "gif", ".gif").size();
    REQUIRE(app->clickLabel("Save as GIF"));
    REQUIRE(app->runUntil([&] { return s.gif().stage() == game::GifSaver::Stage::Failed; }, 600));
    CHECK_EQ(s.gif().error(), std::string("gif_disabled"));
    CHECK(!app->hasLabel("Open Folder"));
    CHECK_EQ(filesIn(*app, "gif", ".gif").size(), gifsBefore);
    REQUIRE(app->clickLabel("Back"));
    REQUIRE(onPage(*app, "Next"));
    REQUIRE(app->clickLabel("Back"));
    REQUIRE(onPage(*app, "Change password"));

    // Another account, without games: its history is empty. (The fake server keeps two-factor on
    // for the accounts signed in after one that turned it on: its code again.)
    REQUIRE(app->clickLabel("Sign out"));
    REQUIRE(onPage(*app, "User name or e-mail"));
    REQUIRE(signInWithCode(*app, "newbie_kid"));
    REQUIRE(toAccount(*app));
    REQUIRE(app->clickLabel("Game history"));
    REQUIRE(onPage(*app, "Next"));
    REQUIRE(historyLoaded(*app));
    CHECK(pager().page().games.empty());
    CHECK(rows(*app).empty());
    CHECK(!enabled(*app, "Next"));
    CHECK(!enabled(*app, "Previous"));
    REQUIRE(app->clickLabel("Back"));
    REQUIRE(onPage(*app, "Change password"));
    CHECK(toTitle(*app));
}
