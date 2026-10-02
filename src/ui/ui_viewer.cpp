// "ui" viewer scene: every UI screen over a dark marble backdrop (stand-in for the 3D hall).
//   scacelith --scene ui --ui-screen main|newgame|custom|options|credits|pause|confirm|promotion|
//                                     gameover|gameover-folded|loading|movelist|notify|hud|hand|
//                                     watch|viewer-pause|viewer-hud|viewer-gameover|gameover-elo|
//                                     calibration (first start; the slider starts at the .ini's
//                                     brightness)
//   coach mode: coach (the coach page), coach-novoice (the same without the voice files), licences
//     (credits > Licences), coach-hud (a subtitle, the takeback offer card, the skip hint),
//     coach-subtitle (a subtitle alone; --ui-text <text> replaces the sample line), coach-pause,
//     coach-gameover, coach-lesson-done
//   coach voice download (sample figures): coach-download (the prompt over the Coach page),
//     coach-download-licence (its licence view), coach-download-hub / coach-download-github /
//     coach-download-extracting / coach-download-failed (the progress panel; github over the
//     title page); coach-flow: the real thing over the title page (game/coach_model.h: the
//     Coach entry's prompt, a real download into --coach-dir <folder>, the panel, the notices)
//   saved games: library (the page; --ui-library <folder> lists that folder, default the pgn folder
//     of the user data directory), library-empty (the same, its folder replaced by an empty one
//     when --ui-library is not given); the title page ("main") shows the "Saved games" entry
//   hot-seat (two players on one PC): newgame-hotseat (New Game with "Human, same PC"),
//     hotseat-hud (players, caption, draw offer card), hotseat-confirm (named resignation),
//     hotseat-gameover (both names and ratings)
//   online pages (in-process mock server, frozen clock): online (sign in), online-register,
//     online-mfa (code step), online-play, online-search, online-account, online-mfa-setup,
//     online-recovery, online-challenge, online-private, online-noserver, direct, direct-host,
//     direct-wait, direct-join; the account API's pages: online-history, online-game (a game of
//     the history), online-game-gif (its GIF saved: a file written to the GIF folder),
//     online-game-gif-making (the GIF being made), online-game-saving (Save game pressed: the
//     PGN on its way), online-game-saved (the PGN written to the saved games), online-devices,
//     online-email, online-email-sent, online-export, online-export-done, online-delete; at the table:
//     online-hud, online-pause, online-report, online-gameover; Saved games signed in:
//     library-gif (Save as GIF enabled), library-gif-done (the selected game's GIF saved)
//   --ui-tab <0..6|display|graphics|audio|gameplay|player|online|controls>   options tab
//   --lang <code>   interface language (en fr de es uk ar ru ja zh-Hant zh-Hans; read by game::Settings)
//   --ui-name <name>, --ui-hand <0..2>   player name / handwriting shown by Options > Player
//   "hand": sample names in every script, written in each handwriting style
//   --ui-black      promotion picker for Black, --ui-draw   drawn game over card
//   --ui-kb         show the keyboard focus highlight, --ui-mouse X,Y   fake mouse (reference px)
//   --ui-keys a,b,.. scripted input, one token per frame: up down left right enter space esc tab
//                   pgup pgdn home end bksp del wait <letter> click@X:Y (reference px, press +
//                   release) type:<text> (typed characters) wheel:<notches> (negative: down)
// Interactive: keys 1..9 / 0 switch screens.
#include "ui.h"
#include "ui_internal.h"
#include "ui_online.h"
#include "ui_screens_online.h"
#include "ui_widgets.h"
#include "../app/scene.h"
#include "../core/log.h"
#include "../chess/chess.h"
#include "../game/online_session.h"
#include "../game/settings.h"
#include "../i18n/i18n.h"
#include "../i18n/unicode.h"
#include "../gl/gl46.h"
#include "../platform/platform.h"
#include "../render/gpu.h"
#include "../render/shader.h"
#include "../game/coach_model.h"
#include "../tts/model_store.h"
#include "../tts/tts.h"
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
        static const char* tabs[] = {"display", "graphics", "audio", "gameplay", "player", "online", "controls"};
        tab_ = std::atoi(tab.c_str());
        for (int i = 0; i < 7; ++i)
            if (tab == tabs[i]) tab_ = i;
        if (ctx.hasArg("--ui-name")) game::settings().playerName = ctx.argValue("--ui-name");
        if (ctx.hasArg("--ui-hand"))
            game::settings().handStyle = ui::font::HandStyle(std::atoi(ctx.argValue("--ui-hand").c_str()) % ui::font::HAND_STYLE_COUNT);
        black_ = ctx.hasArg("--ui-black");
        drawn_ = ctx.hasArg("--ui-draw");
        text_ = ctx.argValue("--ui-text");
        coachDir_ = ctx.argValue("--coach-dir");
        kb_ = ctx.hasArg("--ui-kb");
        std::string mouse = ctx.argValue("--ui-mouse");
        float mx = 0, my = 0;
        if (!mouse.empty() && std::sscanf(mouse.c_str(), "%f,%f", &mx, &my) == 2) ui::im::setMouseOverride(true, m::vec2(mx, my));
        // Saved games: the folder the library page lists.
        library_.folder = ctx.argValue("--ui-library", plat::userDataDirectory() + "pgn/");
        if (ctx.argValue("--ui-screen", "main") == "library-empty" && !ctx.hasArg("--ui-library"))
            library_.folder = plat::userDataDirectory() + "pgn-viewer-empty/";
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
        fake_.textCount = 0;
        if (step_ >= script_.size()) return;
        const std::string tok = script_[step_++];
        if (tok.compare(0, 6, "wheel:") == 0) {  // mouse wheel notches (negative: down), at the mouse
            fake_.wheel = float(std::atof(tok.c_str() + 6));
            LOGI("ui viewer: script frame %d '%s'", int(step_), tok.c_str());
            return;
        }
        if (tok.compare(0, 5, "type:") == 0) {  // typed characters, as WM_CHAR / XLookupString deliver them
            for (char32_t c : uni::decode(tok.substr(5)))
                if (fake_.textCount < int(sizeof(fake_.text) / sizeof(fake_.text[0]))) fake_.text[fake_.textCount++] = uint32_t(c);
            LOGI("ui viewer: script frame %d '%s'", int(step_), tok.c_str());
            return;
        }
        static const struct { const char* name; int key; } names[] = {
            {"up", plat::KEY_UP}, {"down", plat::KEY_DOWN}, {"left", plat::KEY_LEFT}, {"right", plat::KEY_RIGHT},
            {"enter", plat::KEY_ENTER}, {"space", plat::KEY_SPACE}, {"esc", plat::KEY_ESCAPE}, {"tab", plat::KEY_TAB},
            {"pgup", plat::KEY_PAGEUP}, {"pgdn", plat::KEY_PAGEDOWN}, {"home", plat::KEY_HOME}, {"end", plat::KEY_END},
            {"bksp", plat::KEY_BACKSPACE}, {"del", plat::KEY_DELETE}};
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
        s.opponent = saved_.opponent;
        if (screen == "newgame") ui::debug::openMenuPage(ui::debug::MenuPage::NewGame);
        if (screen == "newgame-hotseat") {
            s.opponent = 1;
            ui::debug::openMenuPage(ui::debug::MenuPage::NewGame);
        }
        if (screen == "hotseat-confirm") ui::debug::openPauseConfirm(1);
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
        if (screen == "licences") ui::debug::openMenuPage(ui::debug::MenuPage::Licences);
        if (screen == "coach" || screen == "coach-novoice") {
            ui::debug::openMenuPage(ui::debug::MenuPage::Coach);
            coach_.voiceAvailable = screen == "coach";
        }
        if (screen == "coach-flow") {
            if (!coachDir_.empty()) tts::setModelDirectory(coachDir_);
            game::coachModelInit();
            coach_.voiceAvailable = game::coachVoiceWanted();
        }
        if (screen.compare(0, 14, "coach-download") == 0) {
            ui::debug::openMenuPage(screen == "coach-download-github" ? ui::debug::MenuPage::Title : ui::debug::MenuPage::Coach);
            coach_.voiceAvailable = false;
            if (screen == "coach-download-licence") ui::debug::showModelLicence();
        }
        if (screen == "calibration") ui::debug::openMenuPage(ui::debug::MenuPage::Calibration);
        if (screen == "watch") ui::debug::openMenuPage(ui::debug::MenuPage::Watch);
        if (screen == "library" || screen == "library-empty") ui::debug::openMenuPage(ui::debug::MenuPage::Library);
        if (screen == "library-gif" || screen == "library-gif-done") {
            ui::debug::openMenuPage(ui::debug::MenuPage::Library);
            openOnline(screen);  // signed in to the fake server
            menu_ = true;
            if (screen == "library-gif-done") ui::debug::libraryGif();
        }
        if (screen == "confirm") ui::debug::openPauseConfirm(1);
        if (screen == "gameover-folded") ui::debug::foldGameOver(true);
        if (screen.compare(0, 6, "online") == 0 || screen.compare(0, 6, "direct") == 0) openOnline(screen);
        if (screen == "movelist") ui::notify(i18n::trf("notify.touched_square", {"g1"}), 30.0f);
        if (screen == "notify") {
            ui::notify(i18n::tr("notify.draw_declined"), 30.0f);
            ui::notify(i18n::tr("arbiter.illegal") + std::string(" ") + i18n::tr("arbiter.restored_two_minutes.black"), 30.0f);
        }
    }

    // Online screens: the in-process mock server with a virtual clock that only moves here (the
    // pages are drawn as they are, not while the fake answers).
    void openOnline(const std::string& screen) {
        game::OnlineSession& s = game::onlineSession();
        s.init(true, true);
        online_ = true;
        if (!s.infoKnown()) {
            s.refreshInfo();
            s.runMock(1000.0);
        }
        bool account = screen != "online" && screen != "online-register" && screen != "online-mfa" && screen != "online-noserver" &&
                       screen.compare(0, 6, "direct") != 0;
        if (account && !s.signedIn()) {
            s.api().login("Magnus_T", "viewer-password");
            s.expect(net::Event::Kind::LoginResult);
            s.runMock(3000.0);
            net::Event e;
            s.take(net::Event::Kind::LoginResult, e);
        }
        if (screen == "online-hud" || screen == "online-play") s.runMock(26000.0);  // a challenge received
        static const struct { const char* screen; const char* sub; } pages[] = {
            {"online", "signin"}, {"online-register", "register"}, {"online-mfa", "mfa"}, {"online-play", "play"},
            {"online-search", "search"}, {"online-account", "account"}, {"online-mfa-setup", "mfa-setup"},
            {"online-recovery", "recovery"}, {"online-challenge", "challenge"}, {"online-private", "private"},
            {"online-noserver", "noserver"}, {"direct", "direct"}, {"direct-host", "direct-host"}, {"direct-wait", "direct-wait"},
            {"direct-join", "direct-join"}, {"online-history", "history"}, {"online-game", "game"},
            {"online-game-gif", "game-gif"}, {"online-game-gif-making", "game-gif-making"},
            {"online-game-saving", "game-saving"}, {"online-game-saved", "game-saved"},
            {"online-devices", "devices"}, {"online-email", "email"}, {"online-email-sent", "email-sent"},
            {"online-export", "export"}, {"online-export-done", "export-done"}, {"online-delete", "delete"},
        };
        for (const auto& p : pages)
            if (screen == p.screen) {
                ui::debug::openOnlineMenu(p.sub);
                menu_ = true;
            }
    }

    // Names in every script the game supports, each written in the three handwriting styles: the
    // Latin and Cyrillic rows change with the style, the others keep the hand of their script.
    void handSheet() {
        namespace gfx = ui::gfx;
        m::vec2 v = gfx::viewSize();
        static const char* names[] = {
            "\xC3\x89lodie Lef\xC3\xA8vre",                                              // Latin
            "\xD0\x9E\xD0\xBB\xD0\xB5\xD0\xBA\xD1\x81\xD0\xB0\xD0\xBD\xD0\xB4\xD1\x80 \xD0\x86\xD0\xB2\xD0\xB0\xD0\xBD\xD0\xB5\xD0\xBD\xD0\xBA\xD0\xBE",  // Ukrainian
            "\xD0\x94\xD0\xBC\xD0\xB8\xD1\x82\xD1\x80\xD0\xB8\xD0\xB9 \xD0\x92\xD0\xBE\xD0\xBB\xD0\xBA\xD0\xBE\xD0\xB2",  // Russian
            "\xD9\x85\xD8\xAD\xD9\x85\xD8\xAF \xD8\xB9\xD8\xA8\xD8\xAF \xD8\xA7\xD9\x84\xD9\x84\xD9\x87",  // Arabic
            "\xD9\x81\xD8\xA7\xD8\xB7\xD9\x85\xD8\xA9 \xD8\xA7\xD9\x84\xD8\xB2\xD9\x87\xD8\xB1\xD8\xA7\xD8\xA1",  // Arabic
            "\xE4\xBD\x90\xE8\x97\xA4 \xE3\x81\x95\xE3\x81\x8F\xE3\x82\x89",  // Japanese
            "\xE7\x8E\x8B\xE5\xB0\x8F\xE6\x98\x8E",                                  // Simplified Chinese
            "\xE9\x99\xB3\xE5\xA4\xA7\xE6\x96\x87",                                  // Traditional Chinese
        };
        const int rows = int(sizeof(names) / sizeof(names[0]));
        float w = std::min(1700.0f, v.x - 80.0f), h = 150.0f + 92.0f * float(rows);
        gfx::Rect p(v.x * 0.5f - w * 0.5f, v.y * 0.5f - h * 0.5f, w, h);
        gfx::shadow(p.offset(0, 10), 3, 40, m::vec4(0, 0, 0, 0.6f));
        gfx::fillV(p, m::vec4(0.95f, 0.93f, 0.86f, 1.0f), m::vec4(0.89f, 0.86f, 0.78f, 1.0f), 3.0f);
        float colW = (w - 80.0f) / float(ui::font::HAND_STYLE_COUNT);
        gfx::TextStyle hs;
        hs.face = ui::font::FACE_TITLE;
        hs.size = 20.0f;
        hs.tracking = 0.2f;
        hs.align = gfx::HAlign::Center;
        hs.color = m::vec4(0.45f, 0.30f, 0.15f, 1.0f);
        for (int c = 0; c < ui::font::HAND_STYLE_COUNT; ++c)
            gfx::text(ui::font::handStyleName(c), p.x + 40.0f + colW * (float(c) + 0.5f), p.y + 70.0f, hs);
        for (int r = 0; r < rows; ++r) {
            float base = p.y + 170.0f + 92.0f * float(r);
            gfx::hline(p.x + 30.0f, p.r() - 30.0f, base + 14.0f, m::vec4(0.32f, 0.42f, 0.58f, 0.35f));
            for (int c = 0; c < ui::font::HAND_STYLE_COUNT; ++c) {
                gfx::TextStyle ts;
                ts.hand = c;
                ts.size = 52.0f;
                ts.color = m::vec4(0.10f, 0.13f, 0.30f, 0.95f);
                ts.align = gfx::HAlign::Center;
                ts.size = gfx::fitSize(names[r], ts, colW - 30.0f, 0.5f);
                gfx::text(names[r], p.x + 40.0f + colW * (float(c) + 0.5f), base, ts);
            }
        }
    }

    bool update(AppContext& ctx, float) override {
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
        if (online_) game::onlineSession().update(0.0f);  // events only: the mock's clock stays still
        if (s == "main" || s == "newgame" || s == "newgame-hotseat" || s == "custom" || s == "options" || s == "credits" ||
            s == "watch" || s == "calibration" || s == "coach" || s == "coach-novoice" || s == "licences" || s == "library" ||
            s == "library-empty" || menu_ || s.compare(0, 14, "coach-download") == 0 || s == "coach-flow") {
            a = ui::mainMenu(setup_, watch_, coach_, library_);
            if (a == ui::MenuAction::StartReplay)
                LOGI("ui viewer: replay %s, game %d", library_.replay.path.c_str(), library_.replay.game);
        } else if (s == "coach-hud" || s == "coach-subtitle") {
            ui::Subtitle sub;
            sub.text = text_.empty() ? i18n::tr("coach.offer.text") : text_;
            sub.age = 1.0f;
            sub.duration = ui::subtitleDuration(sub.text, 4.0f);
            ui::subtitles(sub);
            if (s == "coach-hud") {
                ui::CoachHud hud;
                hud.offer = true;
                hud.skippable = true;
                ui::CoachHudAction ca = ui::coachHud(hud);
                if (ca != ui::CoachHudAction::None) LOGI("ui viewer: coach hud -> %d", int(ca));
            }
        } else if (s == "coach-pause") {
            ui::CoachPause p;
            p.canTakeBack = true;
            p.canOfferDraw = true;
            p.canClaimDraw = true;
            a = ui::coachPauseMenu(p);
        } else if (s == "coach-gameover") {
            ui::GameOverExtras x;
            x.detail = i18n::tr("coach.gameover.unrated");
            x.primaryLabel = i18n::tr("coach.gameover.again");
            a = ui::gameOver("0-1", chess::endReasonText(chess::GameEndReason::Checkmate), false, false, 38, x);
        } else if (s == "coach-lesson-done") {
            ui::GameOverExtras x;
            x.line = i18n::tr("coach.lesson.done.line");
            x.primaryLabel = i18n::tr("coach.lesson.next");
            a = ui::gameOver(i18n::tr("coach.lesson.done"), i18n::tr("coach.level.0.name"), true, false, -1, x);
        } else if (s == "hotseat-hud") {
            ui::HotSeatHud hud;
            hud.names[0] = "Alice";
            hud.names[1] = "Bob";
            hud.ratings[0] = "1512";
            hud.ratings[1] = "1488";
            hud.toMove = 1;
            hud.caption = i18n::trf("hotseat.your_move", {hud.names[1]});
            hud.captionAge = 1.0f;
            hud.drawOffer = true;
            hud.drawOfferText = i18n::trf("hotseat.draw.offered", {hud.names[0]});
            ui::hotSeatHud(hud);
        } else if (s == "hotseat-confirm") {
            a = ui::pauseMenu(true, true, i18n::trf("hotseat.confirm.resign", {"Alice", "Bob"}));
        } else if (s == "hotseat-gameover") {
            ui::GameOverExtras x;
            x.line = i18n::trn("hotseat.gameover.wins", 34, {"Alice", "34"});
            x.detail = i18n::trf("hotseat.elo.change", {"Alice", "1500", "1520", i18n::ltr("+20")}) + "  \xC2\xB7  " +
                       i18n::trf("hotseat.elo.change", {"Bob", "1500", "1480", i18n::ltr("\xE2\x88\x92" "20")});
            a = ui::gameOver("1-0", chess::endReasonText(chess::GameEndReason::Checkmate), true, false, 34, x);
        } else if (s == "online-hud") {
            ui::OnlineHud hud;
            hud.pingMs = 34;
            hud.countdown.clear();
            hud.banner = i18n::trf("online.opponent_away", {game::durationText(45000.0)});
            hud.drawOffer = true;
            ui::onlineHud(hud);
            ui::onlineChallenges();
        } else if (s == "online-pause") {
            ui::OnlinePause p;
            p.canClaimDraw = false;
            a = ui::onlinePauseMenu(p);
        } else if (s == "online-report") {
            static int cat = 0;
            static std::string comment = "Moves at a steady 2 s all game";
            ui::pingIndicator(41, false);
            if (ui::reportDialog(cat, comment) >= 0) LOGI("ui viewer: report closed");
        } else if (s == "online-gameover") {
            ui::GameOverExtras x;
            x.detail = i18n::trf("online.rating.change", {"1500", "1512", i18n::ltr("+12")});
            x.reportLabel = i18n::tr("online.report.button");
            ui::pingIndicator(33, false);
            a = ui::gameOver("1-0", i18n::tr("reason.online.abandonment"), true, false, 31, x);
        } else if (s == "viewer-pause") {
            a = ui::viewerPauseMenu();
        } else if (s == "viewer-hud") {
            ui::ViewerHud hud;
            hud.white = i18n::trf("viewer.player", {ui::presetName("Master"), "2400"});
            hud.black = i18n::trf("viewer.player", {ui::presetName("Expert"), "2100"});
            hud.sideToMove = 1;
            hud.viewpoint = i18n::tr("viewer.view.7");
            hud.viewpointAge = 0.5f;
            ui::viewerHud(hud);
        } else if (s == "viewer-gameover" || s == "gameover-elo") {
            ui::GameOverExtras x;
            if (s == "viewer-gameover") {
                x.line = i18n::trf("viewer.gameover.white_wins", {"47"});
                x.detail = i18n::trf("viewer.gameover.players", {ui::presetName("Master") + " (2400)", ui::presetName("Expert") + " (2100)"});
                x.primaryLabel = i18n::tr("viewer.watch_again");
            } else {
                x.detail = i18n::trf("elo.change", {"1500", "1524", i18n::ltr("+24")});
            }
            a = ui::gameOver("1-0", chess::endReasonText(chess::GameEndReason::Checkmate), s == "gameover-elo", false, 47, x);
        } else if (s == "pause" || s == "confirm") {
            a = ui::pauseMenu(true);
        } else if (s == "promotion") {
            int piece = ui::promotionPicker(!black_);
            if (piece) LOGI("ui viewer: promotion -> %d", piece);
        } else if (s == "gameover" || s == "gameover-folded") {
            a = drawn_ ? ui::gameOver("\xC2\xBD-\xC2\xBD", chess::endReasonText(chess::GameEndReason::ThreefoldClaim), false, true, 41)
                       : ui::gameOver("1-0", chess::endReasonText(chess::GameEndReason::Checkmate), true, false, 34);
        } else if (s == "loading") {
            ui::loadingScreen(0.62f, i18n::tr("loading.porcelain"));
        } else if (s == "movelist" || s == "notify") {
            static const std::vector<std::string> opera = {
                "e4", "e5", "Nf3", "d6", "d4", "Bg4", "dxe5", "Bxf3", "Qxf3", "dxe5", "Bc4", "Nf6", "Qb3", "Qe7", "Nc3", "c6", "Bg5",
                "b5", "Nxb5", "cxb5", "Bxb5+", "Nbd7", "O-O-O", "Rd8", "Rxd7", "Rxd7", "Rd1", "Qe6", "Bxd7+", "Nxd7", "Qb8+", "Nxb8", "Rd8#"};
            ui::moveList(opera, s == "movelist");
        } else if (s == "hud") {
            m::vec2 v = ui::viewSize();
            ui::panel(m::vec2(v.x - 420.0f, 60.0f), m::vec2(360.0f, 150.0f));
            ui::text(uni::toUpper(i18n::tr("common.white")), m::vec2(v.x - 390.0f, 84.0f), 20.0f, m::vec4(0.79f, 0.66f, 0.42f, 1.0f), ui::Align::Left, ui::FontStyle::Title, 0.2f);
            ui::text("4:59", m::vec2(v.x - 90.0f, 74.0f), 64.0f, m::vec4(0.93f, 0.9f, 0.83f, 1.0f), ui::Align::Right);
            ui::text(i18n::tr("notify.press_clock"), m::vec2(v.x - 390.0f, 150.0f), 24.0f, m::vec4(0.76f, 0.72f, 0.65f, 1.0f), ui::Align::Left, ui::FontStyle::Italic);
            if (ui::button(i18n::tr("pause.offer_draw"), m::vec2(60.0f, v.y - 120.0f), m::vec2(240.0f, 56.0f))) LOGI("ui viewer: hud button");
        } else if (s == "hand") {
            handSheet();
        }
        if (s.compare(0, 14, "coach-download") == 0) modelDownloadSample(s);
        if (s == "coach-flow") {
            game::drawModelDownload();
            if (game::coachModelInstalled()) LOGI("ui viewer: coach voice installed");
            coach_.voiceAvailable = game::coachVoiceWanted();
        }
        ui::drawNotifications();
        if (a != ui::MenuAction::None) {
            LOGI("ui viewer: action %d", int(a));
            if (a == ui::MenuAction::Quit) quit_ = true;
        }
        ui::endFrame();
    }

    // The coach voice download's prompt and progress panel with sample figures (nothing is
    // downloaded here).
    void modelDownloadSample(const std::string& s) {
        const tts::ModelManifest& m = tts::supertonicManifest();
        if (s == "coach-download" || s == "coach-download-licence") {
            ui::ModelPrompt p;
            p.bytes = double(m.totalBytes());
            p.folder = tts::modelFolder();
            ui::ModelPromptAction pa = ui::modelPrompt(p);
            if (pa != ui::ModelPromptAction::None) LOGI("ui viewer: model prompt -> %d", int(pa));
            return;
        }
        ui::ModelProgressView v;
        if (s == "coach-download-hub") {
            v.state = ui::ModelProgressView::State::Downloading;
            v.done = 63.2e6;
            v.total = double(m.totalBytes());
            v.sourceLabel = m.hubLabel;
            v.file = "vector_estimator.int8.onnx";
        } else if (s == "coach-download-github") {
            v.state = ui::ModelProgressView::State::Downloading;
            v.done = 41.0e6;
            v.total = double(m.archiveSize);
            v.github = true;
            v.sourceLabel = m.archiveLabel;
            v.file = m.archiveName();
        } else if (s == "coach-download-extracting") {
            v.state = ui::ModelProgressView::State::Extracting;
            v.done = 88.0e6;
            v.total = double(m.archiveSize);
            v.github = true;
            v.sourceLabel = m.archiveLabel;
        } else if (s == "coach-download-failed") {
            v.state = ui::ModelProgressView::State::Failed;
            v.error = i18n::tr("coach.download.error.network");
        }
        ui::ModelPanelAction pa = ui::modelProgressPanel(v);
        if (pa != ui::ModelPanelAction::None) LOGI("ui viewer: model panel -> %d", int(pa));
    }

    void shutdown(AppContext&) override {
        if (screen_ == "coach-flow") game::coachModelShutdown();
        ui::im::setInputOverride(nullptr);
        ui::shutdown();
        game::settings() = saved_;  // the viewer never persists its test choices
    }

private:
    game::Settings saved_;
    ui::NewGameSetup setup_;
    ui::WatchSetup watch_;
    ui::CoachSetup coach_;
    ui::LibrarySetup library_;
    std::string text_;
    std::string coachDir_;
    std::string screen_;
    int tab_ = 0;
    bool black_ = false, drawn_ = false, kb_ = false, quit_ = false;
    bool online_ = false, menu_ = false;
    std::vector<std::string> script_;
    size_t step_ = 0;
    plat::Input fake_;
};

}  // namespace

SCACELITH_SCENE("ui", "UI viewer: menus and screens over a marble backdrop (--ui-screen <name>)", UiViewerScene);
