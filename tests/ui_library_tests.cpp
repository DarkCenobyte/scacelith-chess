// The Saved games page of the title menu (src/ui/ui_library.cpp) over a folder of PGN files of the
// test's own: the list read on its worker thread (newest first, the player's games and a file of
// several imported games, games that cannot be read), the selection by the mouse (a double click
// replays) and by the keyboard (arrows, PageUp / PageDown, Home / End, Enter), scrolling, the mode
// filter, Replay (the game it names and the way back to the page), Delete with its confirmation
// (cancelled, confirmed, a file of several games, a file changed or gone meanwhile), Open folder,
// Save as GIF greyed out without an online account, a folder listed again while the page is open,
// and the page mirrored in Arabic. The Analysis page and the Analyse button are left alone.
#include "test.h"
#include "fake_platform.h"
#include "ui_driver.h"
#include "game/game_archive.h"
#include "game/online_session.h"
#include "i18n/i18n.h"
#include "ui/ui_screens_game.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using ui::MenuAction;
using ui::gfx::Rect;
namespace archive = game::archive;

namespace {

std::string T(const char* key) { return i18n::tr(key); }

bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    f << text;
    return bool(f);
}
bool appendFile(const std::string& path, const std::string& text) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::app);
    f << text;
    return bool(f);
}
bool exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::u8path(path), ec);
}

// The folder of saved games: two of the player's games (one file each), a file of six imported
// games, and a file that is not text. Listed newest first: Alice's game, Bob's, the club's six
// (from the 7th of June 2025 back to the 2nd), then the broken file (no date).
const char* const kAlice = "2026-10-01_210409_play_Alice-vs-Stockfish.pgn";
const char* const kBob = "2026-09-15_180000_hotseat_Bob-vs-Carol.pgn";
const char* const kClub = "club.pgn";
const char* const kBroken = "broken.pgn";

void writeGames(const std::string& folder) {
    CHECK(writeFile(archive::joinPath(folder, kAlice),
                    "[Event \"Scacelith game\"]\n[Site \"Scacelith\"]\n[Date \"2026.10.01\"]\n[Time \"21:04:09\"]\n[Round \"-\"]\n"
                    "[White \"Alice\"]\n[Black \"Stockfish\"]\n[Result \"1-0\"]\n[WhiteElo \"1500\"]\n[BlackElo \"-\"]\n"
                    "[TimeControl \"300+3\"]\n[ScacelithMode \"play\"]\n[ScacelithEnd \"checkmate\"]\n\n"
                    "1. e4 e5 2. Qh5 Nc6 3. Bc4 Nf6 4. Qxf7# 1-0\n"));
    CHECK(writeFile(archive::joinPath(folder, kBob),
                    "[Event \"Two players\"]\n[Date \"2026.09.15\"]\n[Time \"18:00:00\"]\n[White \"Bob\"]\n[Black \"Carol\"]\n"
                    "[Result \"1/2-1/2\"]\n[ScacelithMode \"hotseat\"]\n[Termination \"agreement\"]\n\n1. d4 d5 2. c4 e6 1/2-1/2\n"));
    CHECK(writeFile(archive::joinPath(folder, kClub),
                    // An export of another program: an event, ratings, a slow time control and the
                    // opening named by its tags.
                    "[Event \"Club championship\"]\n[Site \"Lyon\"]\n[Date \"2025.06.07\"]\n[Round \"1\"]\n"
                    "[White \"Dupont, Jean\"]\n[Black \"Martin, Luc\"]\n[Result \"0-1\"]\n[WhiteElo \"1850\"]\n[BlackElo \"1920\"]\n"
                    "[TimeControl \"5400+30\"]\n[Opening \"Sicilian Defense\"]\n[Variation \"Najdorf\"]\n[Termination \"Normal\"]\n\n"
                    "1. e4 c5 2. Nf3 d6 3. d4 cxd4 4. Nxd4 Nf6 5. Nc3 a6 0-1\n\n"
                    // Unfinished, White unknown.
                    "[Event \"Club championship\"]\n[Date \"2025.06.06\"]\n[White \"?\"]\n[Black \"Martin, Luc\"]\n[Result \"*\"]\n\n"
                    "1. d4 d5 2. c4 *\n\n"
                    // From a set-up position, Black to move.
                    "[Event \"Endgame study\"]\n[Date \"2025.06.05\"]\n[White \"Study\"]\n[Black \"Endgame\"]\n[Result \"1/2-1/2\"]\n"
                    "[SetUp \"1\"]\n[FEN \"4k3/8/8/8/8/8/4P3/4K2R b K - 0 40\"]\n\n40... Kd7 41. O-O Kc6 1/2-1/2\n\n"
                    // Listed (the listing does not play the moves) but it cannot be loaded: an illegal move.
                    "[Event \"Club championship\"]\n[Date \"2025.06.04\"]\n[White \"Ann\"]\n[Black \"Ben\"]\n[Result \"1-0\"]\n\n"
                    "1. e4 e5 2. Ke3 Nc6 1-0\n\n"
                    // No move at all.
                    "[Event \"Club championship\"]\n[Date \"2025.06.03\"]\n[White \"Cleo\"]\n[Black \"Dan\"]\n[Result \"1-0\"]\n\n1-0\n\n"
                    // A comment never closed: listed with its error.
                    "[Event \"Club championship\"]\n[Date \"2025.06.02\"]\n[White \"Eve\"]\n[Black \"Finn\"]\n[Result \"*\"]\n\n"
                    "1. c4 { unclosed\n"));
    CHECK(writeFile(archive::joinPath(folder, kBroken), std::string("\xFF\xFE[\0E\0", 6)));
}

