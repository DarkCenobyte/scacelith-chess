// Internal to src/ui: the game-mode pages (ui_screens_game.cpp: Watch a Game, the viewer's pause
// menu and overlay, the player's Elo) and the hooks they share with ui_screens.cpp.
#pragma once
#include "../i18n/i18n.h"
#include "ui.h"
#include <string>

namespace ui {
namespace detail {

// ---- Implemented in ui_screens_game.cpp, called from ui_screens.cpp ------------------------------
// "Watch a Game" page. t = page appearance (0..1), opened = first frame on the page. Sets 'back'
// when the player leaves the page; returns StartWatching on Start.
MenuAction watchPage(WatchSetup& setup, float t, bool opened, bool& back);
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

}  // namespace detail
}  // namespace ui
