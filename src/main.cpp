// Scacelith entry point: window, renderer and scene loop.
//
// Command line:
//   --scene <name>        run a registered scene (default: game). --list-scenes prints them.
//   --shot <file.png>     render --frames N frames with a fixed 60 Hz step, save, exit (the
//                         settings file and the saved games are left as they are).
//   --frames <N>          frame count for --shot (default 30).
//   --size <WxH>          window/backbuffer size (default from the .ini).
//   --time <seconds>      start time for deterministic scenes.
//   --data-dir <path>     read shaders/assets from disk (live edit, F5 reloads shaders).
//   --debug-gl            KHR_debug context + synchronous error logging.
//   --ini <path>          settings file (default: Scacelith.ini next to the exe).
//   --online-mock         online play and direct match against in-process fakes (no network).
//   --start-online [cat]  game scene: skip the menu, sign in and play the first opponent found
//                         in category "cat" (default 5+3). See src/game/game_scene.h.
//   --start --hotseat     game scene: skip the menu, two players on one PC (--white-name,
//                         --black-name, --clock-right, --rated, --handover, --play: see
//                         src/game/game_scene.h).
#include "app/scene.h"
#include "core/embedded.h"
#include "core/image.h"
#include "core/ini.h"
#include "core/log.h"
#include "gl/gl46.h"
#include "platform/platform.h"
#include "render/renderer.h"
#include "render/shader.h"
#include "game/settings.h"
#include "i18n/i18n.h"
#include "net/online_client.h"
#include "scacelith_version.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
// Prefer the discrete GPU on hybrid laptops.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

// Local time to the millisecond, "20261002-134501-123": F12 screenshots are named by it, so a
// later session never overwrites an earlier one.
static std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm = {};
    if (const std::tm* local = std::localtime(&t)) tm = *local;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d%02d%02d-%02d%02d%02d-%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec, ms);
    return buf;
}

// The one message box of a failure before the game can show anything, in the player's language
// (Settings::load has set it), naming the log file that holds the details.
static void startupError(const char* key, const std::string& logPath) {
    plat::messageBox("Scacelith", i18n::trf(key, {i18n::ltr(logPath)}).c_str());
}

