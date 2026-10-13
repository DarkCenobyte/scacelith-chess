// The game without a window or a GPU, for the unit tests: main.cpp's loop on the fake platform
// layer (fake_platform.h) and the null OpenGL (null_gl.h). A test starts a scene ("game" by
// default) with command-line options, runs frames, and plays the mouse and the keyboard on it:
//
//   headless::App app;
//   REQUIRE(app.start({"--no-intro"}));
//   REQUIRE(app.clickLabel("New Game"));      // a widget drawn with this text in the last frame
//   app.frames(10);
//
// The settings file, the saved games and every other file of the run live in a temporary folder
// of the App (plat::userDataDirectory() and appDataDirectory() answer it), removed at the end.
// Positions are in physical pixels (the window is 1280 x 720 unless given); the UI's reference
// pixels are converted by clickItem / clickLabel.
#pragma once
#include "app/scene.h"
#include "ui/ui_widgets.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace headless {

// A fresh folder beside the test executable, removed with everything in it by the destructor.
struct TempDir {
    std::string path;              // with a trailing '/'
    explicit TempDir(const std::string& tag);
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    std::string file(const std::string& name) const { return path + name; }
};

class App {
public:
    explicit App(int width = 1280, int height = 720);
    ~App();   // stop()
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // As main.cpp: the settings (from the App's folder unless args give --ini), the renderer, then
    // the scene of --scene (default "game") and its init(). False when one of them fails.
    bool start(const std::vector<std::string>& args = {});
    // One turn of main.cpp's loop: events, update, render, the overlay, swap. False once the scene
    // asked to quit (or the window was closed).
    bool frame(float dt = 1.0f / 60.0f);
    void frames(int n, float dt = 1.0f / 60.0f);
    // Frames until 'done' answers true, at most maxFrames. Whether it did.
    bool runUntil(const std::function<bool()>& done, int maxFrames = 600, float dt = 1.0f / 60.0f);
    // The scene's shutdown, the renderer's, the settings saved. Idempotent.
    void stop();

    // ---- Input (one or more frames each) -----------------------------------------------------
    void moveMouse(float x, float y);                    // a frame with the pointer there
    void click(float x, float y, int button = 0);        // move, press, release: three frames
    void key(int key, bool shift = false, bool ctrl = false);   // down then up: two frames
    void type(const std::string& utf8);                  // the text in one frame
    void wheel(float notches);                           // one frame

    // ---- The UI ----------------------------------------------------------------------------
    // The items of the last frame (ui::im::setTrace is on while an App runs).
    const std::vector<ui::im::TracedItem>& items() const;
    // The first item of the last frame whose label is 'label' (nullptr: none).
    const ui::im::TracedItem* find(const std::string& label) const;
    bool hasLabel(const std::string& label) const { return find(label) != nullptr; }
    // Clicks the centre of an item (reference pixels -> physical), then runs one more frame so the
    // UI acts on it. clickLabel: false when no item of the last frame shows that label.
    void clickItem(const ui::im::TracedItem& it);
    bool clickLabel(const std::string& label);
    // Runs frames until an item shows 'label' (false after maxFrames).
    bool waitForLabel(const std::string& label, int maxFrames = 600);
    std::vector<std::string> labels() const;   // the labels of the last frame, for failure messages

    Scene* scene() { return scene_.get(); }
    AppContext& context() { return ctx_; }
    render::Renderer& renderer();
    const TempDir& folder() const { return *dir_; }
    int frameCount() const { return frame_; }

private:
    std::unique_ptr<TempDir> dir_;
    std::unique_ptr<render::Renderer> renderer_;
    std::unique_ptr<Scene> scene_;
    AppContext ctx_;
    bool running_ = false;
    int frame_ = 0;
    int width_, height_;
};

// One App for the tests of a file that can share it (starting one builds the font atlas and the
// world: seconds under gcov): started with 'args' on the first call, and started again only when a
// later call passes other args. Stopped by stopShared(), which the test runner calls before the
// first test of another file (the App's scene holds the engine and other one-owner systems) and
// after the last test (before the game's statics go). nullptr when it does not start.
App* shared(const std::vector<std::string>& args = {});
void stopShared();

}  // namespace headless
