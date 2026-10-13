// The headless game of the unit tests (headless.h).
#include "headless.h"
#include "fake_platform.h"
#include "null_gl.h"
#include "core/embedded.h"
#include "core/log.h"
#include "character/robot.h"
#include "game/graphics_quality.h"
#include "game/settings.h"
#include "gl/gl_context.h"
#include "net/net_sys.h"
#include "scene/hall.h"
#include "scene/pieces.h"
#include "ui/ui_draw.h"
#include "ui/ui_font.h"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace headless {

TempDir::TempDir(const std::string& tag) {
    static std::atomic<int> serial{0};
#ifdef _WIN32
    const unsigned pid = unsigned(GetCurrentProcessId());
#else
    const unsigned pid = unsigned(getpid());
#endif
    path = net::sys::exeDirectory() + "headless-" + tag + "-" + std::to_string(pid) + "-" + std::to_string(serial++);
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(path), ec);
    std::filesystem::create_directories(std::filesystem::u8path(path), ec);
    path += '/';
}

TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(path), ec);
}

App::App(int width, int height) : dir_(new TempDir("app")), width_(width), height_(height) {}

App::~App() { stop(); }

bool App::start(const std::vector<std::string>& args) {
    stop();
    // One robot, one hall, one chess set and one set of glyphs for every scene of the run: each
    // takes seconds of CPU to build (a minute for the robot under gcov), the same each time.
    character::keepBuiltRobot(true);
    hall::keepBuiltHall(true);
    keepBuiltPieces(true);
    ui::font::keepBuiltGlyphs(true);
    fakeplat::reset();
    fakeplat::setSize(width_, height_);
    fakeplat::setDataFolder(dir_->path);
    fakeplat::setCommandLine(args);
    nullgl::resetStats();
    ctx_ = AppContext();
    ctx_.args = args;
    frame_ = 0;

    // The settings: a first start's, but with the brightness calibrated (the title page comes
    // first), unless the test gives its own file.
    game::Settings& settings = game::settings();
    settings = game::Settings();
    std::string ini = ctx_.argValue("--ini");
    if (ini.empty()) {
        ini = dir_->file("Scacelith.ini");
        if (FILE* f = std::fopen(ini.c_str(), "wb")) {
            std::fputs("[graphics]\nbrightness_calibrated = true\n", f);
            std::fclose(f);
        }
    }
    settings.load(ini);
    ctx_.screenshotMode = ctx_.hasArg("--shot");
    settings.readOnly = ctx_.screenshotMode;
    const std::string t = ctx_.argValue("--time");
    if (!t.empty()) ctx_.fixedTime = float(std::atof(t.c_str()));

    plat::WindowDesc wd;
    wd.width = width_;
    wd.height = height_;
    wd.vsync = false;
    wd.hidden = true;
    if (!plat::init(wd)) return false;
    game::setGraphicsOptionAvailable(game::GfxTessellation, gl46::caps().tessellation);
    renderer_.reset(new render::Renderer());
    render::setRenderer(renderer_.get());
    if (!renderer_->init(settings.renderSettings())) {
        LOGE("headless: renderer initialisation failed");
        renderer_.reset();
        render::setRenderer(nullptr);
        return false;
    }
    renderer_->resize(plat::width(), plat::height());
    ctx_.renderer = renderer_.get();
    scene_ = createScene(ctx_.argValue("--scene", "game"));
    if (!scene_ || !scene_->init(ctx_)) {
        LOGE("headless: scene initialisation failed");
        scene_.reset();
        stop();
        return false;
    }
    ui::im::setTrace(true);
    running_ = true;
    return true;
}

bool App::frame(float dt) {
    if (!scene_) return false;
    fakeplat::advance(dt);
    if (!plat::pumpEvents()) {
        running_ = false;
        return false;
    }
    ctx_.clockDt = dt;
    if (plat::width() > 0 && plat::height() > 0) renderer_->resize(plat::width(), plat::height());
    running_ = scene_->update(ctx_, dt);
    if (plat::width() > 0 && plat::height() > 0) {
        scene_->render(ctx_, dt);
        scene_->renderOverlay(ctx_, dt);
        plat::swapBuffers();
    }
    ++frame_;
    return running_;
}

