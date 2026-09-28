// User interface: SDF text rendering, immediate-mode widgets, and the game's screens (main menu,
// new game setup, options, pause, promotion picker, notifications, game over).
// Implemented by the UI work package. Everything is drawn on the backbuffer after the 3D frame,
// in display (sRGB) space, scaled with the window height (reference 1080p).
//
// Coordinates and sizes of the low-level calls are *reference pixels*: the canvas is always 1080
// units tall and 1080 * aspect units wide (see viewSize()); the UI converts to physical pixels.
//
// Per frame (after the 3D frame, before swapBuffers):
//     ui::beginFrame(w, h, dt);
//     ... screens / notifications / low-level drawing ...
//     ui::endFrame();
// wantsMouse()/wantsKeyboard() report the previous completed frame, so the game can test them in
// its update before drawing the UI.
#pragma once
#include "../math/math.h"
#include <functional>
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
// Regular = EB Garamond, Title = Cinzel display capitals, Italic = EB Garamond Italic.
enum class FontStyle { Regular, Title, Italic };
// pos is the left/centre/right point (per align) of the top of the line box; the line box is
// measure().y tall. tracking = extra letter spacing in em (Cinzel titles look best at ~0.15).
void text(const std::string& s, m::vec2 pos, float sizePx, m::vec4 color, Align align = Align::Left,
          FontStyle st = FontStyle::Regular, float tracking = 0.0f);
m::vec2 measure(const std::string& s, float sizePx, FontStyle st = FontStyle::Regular, float tracking = 0.0f);
void rect(m::vec2 pos, m::vec2 size, m::vec4 color, float radius = 0.0f);
void fullscreenTint(m::vec4 color);
m::vec2 viewSize();   // canvas size in reference pixels (height is always 1080)
float pixelScale();   // physical pixels per reference pixel (window height / 1080)
// The standard translucent panel (black velvet, gold double hairline) and a button in the same
// style, for game HUD elements that want to match the menus. button() returns true on click.
void panel(m::vec2 pos, m::vec2 size);
bool button(const std::string& label, m::vec2 pos, m::vec2 size, bool primary = false, bool enabled = true);

// ---- Data supplied by the game (keeps the UI independent of ai/ and chess/) --------------------
struct DifficultyInfo {
    std::string name;         // "Club Player"
    std::string description;  // one line
    int elo = 0;              // shown as "~1500 Elo"; 0 = not shown
};
// Mirrors ai::presets(): the last entry is "Custom" (it opens the engine parameters).
void setDifficultyList(const std::vector<DifficultyInfo>& list);
// Labels of chess::timeControlPresets() ("Unlimited", "1+0", "3+2", ...). The UI appends "Custom".
void setTimeControlList(const std::vector<std::string>& labels);
// Window sizes offered for windowed mode (defaults: common 16:9 sizes up to 3840x2160).
void setResolutionList(const std::vector<m::ivec2>& sizes);
void setVersionString(const std::string& version);  // bottom line of the main menu

// UI sound hooks (the game forwards them to audio::playUI).
enum class Sound { Hover, Click, Back, Toggle, Tick, Open, Close, Confirm };
void setSoundCallback(std::function<void(Sound)> callback);

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
    None, StartGame, Quit, Resume, Resign, OfferDraw, ClaimDraw, BackToMainMenu, OptionsChanged, Rematch,
    StartWatching  // "Watch a Game" page: Start (the WatchSetup holds the choice)
};

// "Watch a Game" (viewer mode): two Stockfish players. The page starts from the last choices saved
// in game::settings() ([viewer]) and writes them back (and saves the .ini) on Start.
struct WatchSetup {
    int whitePreset = 5, blackPreset = 4;  // indices into the difficulty list ("Custom" excluded)
    int timeControl = 5;                   // as NewGameSetup::timeControl (-1 = custom)
    int customBaseSeconds = 300, customIncrementSeconds = 3, customDelaySeconds = 0;
};

// Title screen over the 3D hall. Handles its sub-pages (New Game, Options, Credits) itself.
// The New Game page starts from the last choices saved in game::settings() and writes them back
// (and saves the .ini) when the player presses Start; 'setup' then holds the choice.
// OptionsChanged is returned on the frame the player applies new options (already stored in
// game::settings() and saved); the game re-applies display/graphics/audio settings.
// The title page also shows the player's Elo (game::settings() [player]) and the New Game page
// shows it next to the opponent list.
MenuAction mainMenu(NewGameSetup& setup);
// Same, with the "Watch a Game" entry filling 'watch' (returns StartWatching on its Start).
MenuAction mainMenu(NewGameSetup& setup, WatchSetup& watch);
// In-game pause menu (Esc). canClaimDraw enables the claim entry; canOfferDraw = false greys out
// "Offer draw" (e.g. an offer is already pending). Esc resumes.
MenuAction pauseMenu(bool canClaimDraw, bool canOfferDraw = true);
// Pawn promotion: returns 0 while choosing, else chess::PieceType (Queen, Rook, Bishop, Knight).
int promotionPicker(bool playerIsWhite);
// Transient message (arbiter, "Draw offer declined", ...), shown for 'seconds'.
void notify(const std::string& message, float seconds = 3.0f);
void drawNotifications();
// End of game card: result line ("1-0", "½-½"), reason, move count. Returns Rematch or BackToMainMenu.
// The card can be folded away by the player to look at the final position.
MenuAction gameOver(const std::string& result, const std::string& reason, bool playerWon, bool draw, int moveCount = -1);
// Additions to the end of game card.
struct GameOverExtras {
    std::string line;          // replaces the sentence under the reason (watched game: who won)
    std::string detail;        // extra line under it: Elo change ("Elo 1512 → 1524 (+12)"), players
    std::string primaryLabel;  // replaces "Rematch" (watched game: "Watch again")
};
MenuAction gameOver(const std::string& result, const std::string& reason, bool playerWon, bool draw, int moveCount,
                    const GameOverExtras& extras);
bool gameOverFolded();  // the card is folded away ("View the board")
// Options page is reachable from both menus; changes go to game::settings() when applied.
bool optionsOpen();
// Optional small move list (toggled by the player with Tab).
void moveList(const std::vector<std::string>& san, bool visible);
// Loading screen while shaders/probes/textures are prepared (progress 0..1).
void loadingScreen(float progress, const std::string& label);

// ---- Viewer mode (ui_screens_game.cpp) ---------------------------------------------------------
// Pause menu while watching: Resume, Options, Main menu (no draw offer, no resignation). Esc resumes.
MenuAction viewerPauseMenu();
// Overlay of the viewer mode: the players (top left), the controls hint (bottom left, the game
// toggles it with H) and the name of a viewpoint just selected.
struct ViewerHud {
    bool visible = true;         // everything (H)
    std::string white, black;    // "Stockfish · Expert · 2100"
    int sideToMove = 0;          // 0 White, 1 Black, -1 none (marks the player to move)
    std::string viewpoint;       // viewpoint just selected ("" = none)
    float viewpointAge = 0.0f;   // seconds since it was selected (the label fades out)
    float speed = 1.2f;          // observer speed (m/s), shown for a moment after a change
    float speedAge = 1e9f;       // seconds since the speed changed
};
void viewerHud(const ViewerHud& hud);

}  // namespace ui