// The title menu with a folder of saved games, the actions it returned, and the notices drawn over
// it (switched off for a frame to tell whether one shows).
struct Menus {
    ui::NewGameSetup setup;
    ui::WatchSetup watch;
    ui::CoachSetup coach;
    ui::LibrarySetup library;
    std::vector<MenuAction> actions;
    bool notices = true;
    void attach(uitest::Driver& d) {
        d.screen = [this] {
            MenuAction a = ui::mainMenu(setup, watch, coach, library);
            if (a != MenuAction::None) actions.push_back(a);
            if (notices) ui::drawNotifications();
        };
    }
    MenuAction last() const { return actions.empty() ? MenuAction::None : actions.back(); }
};

// Stops the listing's worker before the test's folder goes (ui::shutdown does it in the game), and
// gives English back.
struct Cleanup {
    ~Cleanup() {
        ui::detail::libraryShutdown();
        i18n::setLanguage("en");
    }
};

void settle(uitest::Driver& d) { d.frames(30); }

// The rows of the list (bare items of 704 x 62 at 1920 x 1080), from the top.
std::vector<Rect> rows(const uitest::Driver& d) {
    std::vector<Rect> out = d.bare([](const ui::im::TracedItem& it) { return it.r.w == 704.0f && it.r.h == 62.0f; });
    std::sort(out.begin(), out.end(), [](const Rect& a, const Rect& b) { return a.y < b.y; });
    return out;
}

// Frames until the list shows n rows. The listing is read on a thread of its own: the frames wait a
// little real time too (5 s at most).
bool rowsShown(uitest::Driver& d, size_t n) {
    for (int i = 0; i < 2500; ++i) {
        if (rows(d).size() == n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        d.frame();
    }
    return rows(d).size() == n;
}

// Whether a notice shows: one more draw call (the overlay layer) with the notices drawn than without.
bool noticeShown(uitest::Driver& d, Menus& m) {
    m.notices = false;
    const uint64_t without = d.drawsOfFrame();
    m.notices = true;
    const uint64_t with = d.drawsOfFrame();
    return with > without;
}
// Every notice run out (6.6 s at most) in a few long frames.
void quiet(uitest::Driver& d) {
    const float dt = d.dt;
    d.dt = 1.0f;
    d.frames(8);
    d.dt = dt;
    d.frames(2);
}

// The selected row's game replayed: Replay pressed, then the page opened again as after a replay.
// False when Replay did not start one.
bool replay(uitest::Driver& d, Menus& m) {
    m.actions.clear();
    if (!d.click(T("library.replay")) || m.last() != MenuAction::StartReplay) return false;
    const bool title = ui::titlePageShown();
    ui::openSavedGames();
    d.frames(2);
    settle(d);
    return title;
}

// The mouse on an empty corner of the window (no tooltip shows).
void away(uitest::Driver& d) { d.move(4.0f, 1070.0f); }

}  // namespace

