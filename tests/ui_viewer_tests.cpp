// The "ui" viewer scene (src/ui/ui_viewer.cpp): every screen it opens from its command line
// (--ui-screen and the options that go with it: --ui-tab, --ui-name, --ui-hand, --ui-black,
// --ui-draw, --ui-kb, --ui-mouse, --ui-library), the keys 1..9 / 0 that switch screens, and the
// scripted input of --ui-keys (keys, letters, typed text, the wheel, clicks), each checked by
// what the screen shows: its widgets, or the draw calls of the screens without one. The coach,
// analysis, online and voice download screens are left alone.
//
// Each screen is a viewer scene of its own, made and started as main.cpp does it (createScene,
// init with the command line) beside the shared App, whose platform, null OpenGL and UI it uses (the
// font atlas is built once): starting an App per screen would take seconds under gcov. It is not
// shut down (its shutdown() would shut the shared App's UI down); what it changes is given back
// instead: the settings, the input and mouse overrides, the keyboard mode, the voice hooks.
#include "test.h"
#include "fake_platform.h"
#include "null_gl.h"
#include "ui_driver.h"
#include "app/scene.h"
#include "game/game_archive.h"
#include "game/settings.h"
#include "i18n/i18n.h"
#include "ui/ui_screens_game.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using ui::gfx::Rect;

namespace {

std::string T(const char* key) { return i18n::tr(key); }

class Viewer {
public:
    // The viewer started with these options after "--scene ui", on a UI as a start leaves it (no
    // page open, every widget appearing).
    explicit Viewer(const std::vector<std::string>& options) : saved_(game::settings()) {
        headless::App* app = uitest::app();
        if (!app) return;
        ui::beginFrame(plat::width(), plat::height(), 1.0f / 60.0f);
        ui::endFrame();
        ui::detail::screensReset();
        plat::Input& in = fakeplat::next();
        in.mouseX = in.mouseY = 2.0f;
        ctx_.renderer = &app->renderer();
        ctx_.screenshotMode = true;   // half-second steps: the pages settle in two frames
        ctx_.args = {"scacelith", "--scene", "ui"};
        ctx_.args.insert(ctx_.args.end(), options.begin(), options.end());
        scene_ = createScene("ui");
        ok_ = scene_ && scene_->init(ctx_);
    }
    ~Viewer() {
        ui::im::setInputOverride(nullptr);
        ui::im::setMouseOverride(false);
        ui::im::setKeyboardMode(false);
        ui::setVoiceUpdateHooks(nullptr, nullptr);
        game::settings() = saved_;
    }
    Viewer(const Viewer&) = delete;
    Viewer& operator=(const Viewer&) = delete;

    bool ok() const { return ok_; }
    // One turn of main.cpp's loop: the input delivered, update, the backdrop, the UI. False once the
    // viewer asked to quit.
    bool frame() {
        if (!scene_) return false;
        fakeplat::advance(1.0 / 60.0);
        plat::pumpEvents();
        const bool running = scene_->update(ctx_, 1.0f / 60.0f);
        scene_->render(ctx_, 1.0f / 60.0f);
        scene_->renderOverlay(ctx_, 1.0f / 60.0f);
        return running;
    }
    bool frames(int n) {
        bool running = true;
        for (int i = 0; i < n; ++i) running = frame() && running;
        return running;
    }
    uint64_t drawsOfFrame() {
        const uint64_t before = nullgl::stats().draws;
        frame();
        return nullgl::stats().draws - before;
    }
    // A key pressed for a frame, then released (two frames).
    void key(int k) {
        plat::Input& in = fakeplat::next();
        in.keyDown[k] = in.keyPressed[k] = true;
        frame();
        in.keyDown[k] = false;
        in.keyReleased[k] = true;
        frame();
    }
    // A click at a point in reference pixels (three frames); whether the viewer still runs.
    bool click(float x, float y) {
        plat::Input& in = fakeplat::next();
        const float s = ui::pixelScale();
        in.mouseX = x * s;
        in.mouseY = y * s;
        in.mouseInWindow = true;
        bool running = frame();
        in.mouseDown[plat::MOUSE_LEFT] = in.mousePressed[plat::MOUSE_LEFT] = true;
        running = frame() && running;
        in.mouseDown[plat::MOUSE_LEFT] = false;
        in.mouseReleased[plat::MOUSE_LEFT] = true;
        return frame() && running;
    }

