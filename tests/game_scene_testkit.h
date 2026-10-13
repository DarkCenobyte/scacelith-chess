// Helpers of the game scene's tests (tests/game_scene_*_tests.cpp): the headless game (headless.h)
// played the way a player plays it. The pointer is aimed at a piece, a square or the clock by
// projecting the point through the camera of the last frame (render::Renderer::camera()), which is
// the player's eyes at the table; the game's log, written to a file of the test while it lives,
// says when a move was completed, a game ended or saved; the saved games are read back from the
// App's folder with archive::loadFile.
#pragma once
#include "test.h"
#include "fake_platform.h"
#include "headless.h"
#include "chess/chess.h"
#include "chess/pgn.h"
#include "core/ini.h"
#include "core/log.h"
#include "game/game_archive.h"
#include "game/layout.h"
#include "game/settings.h"
#include "platform/platform.h"
#include "render/renderer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace gstest {

// The arguments of the App most of these tests share (headless::shared): the title page, then
// every game started from the menus as a player starts it. --analysis-marks-test puts the review's
// fixed set of marks on the board while a game is played (World::submitAnalysisMarks).
inline std::vector<std::string> menuArgs() { return {"--no-intro", "--analysis-marks-test"}; }

// The game's log in a file of the test's, from construction to destruction.
class LogTail {
public:
    LogTail() : dir_("gslog"), path_(dir_.file("game.log")) { logx::init(path_.c_str()); }
    ~LogTail() { logx::shutdown(); }
    LogTail(const LogTail&) = delete;
    LogTail& operator=(const LogTail&) = delete;
    const std::string& text() {
        refresh();
        return text_;
    }
    size_t mark() { return text().size(); }
    bool has(const std::string& s, size_t from = 0) { return text().find(s, from) != std::string::npos; }
    int count(const std::string& s, size_t from = 0) {
        int n = 0;
        const std::string& t = text();
        for (size_t at = t.find(s, from); at != std::string::npos; at = t.find(s, at + s.size())) ++n;
        return n;
    }
    // The rest of the first line holding 's' after 'from' ("" when there is none).
    std::string lineAfter(const std::string& s, size_t from = 0) {
        const std::string& t = text();
        size_t at = t.find(s, from);
        if (at == std::string::npos) return std::string();
        at += s.size();
        size_t end = t.find('\n', at);
        return t.substr(at, end == std::string::npos ? std::string::npos : end - at);
    }

private:
    void refresh() {
        FILE* f = std::fopen(path_.c_str(), "rb");
        if (!f) return;
        if (std::fseek(f, long(text_.size()), SEEK_SET) == 0) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text_.append(buf, n);
        }
        std::fclose(f);
    }
    headless::TempDir dir_;
    std::string path_;
    std::string text_;
};

// game::settings() as the test found it, put back at its end (the shared App keeps running).
struct SettingsGuard {
    game::Settings saved = game::settings();
    ~SettingsGuard() { game::settings() = saved; }
};

// Frames until 'done', at most 'seconds' of wall-clock time and 'maxFrames' frames. Stockfish
// starts and searches in real time, beside the frames, which run much faster than the game's
// time: past the frames a move takes to be played, each frame waits a millisecond for it.
inline bool runFor(headless::App& app, const std::function<bool()>& done, double seconds, int maxFrames = 100000,
                   float dt = 1.0f / 60.0f) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    for (int i = 0; i < maxFrames; ++i) {
        if (done()) return true;
        if (std::chrono::steady_clock::now() > end) return false;
        if (!app.frame(dt)) return done();
        if (i >= 240) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}

// The SAN of the move the log reports as completed move n ("move 3: Nf3 (White, clocks ...)").
inline std::string loggedMove(LogTail& log, int n, size_t from = 0) {
    const std::string rest = log.lineAfter("] move " + std::to_string(n) + ": ", from);
    return rest.substr(0, rest.find(' '));
}

// The log says 's' (after 'from'), within the limits of runFor.
inline bool waitLog(headless::App& app, LogTail& log, const std::string& s, size_t from = 0, double seconds = 120.0,
                    int maxFrames = 20000) {
    return runFor(app, [&] { return log.has(s, from); }, seconds, maxFrames);
}

// ---- Where things are on screen ---------------------------------------------------------------------

// The pixel (window coordinates, origin top left) where the camera of the last frame sees p.
inline m::vec2 toScreen(headless::App& app, m::vec3 p) {
    const render::Camera& c = app.renderer().camera();
    const m::vec3 d = p - c.position;
    const float z = m::dot(d, c.forward()), x = m::dot(d, c.right()), y = m::dot(d, c.up());
    const float w = float(plat::width()), h = float(plat::height());
    const float t = std::tan(c.fovY * 0.5f);
    const float nx = x / (z * t * (w / h)), ny = y / (z * t);
    return m::vec2((nx + 1.0f) * 0.5f * w - 0.5f, (1.0f - ny) * 0.5f * h - 0.5f);
}

// The centre of a square ("e4") at 'height' above the board.
inline m::vec3 squarePoint(const std::string& sq, float height = 0.0f) {
    const int file = sq[0] - 'a', rank = sq[1] - '1';
    return m::vec3((float(file) - 3.5f) * layout::SQUARE_SIZE, layout::BOARD_TOP_Y + height,
                   (3.5f - float(rank)) * layout::SQUARE_SIZE);
}