// The list: the title page's Saved games entry opens it on the newest game; Replay names the game
// (its file and its place in the file) and the page comes back as it was; a click selects, a double
// click replays; End, Home, PageDown, PageUp and the arrows move the selection (the list scrolls to
// keep it in view) and Enter replays; the wheel scrolls; a game that cannot be loaded greys Replay
// out; Esc goes back to the title page.
TEST(ui_library_list_select_and_replay) {
    REQUIRE(uitest::app());
    headless::TempDir dir("library-list");
    Cleanup cleanup;
    writeGames(dir.path);
    uitest::Driver d;
    uitest::Sounds sounds;
    Menus m;
    m.library.folder = dir.path;
    m.attach(d);
    d.frames(2);
    REQUIRE(d.click(T("menu.library")));
    CHECK(!ui::titlePageShown());
    REQUIRE(rowsShown(d, 9));
    settle(d);
    std::vector<Rect> r = rows(d);
    CHECK_EQ(r[0].x, 284.0f);   // the list on the left, the details on the right
    CHECK_EQ(r[0].y, 316.0f);
    CHECK_EQ(r[1].y - r[0].y, 68.0f);
    CHECK(d.has(T("library.filter")));
    CHECK(d.has(T("library.open_folder")));
    CHECK(!d.disabled(T("library.delete")));
    // The newest game is selected: Replay names it and the page comes back on it.
    CHECK(!d.disabled(T("library.replay")));
    REQUIRE(replay(d, m));
    CHECK_EQ(m.library.replay.path, archive::joinPath(dir.path, kAlice));
    CHECK_EQ(m.library.replay.game, 0);
    CHECK(m.library.replay.pgn.empty());
    REQUIRE(rowsShown(d, 9));   // the listing kept: no "Reading" in between
    m.library.replay = ui::ReplaySetup();
    REQUIRE(replay(d, m));
    CHECK_EQ(m.library.replay.path, archive::joinPath(dir.path, kAlice));
    // A click selects the club's first game (a tick of the toggle sound), a second click on the same
    // row soon after replays it.
    const int toggles = sounds.count(ui::Sound::Toggle);
    d.clickRect(rows(d)[2]);
    CHECK_EQ(sounds.count(ui::Sound::Toggle), toggles + 1);
    REQUIRE(replay(d, m));
    CHECK_EQ(m.library.replay.path, archive::joinPath(dir.path, kClub));
    CHECK_EQ(m.library.replay.game, 0);
    m.actions.clear();
    const Rect bob = rows(d)[1];
    d.clickAt(bob.cx(), bob.cy());
    d.clickAt(bob.cx(), bob.cy());
    CHECK_EQ(m.last(), MenuAction::StartReplay);
    CHECK_EQ(m.library.replay.path, archive::joinPath(dir.path, kBob));
    d.frame();
    CHECK(ui::titlePageShown());
    ui::openSavedGames();
    d.frames(2);
    settle(d);
    // The keys: End goes to the broken file (at the bottom, scrolled into view), which cannot be
    // replayed.
    away(d);
    const int ticks = sounds.count(ui::Sound::Tick);
    d.press(plat::KEY_END);
    settle(d);
    CHECK_EQ(sounds.count(ui::Sound::Tick), ticks + 1);
    r = rows(d);
    REQUIRE(r.size() == 9u);
    CHECK(r[0].y < 316.0f - 100.0f);   // scrolled by 104 (the last row's bottom at the list's)
    CHECK(r[8].b() <= 824.5f);
    CHECK(d.disabled(T("library.replay")));
    // Home: the newest again, at the top.
    d.press(plat::KEY_HOME);
    settle(d);
    CHECK(std::abs(rows(d)[0].y - 316.0f) < 0.5f);
    CHECK(!d.disabled(T("library.replay")));
    // PageDown: six rows on (the club's game without a move, which replays all the same); Enter
    // replays the selected game.
    d.press(plat::KEY_PAGEDOWN);
    d.frames(2);
    CHECK(!d.disabled(T("library.replay")));
    m.actions.clear();
    d.press(plat::KEY_ENTER);
    d.frame();
    CHECK_EQ(m.last(), MenuAction::StartReplay);
    CHECK_EQ(m.library.replay.path, archive::joinPath(dir.path, kClub));
    CHECK_EQ(m.library.replay.game, 4);
    ui::openSavedGames();
    d.frames(2);
    settle(d);
    // One row up with the arrow (the selection follows the keyboard focus): the illegal move, which
    // cannot be replayed; PageUp then goes back to the top.
    ui::im::setKeyboardMode(true);
    d.press(plat::KEY_UP);
    d.frames(2);
    CHECK(d.disabled(T("library.replay")));
    d.press(plat::KEY_UP);
    d.frames(2);
    CHECK(!d.disabled(T("library.replay")));   // the set-up position
    REQUIRE(replay(d, m));
    CHECK_EQ(m.library.replay.game, 2);
    d.press(plat::KEY_PAGEUP);
    d.frames(2);
    REQUIRE(replay(d, m));
    CHECK_EQ(m.library.replay.path, archive::joinPath(dir.path, kAlice));
    ui::im::setKeyboardMode(false);
    // The game with an unclosed comment: listed, but neither shown nor replayed.
    const Rect unclosed = rows(d)[7];   // half under the list's lower edge
    d.clickAt(unclosed.cx(), unclosed.y + 12.0f);
    d.frames(2);
    CHECK(d.disabled(T("library.replay")));
    // The wheel over the list scrolls it (the selection stays).
    d.press(plat::KEY_HOME);
    settle(d);
    d.move(rows(d)[3].cx(), rows(d)[3].cy());
    d.wheel(-1.0f);
    settle(d);
    CHECK(rows(d)[0].y < 316.0f - 90.0f);
    d.wheel(2.0f);
    settle(d);
    CHECK(std::abs(rows(d)[0].y - 316.0f) < 0.5f);
    // Esc: back to the title page.
    d.press(plat::KEY_ESCAPE);
    d.frame();
    CHECK(ui::titlePageShown());
}

