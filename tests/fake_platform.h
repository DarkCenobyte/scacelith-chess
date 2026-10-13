// The platform layer of the unit tests (src/platform/platform.h without a window): plat::init
// loads the null OpenGL (null_gl.h), the window has the size the test gives it, time is a clock
// the test advances, and the input of each frame is what the test sets before pumpEvents().
// The paths are net::sys's, as in the game, or a folder of the test's (setDataFolder); the clipboard
// is a string of its own.
#pragma once
#include "platform/platform.h"
#include <string>
#include <vector>

namespace fakeplat {

// The input the next plat::pumpEvents() delivers. The per-frame fields (pressed, released, text,
// wheel, motion) are cleared once delivered; the held keys and buttons and the mouse position stay.
plat::Input& next();
void setSize(int width, int height);
void setFocus(bool focused);
void advance(double seconds);         // plat::time()
void requestClose();                  // the next pumpEvents() returns false
void setCommandLine(const std::vector<std::string>& args);
void setSystemLanguage(const std::string& tag);
// plat::userDataDirectory() and appDataDirectory() answer this folder (with a trailing separator)
// instead of net::sys's ("" = net::sys's again).
void setDataFolder(const std::string& folder);

// What the game did with the window and the system.
struct Record {
    bool initialised = false;
    plat::WindowDesc desc;            // of the last init()
    plat::DisplayMode mode = plat::DisplayMode::Windowed;
    bool vsync = false;
    bool cursorVisible = true;
    bool mouseCaptured = false;
    int swaps = 0;
    std::string clipboard;            // setClipboardText's text (and clipboardText()'s answer)
    bool clipboardSensitive = false;
    std::vector<std::string> opened;  // openInFileManager's paths
    std::vector<std::string> messages;   // messageBox title + ": " + text
};
Record& record();
// Back to a fresh window: no input, time 0, focused, 1280 x 720, the record cleared.
void reset();

}  // namespace fakeplat