void App::frames(int n, float dt) {
    for (int i = 0; i < n && frame(dt); ++i) {}
}

bool App::runUntil(const std::function<bool()>& done, int maxFrames, float dt) {
    for (int i = 0; i < maxFrames; ++i) {
        if (done()) return true;
        if (!frame(dt)) return done();
    }
    return done();
}

void App::stop() {
    if (scene_) {
        scene_->shutdown(ctx_);
        scene_.reset();
    }
    if (renderer_) {
        renderer_->shutdown();
        renderer_.reset();
        render::setRenderer(nullptr);
    }
    if (fakeplat::record().initialised) {
        game::settings().save();
        plat::shutdown();
    }
    ui::im::setTrace(false);
    running_ = false;
}

void App::moveMouse(float x, float y) {
    plat::Input& in = fakeplat::next();
    in.mouseDX = x - in.mouseX;
    in.mouseDY = y - in.mouseY;
    in.mouseX = x;
    in.mouseY = y;
    in.mouseInWindow = true;
    frame();
}

void App::click(float x, float y, int button) {
    moveMouse(x, y);
    plat::Input& in = fakeplat::next();
    in.mouseDown[button] = true;
    in.mousePressed[button] = true;
    frame();
    in.mouseDown[button] = false;
    in.mouseReleased[button] = true;
    frame();
}

void App::key(int k, bool shift, bool ctrl) {
    plat::Input& in = fakeplat::next();
    in.keyDown[plat::KEY_LSHIFT] = shift;
    in.keyDown[plat::KEY_LCTRL] = ctrl;
    in.keyDown[k] = true;
    in.keyPressed[k] = true;
    frame();
    in.keyDown[k] = false;
    in.keyReleased[k] = true;
    in.keyDown[plat::KEY_LSHIFT] = in.keyDown[plat::KEY_LCTRL] = false;
    frame();
}

void App::type(const std::string& utf8) {
    plat::Input& in = fakeplat::next();
    in.textCount = 0;
    for (size_t i = 0; i < utf8.size() && in.textCount < 64;) {
        // UTF-8 to UTF-32 (the tests type valid text).
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < n && i + size_t(k) < utf8.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + size_t(k)]) & 0x3F);
        in.text[in.textCount++] = cp;
        i += size_t(n);
    }
    frame();
}

void App::wheel(float notches) {
    fakeplat::next().wheel = notches;
    frame();
}

const std::vector<ui::im::TracedItem>& App::items() const { return ui::im::tracedItems(); }

const ui::im::TracedItem* App::find(const std::string& label) const {
    for (const auto& it : items())
        if (it.label == label) return &it;
    return nullptr;
}

void App::clickItem(const ui::im::TracedItem& it) {
    const float s = ui::gfx::scale();
    const ui::gfx::Rect r = it.r;   // a copy: the next frame replaces the list
    click(r.cx() * s, r.cy() * s);
    frame();
}

bool App::clickLabel(const std::string& label) {
    const ui::im::TracedItem* it = find(label);
    if (!it) return false;
    clickItem(*it);
    return true;
}

bool App::waitForLabel(const std::string& label, int maxFrames) {
    return runUntil([&] { return hasLabel(label); }, maxFrames);
}

std::vector<std::string> App::labels() const {
    std::vector<std::string> out;
    for (const auto& it : items())
        if (!it.label.empty()) out.push_back(it.label);
    return out;
}

render::Renderer& App::renderer() { return *renderer_; }

namespace {
std::unique_ptr<App> g_shared;
std::vector<std::string> g_sharedArgs;
}  // namespace

App* shared(const std::vector<std::string>& args) {
    if (g_shared && g_sharedArgs == args && g_shared->scene()) return g_shared.get();
    if (!g_shared) g_shared.reset(new App());
    g_sharedArgs = args;
    if (!g_shared->start(args)) return nullptr;
    return g_shared.get();
}

void stopShared() {
    if (g_shared) g_shared->stop();
    g_shared.reset();
}

}  // namespace headless