    // The widgets of the last frame.
    const ui::im::TracedItem* find(const std::string& label) const {
        for (const auto& it : ui::im::tracedItems())
            if (it.label == label) return &it;
        return nullptr;
    }
    bool has(const std::string& label) const { return find(label) != nullptr; }
    size_t items() const { return ui::im::tracedItems().size(); }
    // Bare widgets of this size (the saved games' rows, the promotion tiles).
    int bare(float w, float h) const {
        int n = 0;
        for (const auto& it : ui::im::tracedItems()) n += it.label.empty() && it.r.w == w && it.r.h == h ? 1 : 0;
        return n;
    }

private:
    game::Settings saved_;
    AppContext ctx_;
    std::unique_ptr<Scene> scene_;
    bool ok_ = false;
};

// A screen and what it shows: a widget's label (an i18n key), or "" for a screen drawn without one.
struct Shown {
    const char* screen;
    std::vector<std::string> options;
    const char* label;
};

// The draw calls of a frame of a screen that draws nothing but the backdrop.
uint64_t backdropDraws() {
    Viewer v({"--ui-screen", "none"});
    v.frames(2);
    return v.drawsOfFrame();
}

bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    f << text;
    return bool(f);
}
bool exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::u8path(path), ec);
}

}  // namespace

// The keys 1..9 and 0 open the viewer's first ten screens in turn: the title page, New Game, its
// custom engine, Options, the pause menu, the promotion picker, the game over card, the loading
// screen, the move list and the notifications.
TEST(ui_viewer_number_keys_switch_screens) {
    REQUIRE(uitest::app());
    const uint64_t backdrop = backdropDraws();
    const int difficulty = game::settings().difficultyPreset, timeControl = game::settings().timeControlPreset;
    Viewer v({"--ui-screen", "none"});
    REQUIRE(v.ok());
    v.frames(2);
    CHECK_EQ(v.items(), size_t(0));
    const Shown keys[] = {{"1", {}, "menu.new_game"}, {"2", {}, "newgame.start"}, {"3", {}, "engine.skill"},
                          {"4", {}, "options.language"}, {"5", {}, "pause.resume"}, {"6", {}, ""},
                          {"7", {}, "gameover.rematch"}, {"8", {}, ""}, {"9", {}, ""}, {"0", {}, ""}};
    for (const Shown& k : keys) {
        v.key(k.screen[0]);
        v.frames(3);
        if (k.label[0]) {
            CHECK(v.has(T(k.label)));
        } else {
            CHECK(v.drawsOfFrame() > backdrop);
        }
        if (k.screen[0] == '6') CHECK_EQ(v.bare(168.0f, 204.0f), 4);   // the four tiles
        // The custom engine's choices (Custom, the custom clock) last until another screen opens.
        if (k.screen[0] == '3') CHECK_EQ(game::settings().timeControlPreset, -1);
        if (k.screen[0] == '4') {
            CHECK_EQ(game::settings().timeControlPreset, timeControl);
            CHECK_EQ(game::settings().difficultyPreset, difficulty);
        }
    }
}

