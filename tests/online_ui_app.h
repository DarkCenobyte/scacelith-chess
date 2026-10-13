// The headless game of the online tests (online_scene_*_tests.cpp, online_ui_*_tests.cpp): the
// game scene with --online-mock on the fakes' virtual clock (--warp 0: the fakes' time moves only
// with the frames, 1/60 s each, so that every answer comes after the same number of frames), and
// helpers to drive its online pages and games by what they show. The human player of these games
// makes one move by hand when it has White (--play e2e4); with Black it makes none.
//
// game::onlineSession() is one object for the whole process, set up once by the first game scene
// that starts (GameScene::initOnline): after the tests of other files it talks to the real network
// layer. onlineApp() therefore stops the App those tests shared and puts a new session in its place
// before it starts the game with --online-mock, and gives each test a new session of the fakes (as
// the game scene sets it up: in-process fakes, virtual clock), signed out, with nothing waiting:
// each test starts from the same state, whatever ran before it. The tests of one file share the
// App; the last test of each file holds a ReleaseAtEnd, which stops it and leaves a new session
// that is not set up, so that the files after it start as they would have without these tests
// (the test runner may also stop a shared App between two files: onlineApp() then starts it
// again).
#pragma once
#include "test.h"
#include "headless.h"
#include "fake_platform.h"
#include "game/online_mock.h"
#include "game/online_session.h"
#include "game/settings.h"
#include "platform/platform.h"
#include "ui/ui_draw.h"
#include "ui/ui_widgets.h"
#include <algorithm>
#include <new>
#include <string>
#include <vector>

