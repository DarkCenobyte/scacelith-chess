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
    Report,         // online: report the opponent (Esc menu, game over card)
    StartCoach,     // Coach page: Start (the CoachSetup holds the choice)
    TakeBack,       // coach game, Esc menu: take back the player's last move
    StartReplay     // "Saved games" page: Replay (LibrarySetup::replay holds the game)
};

// "Watch a Game" (viewer mode): two Stockfish players. The page starts from the last choices saved
// in game::settings() ([viewer]) and writes them back (and saves the .ini) on Start.
struct WatchSetup {
    int whitePreset = 5, blackPreset = 4;  // indices into the difficulty list ("Custom" excluded)
    int timeControl = 5;                   // as NewGameSetup::timeControl (-1 = custom)
    int customBaseSeconds = 300, customIncrementSeconds = 3, customDelaySeconds = 0;
};

// In-game pause menu (Esc). canClaimDraw enables the claim entry; canOfferDraw = false greys out
// "Offer draw" (e.g. an offer is already pending). Esc resumes.
MenuAction pauseMenu(bool canClaimDraw, bool canOfferDraw = true);
// Same, for a hot-seat game: 'resignQuestion' replaces the text of the resignation confirmation
// (it names the player to move). canResign = false greys out "Resign" (a move is on its way).
MenuAction pauseMenu(bool canClaimDraw, bool canOfferDraw, const std::string& resignQuestion, bool canResign = true);
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
// Brightness calibration (every start until it is completed, --calibrate): the next mainMenu()
// call opens on it instead of the title page. Three squares (black, mid grey, white, each with a
// black knight) show what the 3D frame would show at the brightness of the slider. Continue
// stores the brightness in game::settings(), Esc keeps the stored one; both complete it
// (Settings::brightnessCalibrated), save the .ini and go on to the title page.
void openBrightnessCalibration();
// The next mainMenu() call opens on the "Saved games" page (back from a replay); on the title page
// when that call has no library.
void openSavedGames();
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
    bool replay = false;         // a saved game replayed: its keys head the controls hint, and the
                                 // labels above leave room for its bar (replayBar)
};
void viewerHud(const ViewerHud& hud);

// ---- Replay of a saved game (ui_screens_game.cpp) ----------------------------------------------
// The replay's bar, bottom centre over the viewer overlay (drawn after viewerHud while it is
// visible): the move counter, the buttons start, one move back, pause / resume, one move forward,
// end (mouse only, with tooltips naming their keys: the keyboard is the game's), and the speed.
struct ReplayBar {
    int move = 0, moves = 0;     // full moves played so far, in the whole game
    std::string speed;           // "×2", "Instant" (already translated)
    bool paused = false;         // the middle button resumes
    bool atStart = false;        // start and back greyed out
    bool atEnd = false;          // forward and end greyed out
    bool aboveCard = false;      // the folded game over bar is shown: the bar sits above it
};
enum class ReplayAction { None, Start, Back, TogglePause, Forward, End };
ReplayAction replayBar(const ReplayBar& bar);

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

// ---- Coach mode (ui_coach.cpp) ------------------------------------------------------------------
// The coach page (title entry "Coach"), the coach's subtitles, the takeback offer card and the Esc
// menu of a coach game. Names, descriptions and texts of the levels come from assets/i18n
// (coach.level.<n>.name / .desc / .detail).
//
// Levels, index = level: 0 is the interactive lesson on the rules, then the player's Elo bands.
struct CoachLevelInfo {
    int eloLow = 0, eloHigh = 0;  // band shown on the page ("600–900"); both 0 = the rules lesson;
                                  // eloHigh 0 = open band ("2100+")
};
// Replaces the default list (0 rules, 600-900, 900-1200, 1200-1500, 1500-1800, 1800-2100, 2100+).
// A level without translated texts shows its band only.
void setCoachLevels(const std::vector<CoachLevelInfo>& levels);
const std::vector<CoachLevelInfo>& coachLevels();

// Choices of the coach page. The page starts from game::settings() [coach] and writes them back
// (and saves the .ini) on Start.
struct CoachSetup {
    int level = 1;               // index into coachLevels(): 0 = the rules lesson
    int colour = 2;              // the player's colour: 0 White, 1 Black, 2 alternate (the rules
                                 // lesson is always played with White: Settings::coachPlayerColour)
    // Set by the game before mainMenu(): false when the coach's voice files (tts::modelFolder(),
    // <application data>/coach/) are missing or failed to load. The page then says in one line that
    // the coach will speak through subtitles only.
    bool voiceAvailable = true;
};

// The coach's words, bottom centre, over the game (LAYER_OVERLAY: under tooltips and the pointer,
// above the game over card). The game owns the timing: 'age' follows the audio clock of the
// utterance so that text and voice stay in step. Do not draw it while a menu is open (paused,
// ui::optionsOpen()) or while the promotion picker is up: it would paint over them.
struct Subtitle {
    std::string text;            // already in the UI language; "" = none (the last text fades out)
    float age = 1e9f;            // seconds since this line started (fades in over 0.18 s)
    float duration = 0.0f;       // seconds it stays, then fades out over 0.35 s (subtitleDuration)
    float bottom = 0.0f;         // lowest y the plate may use (reference px); 0 = 96 above the bottom
                                 // edge. Pass v.y - 110 while the folded game over bar is shown.
    bool speaker = true;         // "COACH" tag above the text
};
void subtitles(const Subtitle& s);
// How long a subtitle should stay: the audio length or the time needed to read the text,
// whichever is longer (about 15 Latin or 7 CJK characters a second), plus a short margin.
float subtitleDuration(const std::string& text, float audioSeconds);