static int runApp(std::vector<std::string> args) {
    AppContext ctx;
    ctx.args = args;
    std::string exeDir = plat::exeDirectory();
    // The log falls back to the user data dir like the settings when the exe dir is read-only.
    std::string logPath = exeDir + "scacelith.log";
    if (!logx::init(logPath.c_str())) {
        logPath = plat::userDataDirectory() + "scacelith.log";
        logx::init(logPath.c_str());
    }
    LOGI("Scacelith " SCACELITH_VERSION " starting");

    if (ctx.hasArg("--list-scenes")) {
        for (auto& s : listScenes()) std::printf("%-20s %s\n", s.first.c_str(), s.second.c_str());
        return 0;
    }
    std::string dataDir = ctx.argValue("--data-dir");
    if (!dataDir.empty()) embedded::setOverrideDir(dataDir);

    // Settings (.ini). Falls back to the user data dir when the exe dir is read-only.
    std::string iniPath = ctx.argValue("--ini", exeDir + "Scacelith.ini");
    game::Settings& settings = game::settings();
    settings.load(iniPath);
    // Online logins (one per server, DPAPI-protected) live next to an explicit --ini file;
    // otherwise next to the exe, or in the user data dir (net::OnlineClient's default).
    if (!ctx.argValue("--ini").empty()) {
        size_t slash = iniPath.find_last_of("/\\");
        net::onlineClient().setCredentialsFile((slash == std::string::npos ? std::string() : iniPath.substr(0, slash + 1)) + "Scacelith.credentials");
    }

    std::string shotPath = ctx.argValue("--shot");
    int shotFrames = std::atoi(ctx.argValue("--frames", "30").c_str());
    ctx.screenshotMode = !shotPath.empty();
    // Screenshot runs leave the player's settings (Elo, next colour...) and saved games alone.
    settings.readOnly = ctx.screenshotMode;
    std::string size = ctx.argValue("--size");
    int w = settings.displayWidth, h = settings.displayHeight;
    if (!size.empty()) std::sscanf(size.c_str(), "%dx%d", &w, &h);
    std::string t = ctx.argValue("--time");
    if (!t.empty()) ctx.fixedTime = float(std::atof(t.c_str()));

    plat::WindowDesc wd;
    wd.title = "Scacelith";
    wd.width = w;
    wd.height = h;
    wd.mode = (ctx.screenshotMode || !size.empty()) ? plat::DisplayMode::Windowed
                                                     : (settings.fullscreen ? plat::DisplayMode::Borderless : plat::DisplayMode::Windowed);
    wd.vsync = ctx.screenshotMode ? false : settings.vsync;
    wd.debugContext = ctx.hasArg("--debug-gl");
    if (!plat::init(wd)) {
        LOGE("could not initialise OpenGL 4.6");  // plat::init has logged the reason
        startupError("error.opengl", logPath);
        return 1;
    }

    render::Renderer renderer;
    render::setRenderer(&renderer);
    render::RenderSettings rs = settings.renderSettings();
    if (!renderer.init(rs)) {
        LOGE("renderer initialisation failed");
        startupError("error.renderer", logPath);
        return 1;
    }
    renderer.resize(plat::width(), plat::height());
    ctx.renderer = &renderer;

    std::string sceneName = ctx.argValue("--scene", "game");
    std::unique_ptr<Scene> scene = createScene(sceneName);
    if (!scene) {
        LOGW("scene '%s' not found, falling back to 'testbed'", sceneName.c_str());
        scene = createScene("testbed");
    }
    if (!scene || !scene->init(ctx)) {
        LOGE("scene initialisation failed");
        startupError("error.scene", logPath);
        return 1;
    }

    double last = plat::time();
    int frame = 0;
    bool running = true;
    bool f5Held = false, f12Held = false;  // F5 and F12 act once per press, not on auto-repeat
    bool shotFailed = false;
    while (running) {
        if (!plat::pumpEvents()) break;
        double now = plat::time();
        float dt = ctx.screenshotMode ? 1.0f / 60.0f : float(std::min(now - last, 0.1));
        ctx.clockDt = ctx.screenshotMode ? dt : float(std::min(now - last, 2.0));
        last = now;
        const plat::Input& in = plat::input();
        // Shader sources only change on disk (--data-dir); the embedded ones would rebuild as they are.
        if (!dataDir.empty() && in.keyPressed[plat::KEY_F5] && !f5Held) shaders::reloadAll();
        f5Held = in.keyDown[plat::KEY_F5];
        if (plat::width() > 0 && plat::height() > 0) renderer.resize(plat::width(), plat::height());

        running = scene->update(ctx, dt);
        bool visible = plat::width() > 0 && plat::height() > 0;  // 0 x 0 while minimised
        if (visible) {
            scene->render(ctx, dt);
            scene->renderOverlay(ctx, dt);
        }
        ++frame;
        bool f12 = in.keyPressed[plat::KEY_F12] && !f12Held && visible;
        f12Held = in.keyDown[plat::KEY_F12];
        if ((ctx.screenshotMode && frame >= shotFrames) || f12) {
            std::vector<uint8_t> px;
            int sw, sh;
            glFinish();
            renderer.readBackbuffer(px, sw, sh);
            std::string out = ctx.screenshotMode ? shotPath : plat::userDataDirectory() + "screenshot_" + timestamp() + ".png";
            if (image::writePNG(out, sw, sh, 3, px.data())) {
                LOGI("saved %s (%dx%d)", out.c_str(), sw, sh);
            } else {
                LOGE("could not write %s", out.c_str());
                if (ctx.screenshotMode) shotFailed = true;
            }
            if (ctx.screenshotMode) running = false;
        }
        if (visible) plat::swapBuffers();
        else plat::sleepMs(10);  // nothing to present, and no vsync to pace the loop
    }
    scene->shutdown(ctx);
    scene.reset();
    renderer.shutdown();
    settings.save();
    plat::shutdown();
    logx::shutdown();
    return shotFailed ? 2 : 0;  // a --shot run without its image fails (tools/shot.sh)
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return runApp(plat::commandLine()); }
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    return WinMain(GetModuleHandle(nullptr), nullptr, nullptr, SW_SHOWDEFAULT);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    return runApp(args);
}
#endif
