// User interface: SDF text rendering, immediate-mode widgets, and the game's screens (main menu,
// new game setup, options, pause, promotion picker, notifications, game over).
// Implemented by the UI work package. Everything is drawn on the backbuffer after the 3D frame,
// in display (sRGB) space, scaled with the window height (reference 1080p).
#pragma once
#include "../math/math.h"
#include <string>
#include <vector>

namespace ui {

bool init();       // GL context required; builds the font atlas (embedded TTF)
void shutdown();
void beginFrame(int width, int height, float dt);  // reads plat::input()
void endFrame();   // flushes draw lists
// True when the UI consumed the mouse/keyboard this frame (the game should ignore clicks).
bool wantsMouse();
bool wantsKeyboard();

// ---- Low level ------------------------------------------------------------------------------
enum class Align { Left, Center, Right };
enum class FontStyle { Regular, Title };  // Title = large serif display face
void text(const std::string& s, m::vec2 pos, float sizePx, m::vec4 color, Align align = Align::Left, FontStyle st = FontStyle::Regular);
m::vec2 measure(const std::string& s, float sizePx, FontStyle st = FontStyle::Regular);
void rect(m::vec2 pos, m::vec2 size, m::vec4 color, float radius = 0.0f);
void fullscreenTint(m::vec4 color);

// ---- Screens ----------------------------------------------------------------------------------
// The game owns the data; screens edit it and return an action.
struct NewGameSetup {
    int difficulty = 3;           // index into ai::presets()
    int timeControl = 5;          // index into chess::timeControlPresets(), or -1 = custom
    int customBaseSeconds = 600, customIncrementSeconds = 5, customDelaySeconds = 0;
    // Custom engine parameters (only when the "Custom" preset is selected)
    int skillLevel = 10;
    bool limitElo = false;
    int elo = 1800, depth = 0, moveTimeMs = 0, nodes = 0;
};

enum class MenuAction {
    None, StartGame, Quit, Resume, Resign, OfferDraw, ClaimDraw, BackToMainMenu, OptionsChanged, Rematch
};

// Title screen over the 3D hall. Handles its sub-pages (New Game, Options, Credits) itself.
MenuAction mainMenu(NewGameSetup& setup);
// In-game pause menu (Esc). canClaimDraw enables the claim entry.
MenuAction pauseMenu(bool canClaimDraw);
// Pawn promotion: returns 0 while choosing, else chess::PieceType (Queen, Rook, Bishop, Knight).
int promotionPicker(bool playerIsWhite);
// Transient message (arbiter, "Draw offer declined", ...), shown for 'seconds'.
void notify(const std::string& message, float seconds = 3.0f);
void drawNotifications();
// End of game card: result line ("1-0", "½-½"), reason, move count. Returns Rematch or BackToMainMenu.
MenuAction gameOver(const std::string& result, const std::string& reason, bool playerWon, bool draw);
// Options page is reachable from both menus; changes go to game::settings() directly.
bool optionsOpen();
// Optional small move list (toggled by the player with Tab).
void moveList(const std::vector<std::string>& san, bool visible);
// Loading screen while shaders/probes/textures are prepared (progress 0..1).
void loadingScreen(float progress, const std::string& label);

}  // namespace ui