// Coach overlay at the table: the takeback offer card (after a blunder has been explained) and
// the "Space: skip" hint while something skippable runs (an explanation, a demonstration, the
// appraisal at the end of the game). The card's buttons are mouse only: the keyboard stays with
// the game, which reads the keys itself (Backspace accepts, as the Controls tab says; Space never
// answers the card, it skips the coach's talk; touching a piece plays on).
struct CoachHud {
    bool offer = false;          // show the takeback card
    std::string offerText;       // its question ("" = coach.offer.text)
    bool skippable = false;      // show the skip hint (bottom, start side)
};
enum class CoachHudAction { None, TakeBack, PlayOn };
CoachHudAction coachHud(const CoachHud& hud);

// Esc menu of a coach game: Resume, Take back (last move), Offer draw, Claim draw, Resign,
// Options, Main menu (confirmed). Entries whose flag is false are left out (Resume, Options and
// Main menu always show). Returns Resume, TakeBack, OfferDraw, ClaimDraw, Resign, OptionsChanged
// or BackToMainMenu. Esc resumes.
struct CoachPause {
    bool canTakeBack = false;    // the player has a move to take back
    bool canOfferDraw = false;
    bool canClaimDraw = false;
    bool canResign = true;       // false in the rules lesson
    bool mayEndGame = true;      // false while a move is on its way: Claim draw and Resign greyed
};
MenuAction coachPauseMenu(const CoachPause& p);

// ---- Coach voice download (ui_model_download.cpp) ---------------------------------------------
// The prompt that offers to download the coach's voice model (Supertonic 3, not shipped with the
// game) and the progress panel of the download. The game owns the job and the decisions
// (game/coach_model.h: call that, not these, from a scene); these draw and report the choice.
// Texts: coach.download.* in assets/i18n.
struct ModelPrompt {
    double bytes = 145316356.0;  // download size, shown in the text and on the Download button
    std::string folder;          // where the files go (shown in small print)
};
enum class ModelPromptAction { None, Download, NotNow };
// Modal card over whatever is on screen (menus or the table; what was drawn before it this frame
// and the game's input are blocked from the next frame on). "Read the licence" turns the card
// into the OpenRAIL-M text, Back returns. Esc = Not now (Back on the licence). Draw it every frame
// while it is open, after the menus and HUD; it closes when it returns an action.
ModelPromptAction modelPrompt(const ModelPrompt& p);

struct ModelProgressView {
    enum class State { Hidden, Checking, Downloading, Extracting, Failed };
    State state = State::Hidden;   // Hidden: fades out
    double done = 0.0, total = 0.0;   // the bar and, while downloading, the megabytes
    bool github = false;           // the source: the GitHub release archive, else Hugging Face
    std::string sourceLabel;       // "huggingface.co/csukuangfj2/...", "k2-fsa/sherpa-onnx release"
    std::string file;              // the file in progress (small print), "" = none
    std::string error;             // Failed: the reason, translated
};
enum class ModelPanelAction { None, Cancel, Retry, Close };
// Non-modal panel in the top end corner, over the menus and the table alike: what is happening,
// the bar, megabytes, the host, Cancel; when it failed, the reason with Close / Retry. Its
// buttons are mouse only (the keyboard stays with the menus and the game).
ModelPanelAction modelProgressPanel(const ModelProgressView& v);
// Called when the player picks "Coach" on the title page, as the Coach page opens (the game's
// voice download prompt hooks in here: game::coachModelInit). nullptr = none.
void setCoachEntryHook(std::function<void()> hook);

// ---- Saved games (ui_library.cpp) ----------------------------------------------------------------
// The "Saved games" page (title entry after "Watch a Game"): the games of the pgn folder
// (game_archive.h: the player's own games, saved when they end, and any PGN file dropped there),
// newest first, with a filter by mode; the details of the selected game (its tags and moves);
// Replay, Delete (confirmed; files of one game only) and Open folder (the system's file manager).
// The list is read on a worker thread and read again every few seconds while the page is open.
struct ReplaySetup {
    std::string path;            // the .pgn file
    int game = 0;                // the game's index in the file (0 = the first)
};
struct LibrarySetup {
    std::string folder;          // the pgn folder (plat::appDataDirectory() + "pgn/"); "" = no
                                 // "Saved games" entry on the title page
    ReplaySetup replay;          // the game to replay when mainMenu() returns StartReplay
};
// Title screen over the 3D hall. Handles its sub-pages (New Game, Options, Credits) itself.
// The New Game page starts from the last choices saved in game::settings() and writes them back
// (and saves the .ini) when the player presses Start; 'setup' then holds the choice.
// OptionsChanged is returned on the frame the player applies new options (already stored in
// game::settings() and saved); the game re-applies display/graphics/audio settings.
// The title page also shows the player's Elo (game::settings() [player]) and the New Game page
// shows it next to the opponent list.
// The "Watch a Game" entry fills 'watch' (returns StartWatching on its Start), the "Coach" entry
// fills 'coach' (returns StartCoach on its Start; see CoachSetup), and the "Saved games" entry
// returns StartReplay on Replay ('library.replay' then names the game; no such entry when
// library.folder is empty).
MenuAction mainMenu(NewGameSetup& setup, WatchSetup& watch, CoachSetup& coach, LibrarySetup& library);

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