// The pages of the title menu that --ui-screen opens directly.
TEST(ui_viewer_menu_pages) {
    REQUIRE(uitest::app());
    const Shown pages[] = {
        {"main", {}, "menu.library"},   // the viewer always has a folder of saved games
        {"newgame", {}, "newgame.start"},
        {"newgame-hotseat", {}, "hotseat.white_player"},
        {"custom", {}, "engine.skill"},
        {"credits", {}, "credits.licences"},
        {"licences", {}, "common.back"},
        {"calibration", {}, "calibration.continue"},
        {"watch", {}, "viewer.start"},
        {"options", {"--ui-tab", "3"}, "options.legal_moves"},   // a tab by its number: Gameplay
    };
    for (const Shown& p : pages) {
        std::vector<std::string> options = {"--ui-screen", p.screen};
        options.insert(options.end(), p.options.begin(), p.options.end());
        Viewer v(options);
        REQUIRE(v.ok());
        v.frames(3);
        CHECK(v.has(T(p.label)));
        if (std::string(p.screen) == "licences") CHECK(!v.has(T("credits.licences")));
        // The custom engine: the last difficulty (Custom) and the custom clock, for this run only.
        if (std::string(p.screen) == "custom") CHECK_EQ(game::settings().timeControlPreset, -1);
        if (std::string(p.screen) == "newgame-hotseat") CHECK_EQ(game::settings().opponent, 1);
    }
}

// The screens over the table that --ui-screen opens: the pause menus and their questions, the
// promotion picker for Black, the game over cards (drawn, folded, with the Elo, the viewer's, the
// hot-seat's), the HUDs, the loading screen, the move list, the notices and the handwriting sheet.
TEST(ui_viewer_table_screens) {
    REQUIRE(uitest::app());
    const uint64_t backdrop = backdropDraws();
    const Shown screens[] = {
        {"pause", {}, "pause.resume"},
        {"confirm", {}, "confirm.resign.ok"},
        {"promotion", {"--ui-black"}, ""},
        {"gameover", {"--ui-draw"}, "gameover.rematch"},
        {"gameover-folded", {}, "gameover.show"},
        {"gameover-elo", {}, "gameover.rematch"},
        {"viewer-pause", {}, "viewer.resume"},
        {"viewer-gameover", {}, "viewer.watch_again"},
        {"hotseat-hud", {}, "hotseat.draw.accept"},
        {"hotseat-confirm", {}, "common.cancel"},
        {"hotseat-gameover", {}, "gameover.rematch"},
        {"hud", {}, "pause.offer_draw"},
        {"loading", {}, ""},
        {"movelist", {}, ""},
        {"notify", {}, ""},
        {"hand", {}, ""},
        {"viewer-hud", {}, ""},
    };
    for (const Shown& s : screens) {
        std::vector<std::string> options = {"--ui-screen", s.screen};
        options.insert(options.end(), s.options.begin(), s.options.end());
        Viewer v(options);
        REQUIRE(v.ok());
        v.frames(3);
        if (s.label[0]) {
            CHECK(v.has(T(s.label)));
        } else {
            CHECK(v.drawsOfFrame() > backdrop);
        }
    }
}

// Options from the command line: the tab by its name, the player's name and handwriting (shown by
// the Player tab, never saved: the settings come back when the viewer goes).
TEST(ui_viewer_options_player_tab) {
    REQUIRE(uitest::app());
    const std::string name = game::settings().playerName;
    const auto hand = game::settings().handStyle;
    {
        Viewer v({"--ui-screen", "options", "--ui-tab", "player", "--ui-name", "Zo\xC3\xA9", "--ui-hand", "5"});
        REQUIRE(v.ok());
        v.frames(3);
        CHECK(v.has(T("player.name")));
        CHECK(v.has(T("player.handwriting")));
        CHECK_EQ(game::settings().playerName, std::string("Zo\xC3\xA9"));
        CHECK_EQ(int(game::settings().handStyle), 5 % int(ui::font::HAND_STYLE_COUNT));
    }
    CHECK_EQ(game::settings().playerName, name);
    CHECK(game::settings().handStyle == hand);
}

