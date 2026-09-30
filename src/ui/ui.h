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
// Mirrors ai::presets(): the last entry is "Custom" (it opens the engine parameters). Pass the
// English names and descriptions: the screens show them translated (presetName below).
void setDifficultyList(const std::vector<DifficultyInfo>& list);
// Translations of an ai::presets() entry, looked up by its English name ("Club Player" ->
// preset.club_player.name / .desc in assets/i18n); unknown names are returned unchanged.
std::string presetName(const std::string& englishName);
std::string presetDescription(const std::string& englishName, const std::string& englishDescription);
// A chess::TimeControl::label() for display ("Unlimited" translated, "3+2" unchanged).
std::string timeControlLabel(const std::string& label);
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
    int opponent = 0;             // 0 Stockfish, 1 a second human on this PC (hot-seat)
    int difficulty = 3;           // index into ai::presets()
    int timeControl = 5;          // index into chess::timeControlPresets(), or -1 = custom
    int customBaseSeconds = 600, customIncrementSeconds = 5, customDelaySeconds = 0;
    // Custom engine parameters (only when the "Custom" preset is selected)
    int skillLevel = 10;
    bool limitElo = false;
    int elo = 1800, depth = 0, moveTimeMs = 0, nodes = 0;
    // Hot-seat (opponent 1), by colour (0 White, 1 Black): the players' names as written on the
    // scoresheets (never empty once the page has run), their handwriting (font::HandStyle), the
    // colour at whose right the clock stands (the other player plays left-handed), rated or friendly.
    std::string names[2];
    int hands[2] = {0, 1};
    int clockRightOf = 0;
    bool rated = false;
};

enum class MenuAction {
    None, StartGame, Quit, Resume, Resign, OfferDraw, ClaimDraw, BackToMainMenu, OptionsChanged, Rematch,
    StartWatching,  // "Watch a Game" page: Start (the WatchSetup holds the choice)
    Abort,          // online: abort the game (before your first move)
    Report          // online: report the opponent (Esc menu, game over card)
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
// Same, for a hot-seat game: 'resignQuestion' replaces the text of the resignation confirmation
// (it names the player to move).
MenuAction pauseMenu(bool canClaimDraw, bool canOfferDraw, const std::string& resignQuestion);
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
    bool primaryDisabled = false;  // online: rematch requested / declined
    std::string reportLabel;   // online: quiet "Report opponent" button (returns Report), "" = none
};
MenuAction gameOver(const std::string& result, const std::string& reason, bool playerWon, bool draw, int moveCount,
                    const GameOverExtras& extras);
bool gameOverFolded();  // the card is folded away ("View the board")
// Options page is reachable from both menus; changes go to game::settings() when applied.
bool optionsOpen();
// Brightness calibration (first start, --calibrate): the next mainMenu() call opens on it instead
// of the title page. Three squares (black, mid grey, white, each with a black knight) show what
// the 3D frame would show at the brightness of the slider. Continue stores the brightness in
// game::settings(), Esc keeps the stored one; both save the .ini and go on to the title page.
void openBrightnessCalibration();
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

// ---- Hot-seat: two players on one PC (ui_hotseat.cpp) ------------------------------------------
// Overlay of a hot-seat game: the two players (top left, the one to move marked), a caption naming
// the player whose turn begins (bottom centre, while the view goes over to them and a moment
// after), and the draw offer card of the player to move (Accept / Decline, mouse only: Space
// belongs to the clock).
struct HotSeatHud {
    std::string names[2];        // White, Black
    std::string ratings[2];      // local ratings of a rated game ("1512"), "" = none
    int toMove = -1;             // 0 White, 1 Black, -1 none (game over)
    std::string caption;         // "Bob, your move" ("" = none)
    float captionAge = 1e9f;     // seconds since the caption appeared (it fades out)
    bool drawOffer = false;      // the player to move is offered a draw
    std::string drawOfferText;   // "Alice offers a draw."
};
enum class HotSeatAction { None, AcceptDraw, DeclineDraw };
HotSeatAction hotSeatHud(const HotSeatHud& hud);

// ---- In-game pointer (ui_screens_game.cpp) -------------------------------------------------------
// Drawn by the game during first-person play in place of the system arrow (hidden meanwhile), on
// top of everything, at 'pixelPos' (physical pixels, as plat::Input). Its shape says what a click
// would do there.
enum class GameCursor {
    Idle,      // nothing to click here
    Waiting,   // the opponent's turn (dimmed)
    Piece,     // over one of your pieces that can be touched
    Holding,   // a piece in hand, the pointer off the board
    Square,    // a piece in hand, over a square it can go to (a legal one when hints are shown)
    Clock      // the clock can be pressed
};
void gameCursor(m::vec2 pixelPos, GameCursor kind);

}  // namespace ui