// The filter by mode: the player's games against Stockfish, two players at one computer, the
// imported ones; an empty kind says so and greys Replay and Delete out.
TEST(ui_library_filter_by_mode) {
    REQUIRE(uitest::app());
    headless::TempDir dir("library-filter");
    Cleanup cleanup;
    writeGames(dir.path);
    uitest::Driver d;
    Menus m;
    m.library.folder = dir.path;
    m.attach(d);
    ui::debug::openMenuPage(ui::debug::MenuPage::Library);
    d.frames(2);
    REQUIRE(rowsShown(d, 9));
    settle(d);
    // Every step of the filter, and the rows it shows.
    const size_t shown[] = {9, 1, 0, 1, 0, 0, 7};
    for (int f = 1; f < 7; ++f) {
        REQUIRE(d.nudge(T("library.filter"), plat::KEY_RIGHT));
        d.frames(2);
        CHECK_EQ(rows(d).size(), shown[f]);
        if (shown[f] == 0) {
            CHECK(d.disabled(T("library.replay")));
            CHECK(d.disabled(T("library.delete")));
        }
        if (f == 3) {   // two players: Bob's game, selected by itself
            REQUIRE(replay(d, m));
            CHECK_EQ(m.library.replay.path, archive::joinPath(dir.path, kBob));
        }
    }
    // Imported: the club's first game comes first.
    REQUIRE(replay(d, m));
    CHECK_EQ(m.library.replay.path, archive::joinPath(dir.path, kClub));
    CHECK_EQ(m.library.replay.game, 0);
    // Back to every game (the filter is kept by the page from one visit to the next).
    REQUIRE(d.nudge(T("library.filter"), plat::KEY_LEFT, 6));
    d.frames(2);
    CHECK_EQ(rows(d).size(), 9u);
    d.click(T("common.back"));
    CHECK(ui::titlePageShown());
}