// A point near the top of the piece of type 't' standing on 'sq': a seated player sees the head of
// a pawn above the pieces of the first rank, not its middle.
inline m::vec3 piecePoint(const std::string& sq, chess::PieceType t) {
    return squarePoint(sq, layout::PIECE_HEIGHT[t] * (t == chess::Pawn ? 0.9f : 0.8f));
}

// A point on top of the clock (it stands at +X, between the players).
inline m::vec3 clockPoint() { return m::vec3(layout::CLOCK_OFFSET_X, layout::TABLE_TOP_Y + layout::CLOCK_HEIGHT, layout::CLOCK_Z); }

// Points the mouse at p and waits for the view, which drifts a little towards the pointer, to
// settle (a few frames), so that the pointer is on p.
inline void pointAt(headless::App& app, m::vec3 p) {
    m::vec2 s = toScreen(app, p);
    for (int i = 0; i < 40; ++i) {
        app.moveMouse(s.x, s.y);
        const m::vec2 n = toScreen(app, p);
        if (m::length(n - s) < 0.25f) break;
        s = n;
    }
}

// A press and a release of a mouse button where the pointer is (two frames).
inline void clickHere(headless::App& app, int button = plat::MOUSE_LEFT) {
    plat::Input& in = fakeplat::next();
    in.mouseDown[button] = in.mousePressed[button] = true;
    app.frame();
    in.mouseDown[button] = false;
    in.mouseReleased[button] = true;
    app.frame();
}

inline void clickAt(headless::App& app, m::vec3 p, int button = plat::MOUSE_LEFT) {
    pointAt(app, p);
    clickHere(app, button);
}

// A move played by hand: the piece touched, then put down on its square ("g1f3"; a capture is
// aimed at the piece taken). The clock is not pressed.
inline void playMove(headless::App& app, const chess::Position& pos, const std::string& uci) {
    const std::string from = uci.substr(0, 2), to = uci.substr(2, 2);
    const chess::Piece mover = pos.at(chess::parseSquare(from));
    const chess::Piece victim = pos.at(chess::parseSquare(to));
    clickAt(app, piecePoint(from, mover.type));
    app.frames(6);
    clickAt(app, victim.empty() ? squarePoint(to) : piecePoint(to, victim.type));
}

// A key pressed and released with modifiers (two frames).
inline void pressKey(headless::App& app, int key, bool shift = false) { app.key(key, shift); }

// ---- Menus ------------------------------------------------------------------------------------------

// Clicks the last item of the last frame showing 'label' (a confirmation's button is drawn after
// the menu entry of the same name).
inline bool clickLast(headless::App& app, const std::string& label) {
    const ui::im::TracedItem* found = nullptr;
    for (const ui::im::TracedItem& it : app.items())
        if (it.label == label) found = &it;
    if (!found) return false;
    app.clickItem(*found);
    return true;
}

// Back to the title page from wherever the shared App is (a page of the menu, a game being played
// or over, a game watched): Esc, "Main menu" and the confirmation, as a player would.
inline bool toTitle(headless::App& app) {
    for (int i = 0; i < 40; ++i) {
        if (app.hasLabel("New Game") && app.hasLabel("Watch a Game")) return true;
        if (app.hasLabel("Leave")) clickLast(app, "Leave");
        else if (app.hasLabel("Main menu")) app.clickLabel("Main menu");
        else if (app.hasLabel("Cancel")) app.clickLabel("Cancel");
        else app.key(plat::KEY_ESCAPE);
        app.frames(60);
    }
    return app.waitForLabel("New Game", 600);
}

// The shared App on its title page with the engine settings of these tests: Stockfish answers at
// once (no humanised thinking time) at its weakest level (a one-ply search).
inline headless::App* titleApp() {
    headless::App* app = headless::shared(menuArgs());
    if (!app) return nullptr;
    if (!app->waitForLabel("New Game", 30000)) return nullptr;
    if (!toTitle(*app)) return nullptr;
    return app;
}

// ---- Saved games ------------------------------------------------------------------------------------

inline std::string pgnFolder(headless::App& app) { return app.folder().path + "pgn/"; }

inline std::set<std::string> pgnFiles(const std::string& folder) {
    std::set<std::string> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(std::filesystem::u8path(folder), ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".pgn") out.insert(it->path().u8string());
    return out;
}

// The games saved since 'before' (a listing of pgnFiles), read back.
inline std::vector<chess::pgn::Record> newGames(const std::string& folder, const std::set<std::string>& before) {
    std::vector<chess::pgn::Record> out;
    for (const std::string& f : pgnFiles(folder)) {
        if (before.count(f)) continue;
        game::archive::LoadResult r = game::archive::loadFile(f);
        if (r.ok) out.push_back(r.record);
    }
    return out;
}

inline std::vector<std::string> sanOf(const chess::pgn::Record& r) {
    std::vector<std::string> out;
    for (const chess::pgn::Ply& p : r.plies) out.push_back(p.san);
    return out;
}

// The settings file as saved now.
inline IniFile savedIni() {
    IniFile ini;
    ini.load(game::settings().path);
    return ini;
}

}  // namespace gstest
