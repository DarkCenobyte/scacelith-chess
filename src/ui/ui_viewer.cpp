// "ui" viewer scene: every UI screen over a dark marble backdrop (stand-in for the 3D hall).
//   scacelith --scene ui --ui-screen main|newgame|custom|options|credits|pause|confirm|promotion|
//                                     gameover|gameover-folded|loading|movelist|notify|hud|
//                                     watch|viewer-pause|viewer-hud|viewer-gameover|gameover-elo
//   --ui-tab <0..4|display|graphics|audio|gameplay|controls>   options tab
//   --ui-black      promotion picker for Black, --ui-draw   drawn game over card
//   --ui-kb         show the keyboard focus highlight, --ui-mouse X,Y   fake mouse (reference px)
//   --ui-keys a,b,.. scripted input, one token per frame: up down left right enter space esc tab
//                   pgup pgdn wait <letter> click@X:Y (reference px, press + release)
// Interactive: keys 1..9 / 0 switch screens.
#include "ui.h"
#include "ui_internal.h"
#include "ui_widgets.h"
#include "../app/scene.h"
#include "../core/log.h"
#include "../game/settings.h"
#include "../gl/gl46.h"
#include "../i18n/i18n.h"
#include "../platform/platform.h"
#include "../render/gpu.h"
#include "../render/shader.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace {

const char* kScreens[] = {"main", "newgame", "custom", "options", "pause", "promotion", "gameover", "loading", "movelist", "notify"};

class UiViewerScene : public Scene {
public:
    bool init(AppContext& ctx) override {
        saved_ = game::settings();
        ui::setSoundCallback([](ui::Sound s) { LOGD("ui sound %d", int(s)); });
        if (!ui::init()) LOGW("ui viewer: ui::init reported a problem");
        std::string tab = ctx.argValue("--ui-tab", "0");
        static const char* tabs[] = {"display", "graphics", "audio", "gameplay", "controls"};
        tab_ = std::atoi(tab.c_str());
        for (int i = 0; i < 5; ++i)
            if (tab == tabs[i]) tab_ = i;
        black_ = ctx.hasArg("--ui-black");
        drawn_ = ctx.hasArg("--ui-draw");
        kb_ = ctx.hasArg("--ui-kb");
        std::string mouse = ctx.argValue("--ui-mouse");
        float mx = 0, my = 0;
        if (!mouse.empty() && std::sscanf(mouse.c_str(), "%f,%f", &mx, &my) == 2) ui::im::setMouseOverride(true, m::vec2(mx, my));
        open(ctx.argValue("--ui-screen", "main"));
        std::string keys = ctx.argValue("--ui-keys");
        size_t p = 0;
        while (!keys.empty() && p <= keys.size()) {
            size_t q = keys.find(',', p);
            std::string tok = keys.substr(p, q == std::string::npos ? std::string::npos : q - p);
            if (tok.compare(0, 6, "click@") == 0) {  // press, then release on the next frame
                script_.push_back("press@" + tok.substr(6));
                script_.push_back("release@" + tok.substr(6));
            } else if (!tok.empty()) {
                script_.push_back(tok);
            }
            if (q == std::string::npos) break;
            p = q + 1;
        }
        return true;
    }

