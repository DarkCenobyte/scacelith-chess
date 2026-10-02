// Scacelith entry point: window, renderer and scene loop.
//
// Command line:
//   --scene <name>        run a registered scene (default: game). --list-scenes prints them.
//   --shot <file.png>     render --frames N frames with a fixed 60 Hz step, save, exit.
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
#include "net/online_client.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
// Prefer the discrete GPU on hybrid laptops.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

static int runApp(std::vector<std::string> args) {
    AppContext ctx;
    ctx.args = args;
    std::string exeDir = plat::exeDirectory();
    // The log falls back to the user data dir like the settings when the exe dir is read-only.
    if (!logx::init((exeDir + "scacelith.log").c_str())) logx::init((plat::userDataDirectory() + "scacelith.log").c_str());
    LOGI("Scacelith 0.1.0 starting");

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
        plat::messageBox("Scacelith", "Could not initialise OpenGL 4.6. Please update your graphics driver.");
        return 1;
    }

    render::Renderer renderer;
    render::setRenderer(&renderer);
    render::RenderSettings rs = settings.renderSettings();
    if (!renderer.init(rs)) {
        plat::messageBox("Scacelith", "Renderer initialisation failed (see scacelith.log).");
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
        plat::messageBox("Scacelith", "Scene initialisation failed (see scacelith.log).");
        return 1;
    }

    double last = plat::time();
    int frame = 0;
    bool running = true;
    while (running) {
        if (!plat::pumpEvents()) break;
        double now = plat::time();
        float dt = ctx.screenshotMode ? 1.0f / 60.0f : float(std::min(now - last, 0.1));
        last = now;
        const plat::Input& in = plat::input();
        if (in.keyPressed[plat::KEY_F5]) shaders::reloadAll();
        if (plat::width() > 0 && plat::height() > 0) renderer.resize(plat::width(), plat::height());

        running = scene->update(ctx, dt);
        if (plat::width() > 0 && plat::height() > 0) {
            scene->render(ctx, dt);
            scene->renderOverlay(ctx, dt);
        }
        ++frame;
        bool f12 = in.keyPressed[plat::KEY_F12];
        if ((ctx.screenshotMode && frame >= shotFrames) || f12) {
            std::vector<uint8_t> px;
            int sw, sh;
            glFinish();
            renderer.readBackbuffer(px, sw, sh);
            std::string out = ctx.screenshotMode ? shotPath : plat::userDataDirectory() + "screenshot_" + std::to_string(frame) + ".png";
            if (image::writePNG(out, sw, sh, 3, px.data())) LOGI("saved %s (%dx%d)", out.c_str(), sw, sh);
            else LOGE("could not write %s", out.c_str());
            if (ctx.screenshotMode) running = false;
        }
        plat::swapBuffers();
    }
    scene->shutdown(ctx);
    scene.reset();
    renderer.shutdown();
    settings.save();
    plat::shutdown();
    logx::shutdown();
    return 0;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        char buf[4096];
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, buf, sizeof(buf), nullptr, nullptr);
        args.push_back(buf);
    }
    LocalFree(wargv);
    return runApp(args);
}
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
