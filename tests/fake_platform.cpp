// The fake platform layer of the unit tests (fake_platform.h).
#include "fake_platform.h"
#include "null_gl.h"
#include "core/log.h"
#include "gl/gl_context.h"
#include "net/net_sys.h"

namespace fakeplat {
namespace {
struct State {
    plat::Input current, pending;
    int width = 1280, height = 720;
    bool focus = true;
    double time = 0.0;
    bool closing = false;
    std::vector<std::string> args;
    std::string language = "en-US";
    std::string dataFolder;
    Record record;
};
State& st() {
    static State s;
    return s;
}
}  // namespace

plat::Input& next() { return st().pending; }
void setSize(int width, int height) {
    st().width = width;
    st().height = height;
}
void setFocus(bool focused) { st().focus = focused; }
void advance(double seconds) { st().time += seconds; }
void requestClose() { st().closing = true; }
void setCommandLine(const std::vector<std::string>& args) { st().args = args; }
void setSystemLanguage(const std::string& tag) { st().language = tag; }
void setDataFolder(const std::string& folder) {
    st().dataFolder = folder;
    if (!folder.empty() && folder.back() != '/' && folder.back() != '\\') st().dataFolder += '/';
}
Record& record() { return st().record; }
void reset() {
    State& s = st();
    s.current = plat::Input();
    s.pending = plat::Input();
    s.width = 1280;
    s.height = 720;
    s.focus = true;
    s.time = 0.0;
    s.closing = false;
    s.args.clear();
    s.language = "en-US";
    s.dataFolder.clear();
    s.record = Record();
}

}  // namespace fakeplat

namespace plat {
using fakeplat::st;

bool init(const WindowDesc& desc) {
    if (nullgl::load() != 0) {
        LOGE("null GL: entry points missing");
        return false;
    }
    gl46::afterContextCreated(desc.debugContext);
    auto& r = st().record;
    r.initialised = true;
    r.desc = desc;
    r.mode = desc.mode;
    r.vsync = desc.vsync;
    st().width = desc.width;
    st().height = desc.height;
    return true;
}
void shutdown() { st().record.initialised = false; }

bool pumpEvents() {
    // Deliver the pending input, then clear its per-frame part for the frame after.
    st().current = st().pending;
    Input& p = st().pending;
    for (int k = 0; k < KEY_COUNT; ++k) p.keyPressed[k] = p.keyReleased[k] = false;
    for (int b = 0; b < MOUSE_BUTTON_COUNT; ++b) p.mousePressed[b] = p.mouseReleased[b] = false;
    p.mouseDX = p.mouseDY = 0.0f;
    p.wheel = 0.0f;
    p.textCount = 0;
    return !st().closing;
}
void swapBuffers() { ++st().record.swaps; }
void setVsync(bool on) { st().record.vsync = on; }
void setDisplayMode(DisplayMode mode, int w, int h) {
    st().record.mode = mode;
    st().width = w;
    st().height = h;
}
int width() { return st().width; }
int height() { return st().height; }
bool hasFocus() { return st().focus; }
double time() { return st().time; }
void sleepMs(int) {}

const Input& input() { return st().current; }
void setCursorVisible(bool visible) { st().record.cursorVisible = visible; }
void setMouseCaptured(bool captured) { st().record.mouseCaptured = captured; }

std::string exeDirectory() { return net::sys::exeDirectory(); }
std::string userDataDirectory() { return st().dataFolder.empty() ? net::sys::userDataDirectory() : st().dataFolder; }
std::string appDataDirectory() { return st().dataFolder.empty() ? net::sys::appDataDirectory() : st().dataFolder; }
void messageBox(const char* title, const char* text, bool) {
    st().record.messages.push_back(std::string(title) + ": " + text);
}
uint64_t randomSeed() { return 0x5ca1ab1eu; }
std::string clipboardText() { return st().record.clipboard; }
#ifdef _WIN32
bool openClipboard(void*) { return false; }   // ui_clipboard.cpp then reports the copy failed
#else
bool setClipboardText(const std::string& utf8, bool sensitive) {
    st().record.clipboard = utf8;
    st().record.clipboardSensitive = sensitive;
    return true;
}
#endif
std::string systemLanguage() { return st().language; }
std::vector<std::string> commandLine() { return st().args; }
bool openInFileManager(const std::string& path) {
    st().record.opened.push_back(path);
    return true;
}

}  // namespace plat
