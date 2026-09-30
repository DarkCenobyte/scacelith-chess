// Shared state between the UI translation units (core, screens, viewer). Internal to src/ui.
#pragma once
#include "ui.h"
#include <string>
#include <vector>

namespace ui {
namespace detail {

struct Data {
    std::vector<DifficultyInfo> difficulties;
    std::vector<std::string> timeControls;
    std::vector<m::ivec2> resolutions;
    std::string version = "0.1.0";
};
Data& data();

// Called from ui::beginFrame / ui::endFrame.
void screensBeginFrame(float dt);
void screensEndFrame();
void screensReset();

}  // namespace detail

// Hooks used by the "ui" viewer scene to open a given state directly.
namespace debug {
enum class MenuPage { Title = 0, NewGame = 1, Options = 2, Credits = 3, Watch = 4, Online = 5, Calibration = 6 };
void openMenuPage(MenuPage page);   // next mainMenu() call starts on this page
// Next mainMenu() call starts on the online page's sub-page 'sub' (see debug::openOnlinePage).
void openOnlineMenu(const std::string& sub);
void setOptionsTab(int tab);        // 0 Display, 1 Graphics, 2 Audio, 3 Gameplay, 4 Player, 5 Online, 6 Controls
void openPauseConfirm(int which);   // 1 = resign, 2 = main menu (next pauseMenu() call)
void foldGameOver(bool folded);
}  // namespace debug

}  // namespace ui
