// Internal to src/ui: the game-mode pages (ui_screens_game.cpp: Watch a Game, the viewer's pause
// menu and overlay, the player's Elo; ui_coach.cpp: the coach page; ui_library.cpp: the saved
// games) and the hooks they share with ui_screens.cpp.
#pragma once
#include "../i18n/i18n.h"
#include "ui.h"
#include <string>
#include <vector>

namespace ui {
namespace detail {

// ---- Implemented in ui_screens_game.cpp, called from ui_screens.cpp ------------------------------
// "Watch a Game" page. t = page appearance (0..1), opened = first frame on the page. Sets 'back'
// when the player leaves the page; returns StartWatching on Start.
MenuAction watchPage(WatchSetup& setup, float t, bool opened, bool& back);
// Coach page (ui_coach.cpp), same contract as watchPage: returns StartCoach on Start.
MenuAction coachPage(CoachSetup& setup, float t, bool opened, bool& back);
// "Saved games" page (ui_library.cpp), same contract as watchPage: returns StartReplay on Replay
// (setup.replay names the game).
MenuAction libraryPage(LibrarySetup& setup, float t, bool opened, bool& back);
// Stops the library's listing worker and waits for it (ui::shutdown, before the program exits).
void libraryShutdown();
// The player's Elo under the title menu, from x (the start edge: left, or right in a right-to-left
// UI) on the first baseline y.
void titleRating(float x, float y);
// The player's Elo at the end of the OPPONENT heading of the column [x, x + width] (baseline y),
// with the expected score against the selected opponent (difficulty index).
void newGameRating(float x, float width, float y, int difficulty);
// Extra line of the game over card (centred at cx, baseline y).
void gameOverDetail(const std::string& text, float cx, float y);

// ---- Implemented in ui_screens.cpp, used by ui_screens_game.cpp ----------------------------------
bool runOptionsPage(MenuAction& act);  // the options page; true once it is closed
void openOptionsPage();
void dimBackground(float a);           // full-screen dim behind menu panels
// The custom time control's steppers (New Game, Watch a Game, challenges, direct match): the base
// times offered (15 s to 3 h), the index of the value nearest to 'value', a base time as "m:ss".
const std::vector<int>& baseTimeValues();
int nearestIndex(const std::vector<int>& v, int value);
std::string clockText(int seconds);
std::string spacedPlus(const std::string& label);  // "3+2" -> "3 + 2" with thin spaces

}  // namespace detail
}  // namespace ui