    // Scripted input for automated interaction checks.
    void scriptFrame() {
        for (int k = 0; k < plat::KEY_COUNT; ++k) fake_.keyPressed[k] = fake_.keyReleased[k] = false;
        for (int b = 0; b < plat::MOUSE_BUTTON_COUNT; ++b) fake_.mousePressed[b] = fake_.mouseReleased[b] = false;
        fake_.wheel = 0;
        if (step_ >= script_.size()) return;
        const std::string tok = script_[step_++];
        static const struct { const char* name; int key; } names[] = {
            {"up", plat::KEY_UP}, {"down", plat::KEY_DOWN}, {"left", plat::KEY_LEFT}, {"right", plat::KEY_RIGHT},
            {"enter", plat::KEY_ENTER}, {"space", plat::KEY_SPACE}, {"esc", plat::KEY_ESCAPE}, {"tab", plat::KEY_TAB},
            {"pgup", plat::KEY_PAGEUP}, {"pgdn", plat::KEY_PAGEDOWN}};
        for (auto& n : names)
            if (tok == n.name) fake_.keyPressed[n.key] = true;
        if (tok.size() == 1 && std::isalpha(static_cast<unsigned char>(tok[0])))
            fake_.keyPressed[std::toupper(static_cast<unsigned char>(tok[0]))] = true;
        float x = 0, y = 0;
        size_t at = tok.find('@');
        if (at != std::string::npos && std::sscanf(tok.c_str() + at + 1, "%f:%f", &x, &y) == 2) {
            float s = ui::pixelScale();
            fake_.mouseX = x * s;
            fake_.mouseY = y * s;
            bool press = tok.compare(0, 5, "press") == 0;
            fake_.mousePressed[plat::MOUSE_LEFT] = press;
            fake_.mouseDown[plat::MOUSE_LEFT] = press;
            fake_.mouseReleased[plat::MOUSE_LEFT] = !press;
        }
        LOGI("ui viewer: script frame %d '%s'", int(step_), tok.c_str());
    }

    void open(const std::string& screen) {
        screen_ = screen;
        game::Settings& s = game::settings();
        s.difficultyPreset = saved_.difficultyPreset;
        s.timeControlPreset = saved_.timeControlPreset;
        if (screen == "newgame") ui::debug::openMenuPage(ui::debug::MenuPage::NewGame);
        if (screen == "custom") {
            s.difficultyPreset = 1 << 20;  // clamped to the last entry = Custom
            s.timeControlPreset = -1;
            ui::debug::openMenuPage(ui::debug::MenuPage::NewGame);
        }
        if (screen == "options") {
            ui::debug::openMenuPage(ui::debug::MenuPage::Options);
            ui::debug::setOptionsTab(tab_);
        }
        if (screen == "credits") ui::debug::openMenuPage(ui::debug::MenuPage::Credits);
        if (screen == "watch") ui::debug::openMenuPage(ui::debug::MenuPage::Watch);
        if (screen == "confirm") ui::debug::openPauseConfirm(1);
        if (screen == "gameover-folded") ui::debug::foldGameOver(true);
        if (screen == "movelist") ui::notify("Touch-move: you must move the knight on g1.", 30.0f);
        if (screen == "notify") {
            ui::notify("Your opponent offers a draw.", 30.0f);
            ui::notify("Illegal move \xE2\x80\x94 two minutes are added to your opponent\xE2\x80\x99s clock.", 30.0f);
        }
        frames_ = 0;
    }

    bool update(AppContext& ctx, float dt) override {
        time_ += dt;
        const plat::Input& in = plat::input();
        for (int i = 0; i < 10; ++i) {
            int key = i == 9 ? '0' : '1' + i;
            if (in.keyPressed[key]) open(kScreens[i]);
        }
        (void)ctx;
        return !quit_;
    }