// Delete: asked first (Cancel and Esc keep the game), then the file goes and the next game is
// selected; a game of a file of several games is never deleted from here (a notice says why); the
// broken file cannot be deleted; a file changed or deleted since it was listed is kept or forgotten
// with a notice.
TEST(ui_library_delete) {
    REQUIRE(uitest::app());
    headless::TempDir dir("library-delete");
    Cleanup cleanup;
    writeGames(dir.path);
    uitest::Driver d;
    Menus m;
    m.library.folder = dir.path;
    m.attach(d);
    ui::debug::openMenuPage(ui::debug::MenuPage::Library);
    d.frames(2);
    // The first listing has just arrived: the next one is two seconds of frames away, so the file
    // changed below is not listed again before Delete is confirmed.
    REQUIRE(rowsShown(d, 9));
    const std::string alice = archive::joinPath(dir.path, kAlice), bob = archive::joinPath(dir.path, kBob),
                      club = archive::joinPath(dir.path, kClub);
    d.press(plat::KEY_DELETE);
    d.frame();
    CHECK_EQ(d.count(T("library.delete.ok")), 2);   // the page's Delete and the dialog's
    CHECK(appendFile(alice, "\n"));
    REQUIRE(d.clickLast(T("library.delete.ok")));
    CHECK(exists(alice));
    CHECK(noticeShown(d, m));   // the file has changed: listed again
    CHECK_EQ(d.count(T("library.delete.ok")), 1);
    quiet(d);
    REQUIRE(rowsShown(d, 9));
    settle(d);

    // Bob's game: Cancel, then Esc, keep it.
    d.clickRect(rows(d)[1]);
    REQUIRE(d.click(T("library.delete")));
    CHECK_EQ(d.count(T("library.delete.ok")), 2);
    REQUIRE(d.clickLast(T("common.cancel")));
    d.frame();
    CHECK_EQ(d.count(T("library.delete.ok")), 1);
    CHECK(exists(bob));
    away(d);
    d.press(plat::KEY_DELETE);
    d.frame();
    CHECK_EQ(d.count(T("library.delete.ok")), 2);
    d.press(plat::KEY_ESCAPE);
    d.frames(2);
    CHECK_EQ(d.count(T("library.delete.ok")), 1);
    CHECK(!ui::titlePageShown());   // Esc closed the question, not the page
    CHECK(exists(bob));
    CHECK(!noticeShown(d, m));
    // Confirmed: the file goes, its row at once, and the next game is selected.
    d.press(plat::KEY_DELETE);
    d.frame();
    REQUIRE(d.clickLast(T("library.delete.ok")));
    CHECK(!exists(bob));
    CHECK_EQ(rows(d).size(), 8u);
    CHECK(noticeShown(d, m));
    CHECK(exists(alice));
    REQUIRE(replay(d, m));
    CHECK_EQ(m.library.replay.path, club);
    CHECK_EQ(m.library.replay.game, 0);
    REQUIRE(rowsShown(d, 8));
    quiet(d);

    // The club's first game (selected): its file holds six, so a notice instead of the question.
    d.press(plat::KEY_DELETE);
    d.frame();
    CHECK_EQ(d.count(T("library.delete.ok")), 1);
    CHECK(noticeShown(d, m));
    CHECK(exists(club));
    quiet(d);
    // The broken file: Delete greyed out, the Delete key does nothing.
    d.press(plat::KEY_END);
    d.frames(2);
    CHECK(d.disabled(T("library.delete")));
    d.press(plat::KEY_DELETE);
    d.frame();
    CHECK_EQ(d.count(T("library.delete.ok")), 1);
    CHECK(!noticeShown(d, m));

    // Alice's game, deleted by hand while the question is up: the listing that follows forgets it,
    // and the confirmation only says the list changed.
    d.press(plat::KEY_HOME);
    d.frames(2);
    d.press(plat::KEY_DELETE);
    d.frame();
    CHECK_EQ(d.count(T("library.delete.ok")), 2);
    std::error_code ec;
    std::filesystem::remove(std::filesystem::u8path(alice), ec);
    REQUIRE(rowsShown(d, 7));
    REQUIRE(d.clickLast(T("library.delete.ok")));
    CHECK_EQ(d.count(T("library.delete.ok")), 1);
    CHECK(noticeShown(d, m));
    CHECK(exists(club));
    CHECK_EQ(rows(d).size(), 7u);
}