namespace onlinetest {

using Kind = net::Event::Kind;

inline const std::vector<std::string>& appArgs() {
    static const std::vector<std::string> args = {"--no-intro", "--online-mock", "--warp", "0", "--play", "e2e4"};
    return args;
}

// Whether the App the online tests share is the one running (an inline function's static: one
// for every file that includes this header).
inline bool& appStarted() {
    static bool started = false;
    return started;
}

// The process's online session as a new one, not set up (the next game scene, or init(), does it),
// signed out, nothing waiting; the switches of the fakes back to their defaults. Only while no
// scene holds a game of it (the GameLink it gave).
inline void freshSession() {
    game::OnlineSession& s = game::onlineSession();
    s.~OnlineSession();
    new (&s) game::OnlineSession();
    net::mock::useVirtualClock(false);
    net::mock::useManualClock(false);
    net::mock::forceOpponentStance(-1);
}

// The online settings the tests start from (the official server, rated 5+3, colours by lot).
inline void defaultOnlineSettings() {
    game::Settings& gs = game::settings();
    const game::Settings d;
    gs.onlineCustomServer = d.onlineCustomServer;
    gs.onlineHost = d.onlineHost;
    gs.onlineApiPort = d.onlineApiPort;
    gs.onlineWsPort = d.onlineWsPort;
    gs.onlinePin = d.onlinePin;
    gs.onlineCategory = d.onlineCategory;
    gs.onlineRated = d.onlineRated;
    gs.onlineColor = d.onlineColor;
    gs.onlineCustomBaseSeconds = d.onlineCustomBaseSeconds;
    gs.onlineCustomIncrementSeconds = d.onlineCustomIncrementSeconds;
    gs.directPort = d.directPort;
    gs.directUpnp = d.directUpnp;
    gs.directColor = d.directColor;
    gs.directAutoPress = d.directAutoPress;
    gs.directTimeControl = d.directTimeControl;
    gs.directBaseSeconds = d.directBaseSeconds;
    gs.directIncrementSeconds = d.directIncrementSeconds;
    gs.directAddress = d.directAddress;
    gs.directJoinPort = d.directJoinPort;
}

// The game with --online-mock on its title page, with a new session of the fakes (see above).
// The App is started on first use (and when the test runner stopped it between two files), and
// again when the last test left it elsewhere than on the title page (it failed).
inline headless::App* onlineApp() {
    if (!appStarted()) {
        headless::stopShared();
        freshSession();
    }
    headless::App* app = headless::shared(appArgs());
    appStarted() = app != nullptr;
    if (!app) return nullptr;
    if (app->frameCount() > 0 && (!app->hasLabel("Play Online") || game::onlineSession().link())) {
        headless::stopShared();
        freshSession();
        app = headless::shared(appArgs());
        appStarted() = app != nullptr;
        if (!app) return nullptr;
    }
    if (app->frameCount() == 0 && !app->waitForLabel("Play Online", 20000)) return nullptr;
    // A new session, set up as the game scene sets it up (GameScene::initOnline with --warp).
    freshSession();
    defaultOnlineSettings();
    game::onlineSession().init(true, true);
    app->frames(2);
    return app;
}

inline void releaseOnlineApp() {
    if (!appStarted()) return;
    headless::stopShared();
    freshSession();
    defaultOnlineSettings();
    appStarted() = false;
}

// Releases the App at the end of the scope: the last test of each online file holds one, so that
// the files after it find the process as it was (the session not set up, no App of ours alive).
struct ReleaseAtEnd {
    ReleaseAtEnd() = default;
    ReleaseAtEnd(const ReleaseAtEnd&) = delete;
    ReleaseAtEnd& operator=(const ReleaseAtEnd&) = delete;
    ~ReleaseAtEnd() { releaseOnlineApp(); }
};

// ---- Driving the UI ----------------------------------------------------------------------------
// Clicks the item of the last frame labelled 'label' that was drawn last: a confirmation's button
// rather than the menu entry of the same name under it. False when none shows.
inline bool clickLast(headless::App& app, const std::string& label) {
    const ui::im::TracedItem* last = nullptr;
    for (const auto& it : app.items())
        if (it.label == label) last = &it;
    if (!last) return false;
    app.clickItem(*last);
    return true;
}

// Waits for a page showing 'label', then for its entrance to end (a page takes no input while it
// slides in).
inline bool onPage(headless::App& app, const std::string& label, int frames = 60) {
    if (!app.waitForLabel(label, frames)) return false;
    app.frames(30);
    return app.hasLabel(label);
}

// Clicks the field labelled 'label' (in its box, at the right end of the row), empties it (the
// pages keep what was typed in them from one visit to the next: End, then one Backspace a frame,
// up to 'longest' characters) and types 'text' into it. The field keeps its focus until the next
// click elsewhere. False when no such field shows.
inline bool fill(headless::App& app, const std::string& label, const std::string& text, int longest = 64) {
    const ui::im::TracedItem* it = app.find(label);
    if (!it) return false;
    const ui::gfx::Rect r = it->r;
    const float x = ui::im::rtl() ? r.x + 40.0f : r.r() - 40.0f;
    app.click(x * ui::gfx::scale(), r.cy() * ui::gfx::scale());
    app.frame();
    app.key(plat::KEY_END);
    for (int i = 0; i < longest; ++i) {
        fakeplat::next().keyPressed[plat::KEY_BACKSPACE] = true;
        app.frame();
    }
    app.type(text);
    return true;
}

// Whether an item labelled 'label' shows and is live (not ITEM_DISABLED).
inline bool enabled(headless::App& app, const std::string& label) {
    const ui::im::TracedItem* it = app.find(label);
    return it && (it->flags & ui::im::ITEM_DISABLED) == 0;
}

// The items of the last frame drawn without a label (choice rows, tiles, rows of a list) whose
// height is 'h' reference pixels, from the top (then from the left).
inline std::vector<ui::im::TracedItem> bare(headless::App& app, float h) {
    std::vector<ui::im::TracedItem> out;
    for (const auto& it : app.items())
        if (it.label.empty() && it.r.h > h - 0.5f && it.r.h < h + 0.5f) out.push_back(it);
    std::sort(out.begin(), out.end(), [](const ui::im::TracedItem& a, const ui::im::TracedItem& b) {
        return a.r.y < b.r.y || (a.r.y == b.r.y && a.r.x < b.r.x);
    });
    return out;
}

// Opens the Esc menu of the game being played (Esc does nothing before the game is Playing).
inline bool pause(headless::App& app, int tries = 120) {
    for (int i = 0; i < tries; ++i) {
        if (app.hasLabel("Resume")) return true;
        app.key(plat::KEY_ESCAPE);
    }
    return app.hasLabel("Resume");
}

// From the title page: Play Online, the sign-in page once the server's description came (with it
// comes the Google button, which moves the links under it).
inline bool toSignInPage(headless::App& app) {
    game::OnlineSession& s = game::onlineSession();
    if (!app.clickLabel("Play Online") || !onPage(app, "Sign in")) return false;
    if (!app.runUntil([&] { return s.infoKnown(); }, 300)) return false;
    app.frames(2);
    return app.hasLabel("Sign in");
}

// From the title page: Play Online, then signs in as 'name' (the fakes take any password) and
// waits for the realtime connection. True once the Play page shows, connected.
inline bool signIn(headless::App& app, const std::string& name) {
    game::OnlineSession& s = game::onlineSession();
    if (!toSignInPage(app)) return false;
    if (!fill(app, "User name or e-mail", name) || !fill(app, "Password", "correct horse battery")) return false;
    if (!app.clickLabel("Sign in")) return false;
    return app.runUntil([&] { return s.signedIn() && s.conn() == net::ConnState::Online && app.hasLabel("Find opponent"); }, 300);
}

// Back to the title page from the online pages (Back until it shows).
inline bool toTitle(headless::App& app) {
    for (int i = 0; i < 8 && !app.hasLabel("Play Online"); ++i) {
        if (!app.clickLabel("Back")) app.frames(10);
        app.frames(20);
    }
    return app.hasLabel("Play Online");
}

// Runs the frames until the session's answer of kind 'k' has come (busy() no more).
inline bool answered(headless::App& app, Kind k, int frames = 300) {
    return app.runUntil([&] { return !game::onlineSession().busy(k); }, frames);
}

// The newest games of the account's history, fetched through the session (its first page).
inline std::vector<net::GameSummary> history(headless::App& app) {
    game::OnlineSession& s = game::onlineSession();
    s.loadHistory(net::GamesFilter());
    app.runUntil([&] { return s.accountData().history.loaded() && !s.accountData().history.waiting(); }, 300);
    return s.accountData().history.page().games;
}

}  // namespace onlinetest