// Saved games: --ui-library lists the folder given (a row per game), library-empty an empty folder;
// --ui-keys plays its tokens one a frame on the page, with the keyboard focus shown (--ui-kb): the
// selection moved by the arrows and the page keys, the wheel, typed text, Delete asked and
// cancelled by Esc (the file stays), then Enter replays the selected game (back to the title page)
// and a click lands on an empty corner.
TEST(ui_viewer_saved_games_and_scripted_keys) {
    REQUIRE(uitest::app());
    headless::TempDir dir("viewer-library");
    struct Stop {
        ~Stop() { ui::detail::libraryShutdown(); }
    } stop;
    const std::string first = game::archive::joinPath(dir.path, "2026-10-02_100000_play_Ann-vs-Stockfish.pgn");
    const std::string second = game::archive::joinPath(dir.path, "2026-10-01_100000_hotseat_Ben-vs-Cleo.pgn");
    CHECK(writeFile(first, "[Date \"2026.10.02\"]\n[White \"Ann\"]\n[Black \"Stockfish\"]\n[Result \"1-0\"]\n"
                           "[ScacelithMode \"play\"]\n\n1. e4 e5 2. Qh5 Nc6 3. Bc4 Nf6 4. Qxf7# 1-0\n"));
    CHECK(writeFile(second, "[Date \"2026.10.01\"]\n[White \"Ben\"]\n[Black \"Cleo\"]\n[Result \"*\"]\n"
                            "[ScacelithMode \"hotseat\"]\n\n1. d4 *\n"));
    {
        Viewer v({"--ui-screen", "library", "--ui-library", dir.path});
        REQUIRE(v.ok());
        v.frames(3);
        CHECK_EQ(v.bare(704.0f, 62.0f), 2);
        CHECK(v.has(T("library.replay")));
    }
    {
        Viewer v({"--ui-screen", "library-empty"});
        REQUIRE(v.ok());
        v.frames(3);
        CHECK_EQ(v.bare(704.0f, 62.0f), 0);
        CHECK(v.has(T("library.replay")));
    }
    {
        Viewer v({"--ui-screen", "library", "--ui-library", dir.path, "--ui-kb", "--ui-keys",
                  "wait,wait,wait,down,up,pgdn,pgup,end,home,wheel:-1,wheel:1,type:ab,mock:5,tab,bksp,q,del,wait,esc,wait,"
                  "home,wait,enter,wait,click@4:1070,wait"});
        REQUIRE(v.ok());
        v.frames(2);
        CHECK(ui::im::keyboardMode());
        CHECK_EQ(v.bare(704.0f, 62.0f), 2);
        v.frames(26);
        CHECK(ui::titlePageShown());   // Enter replayed the game
        CHECK(v.has(T("menu.library")));
        CHECK(exists(first));          // Delete was asked, then cancelled
        CHECK(exists(second));
    }
}

// --ui-mouse puts the pointer where it says, whatever the platform's mouse does: on the HUD's
// sample button the UI holds the mouse, which it does not with the platform's pointer in a corner.
// Quit on the title page ends the viewer.
TEST(ui_viewer_fake_mouse_and_quit) {
    REQUIRE(uitest::app());
    {
        Viewer v({"--ui-screen", "hud", "--ui-mouse", "180,990"});
        REQUIRE(v.ok());
        v.frames(3);
        CHECK_EQ(ui::im::mouse().x, 180.0f);
        CHECK_EQ(ui::im::mouse().y, 990.0f);
        CHECK(ui::wantsMouse());
    }
    {
        Viewer v({"--ui-screen", "hud"});
        REQUIRE(v.ok());
        v.frames(3);
        CHECK(ui::im::mouse().x < 10.0f);
        CHECK(!ui::wantsMouse());
    }
    Viewer v({"--ui-screen", "main"});
    REQUIRE(v.ok());
    CHECK(v.frames(3));
    const ui::im::TracedItem* quit = v.find(T("menu.quit"));
    REQUIRE(quit != nullptr);
    const Rect r = quit->r;
    v.click(r.cx(), r.cy());
    CHECK(!v.frame());
}