    void render(AppContext&, float) override {
        int w = plat::width(), h = plat::height();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        const ShaderProgram& p = shaders::fullscreen("shaders/ui/backdrop.frag");
        if (p.valid()) {
            p.use();
            p.set("uResolution", float(w), float(h));
            p.set("uTime", time_);
            gpu::drawFullscreenTriangle();
        } else {
            glClearColor(0.03f, 0.028f, 0.025f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    }

    void renderOverlay(AppContext& ctx, float dt) override {
        // Screenshots use few frames: a large step lets every appear animation settle.
        if (!script_.empty()) {
            scriptFrame();
            ui::im::setInputOverride(&fake_);
        }
        ui::beginFrame(plat::width(), plat::height(), ctx.screenshotMode ? 0.5f : dt);
        if (kb_) ui::im::setKeyboardMode(true);
        ui::MenuAction a = ui::MenuAction::None;
        const std::string& s = screen_;
        if (s == "main" || s == "newgame" || s == "custom" || s == "options" || s == "credits" || s == "watch") {
            a = ui::mainMenu(setup_, watch_);
        } else if (s == "viewer-pause") {
            a = ui::viewerPauseMenu();
        } else if (s == "viewer-hud") {
            ui::ViewerHud hud;
            hud.white = i18n::trf("viewer.player", {"Master", "2400"});
            hud.black = i18n::trf("viewer.player", {"Expert", "2100"});
            hud.sideToMove = 1;
            hud.viewpoint = i18n::tr("viewer.view.7");
            hud.viewpointAge = 0.5f;
            ui::viewerHud(hud);
        } else if (s == "viewer-gameover" || s == "gameover-elo") {
            ui::GameOverExtras x;
            if (s == "viewer-gameover") {
                x.line = i18n::trf("viewer.gameover.white_wins", {"47"});
                x.detail = i18n::trf("viewer.gameover.players", {"Master (2400)", "Expert (2100)"});
                x.primaryLabel = i18n::tr("viewer.watch_again");
            } else {
                x.detail = i18n::trf("elo.change", {"1500", "1524", "+24"});
            }
            a = ui::gameOver("1-0", "Checkmate", s == "gameover-elo", false, 47, x);
        } else if (s == "pause" || s == "confirm") {
            a = ui::pauseMenu(true);
        } else if (s == "promotion") {
            int piece = ui::promotionPicker(!black_);
            if (piece) LOGI("ui viewer: promotion -> %d", piece);
        } else if (s == "gameover" || s == "gameover-folded") {
            a = drawn_ ? ui::gameOver("½-½", "Threefold repetition", false, true, 41)
                       : ui::gameOver("1-0", "Checkmate", true, false, 34);
        } else if (s == "loading") {
            ui::loadingScreen(0.62f, "Baking the light of the hall\xE2\x80\xA6");
        } else if (s == "movelist" || s == "notify") {
            static const std::vector<std::string> opera = {
                "e4", "e5", "Nf3", "d6", "d4", "Bg4", "dxe5", "Bxf3", "Qxf3", "dxe5", "Bc4", "Nf6", "Qb3", "Qe7", "Nc3", "c6", "Bg5",
                "b5", "Nxb5", "cxb5", "Bxb5+", "Nbd7", "O-O-O", "Rd8", "Rxd7", "Rxd7", "Rd1", "Qe6", "Bxd7+", "Nxd7", "Qb8+", "Nxb8", "Rd8#"};
            ui::moveList(opera, s == "movelist");
        } else if (s == "hud") {
            m::vec2 v = ui::viewSize();
            ui::panel(m::vec2(v.x - 420.0f, 60.0f), m::vec2(360.0f, 150.0f));
            ui::text("WHITE", m::vec2(v.x - 390.0f, 84.0f), 20.0f, m::vec4(0.79f, 0.66f, 0.42f, 1.0f), ui::Align::Left, ui::FontStyle::Title, 0.2f);
            ui::text("4:59", m::vec2(v.x - 90.0f, 74.0f), 64.0f, m::vec4(0.93f, 0.9f, 0.83f, 1.0f), ui::Align::Right);
            ui::text("Your move", m::vec2(v.x - 390.0f, 150.0f), 24.0f, m::vec4(0.76f, 0.72f, 0.65f, 1.0f), ui::Align::Left, ui::FontStyle::Italic);
            if (ui::button("Offer draw", m::vec2(60.0f, v.y - 120.0f), m::vec2(240.0f, 56.0f))) LOGI("ui viewer: hud button");
        }
        ui::drawNotifications();
        if (a != ui::MenuAction::None) {
            LOGI("ui viewer: action %d", int(a));
            if (a == ui::MenuAction::Quit) quit_ = true;
        }
        ui::endFrame();
        ++frames_;
    }

    void shutdown(AppContext&) override {
        ui::im::setInputOverride(nullptr);
        ui::shutdown();
        game::settings() = saved_;  // the viewer never persists its test choices
    }

private:
    game::Settings saved_;
    ui::NewGameSetup setup_;
    ui::WatchSetup watch_;
    std::string screen_;
    int tab_ = 0;
    bool black_ = false, drawn_ = false, kb_ = false, quit_ = false;
    float time_ = 0.0f;
    int frames_ = 0;
    std::vector<std::string> script_;
    size_t step_ = 0;
    plat::Input fake_;
};

}  // namespace

SCACELITH_SCENE("ui", "UI viewer: menus and screens over a marble backdrop (--ui-screen <name>)", UiViewerScene);