// A folder that does not exist yet: nothing to select (Replay and Delete greyed out), Open folder
// makes it and shows it in the file manager, and a game put there shows up while the page is open.
// Save as GIF needs an account of an online server: greyed out, its tooltip says why.
TEST(ui_library_empty_folder_and_open_folder) {
    REQUIRE(uitest::app());
    headless::TempDir dir("library-empty");
    Cleanup cleanup;
    const std::string folder = dir.path + "games/";
    uitest::Driver d;
    Menus m;
    m.library.folder = folder;
    m.attach(d);
    ui::debug::openMenuPage(ui::debug::MenuPage::Library);
    d.frames(2);
    settle(d);
    CHECK(rows(d).empty());
    CHECK(d.disabled(T("library.replay")));
    CHECK(d.disabled(T("library.delete")));
    const std::size_t opened = fakeplat::record().opened.size();
    REQUIRE(d.click(T("library.open_folder")));
    REQUIRE(fakeplat::record().opened.size() == opened + 1);
    CHECK_EQ(fakeplat::record().opened.back(), folder);
    CHECK(std::filesystem::is_directory(std::filesystem::u8path(folder)));
    // A game saved meanwhile: listed again within a few seconds, selected, replayed.
    CHECK(writeFile(archive::joinPath(folder, kBob),
                    "[Event \"Two players\"]\n[Date \"2026.09.15\"]\n[White \"Bob\"]\n[Black \"Carol\"]\n[Result \"*\"]\n"
                    "[ScacelithMode \"hotseat\"]\n\n1. e4 *\n"));
    REQUIRE(rowsShown(d, 1));
    d.frames(2);
    CHECK(!d.disabled(T("library.replay")));
    CHECK(!d.disabled(T("library.delete")));
    // Save as GIF: drawn greyed out, with a tooltip, when no account of an online server is known.
    const game::OnlineSession& se = game::onlineSession();
    if (!se.signedIn() && !se.hasSavedSession()) {
        CHECK(d.disabled(T("gif.save")));
        away(d);
        d.wait(1.0f);
        const uint64_t plain = d.drawsOfFrame();
        REQUIRE(d.hover(T("gif.save")));
        d.wait(1.5f);
        CHECK(d.drawsOfFrame() > plain);   // the tooltip's layer
        away(d);
    }
    REQUIRE(replay(d, m));
    CHECK_EQ(m.library.replay.path, archive::joinPath(folder, kBob));
    d.click(T("common.back"));
    CHECK(ui::titlePageShown());
}

// In Arabic the page is mirrored: the list on the right; a click still selects and Replay still
// names the game. The openings of the rows are worked out again in the new language.
TEST(ui_library_mirrored_in_arabic) {
    REQUIRE(uitest::app());
    headless::TempDir dir("library-rtl");
    Cleanup cleanup;
    writeGames(dir.path);
    uitest::Driver d;
    Menus m;
    m.library.folder = dir.path;
    m.attach(d);
    ui::debug::openMenuPage(ui::debug::MenuPage::Library);
    d.frames(2);
    REQUIRE(rowsShown(d, 9));
    REQUIRE(i18n::setLanguage("ar"));
    d.frames(2);
    settle(d);
    std::vector<Rect> r = rows(d);
    REQUIRE(r.size() == 9u);
    CHECK_EQ(r[0].x, 1920.0f - 284.0f - 704.0f);
    d.clickRect(r[2]);
    REQUIRE(replay(d, m));
    CHECK_EQ(m.library.replay.path, archive::joinPath(dir.path, kClub));
    CHECK_EQ(m.library.replay.game, 0);
    d.press(plat::KEY_ESCAPE);
    d.frame();
    CHECK(ui::titlePageShown());
}
