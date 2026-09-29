// Persistent player settings (Scacelith.ini). Shared by the menus (ui/), the game and main.cpp.
#pragma once
#include "../render/renderer.h"
#include "../ui/ui_font.h"
#include "elo.h"
#include <string>
#include <vector>

namespace game {

// A player of rated hot-seat games on this PC, by name (New Game > Human, same PC > Rated game):
// one [local_player_N] section each (name, elo, games, wins, draws, losses, peak). Separate from
// the [player] rating, which only games against Stockfish change.
struct LocalPlayer {
    std::string name;
    elo::Record record;
};

struct Settings {
    // [display]
    int displayWidth = 1600;
    int displayHeight = 900;
    bool fullscreen = true;       // borderless fullscreen
    bool vsync = true;
    float renderScale = 1.0f;
    // [graphics]
    int quality = 2;              // 0 Low, 1 Medium, 2 High, 3 Ultra
    bool motionBlur = true;
    bool depthOfField = true;
    float brightness = 0.0f;      // exposure compensation (EV)
    // [audio]
    float masterVolume = 0.9f;
    float effectsVolume = 1.0f;
    float ambienceVolume = 0.7f;
    bool ambience = true;
    // [gameplay]
    bool showLegalMoves = true;   // highlight the legal destinations of the touched piece
    bool showCoordinates = false; // board has no printed coordinates by default (tournament boards)
    float mouseSensitivity = 1.0f;
    bool invertLook = false;
    int nextColor = -1;           // -1 = random (first game), 0 = white, 1 = black
    // Hot-seat: the view goes from one player's eyes to the other's after each move, in a camera
    // flight of this length (0.8 to 2 s), or 0 = an instant cut through black (motion sickness).
    float handoverSeconds = 1.6f;
    // [newgame] last choices on the new game screen
    int opponent = 0;             // 0 Stockfish, 1 a second human on this PC (hot-seat)
    int difficultyPreset = 3;     // index into ai::presets()
    int timeControlPreset = 5;    // index into chess::timeControlPresets()
    int customBaseSeconds = 600;
    int customIncrementSeconds = 5;
    int customDelaySeconds = 0;
    // Custom engine settings (used when the "Custom" difficulty preset is selected)
    int customSkillLevel = 10;
    bool customLimitElo = false;
    int customElo = 1800;
    int customDepth = 0;
    int customMoveTimeMs = 0;
    int customNodes = 0;
    int engineThreads = 1;
    int engineHashMB = 64;
    bool humanizeThinking = true; // spend realistic time before moving
    // [player] the human's rating (elo.h), updated after every rated game against Stockfish
    int playerElo = 1500;
    int playerGames = 0, playerWins = 0, playerDraws = 0, playerLosses = 0;
    int playerPeakElo = 1500;
    // [hotseat] last choices of the two-player game (New Game > Human, same PC), by colour
    std::string hotseatNames[2];  // "" = the Options > Player name for White, "Player 2" (translated) for Black
    int hotseatHands[2] = {-1, -1};  // ui::font::HandStyle; -1 = the Options > Player hand / another one
    int hotseatClockRightOf = 0;  // the clock stands at White's (0) or Black's (1) right
    bool hotseatRated = false;    // rated between the two names (localPlayers), friendly by default
    // [local_player_N] ratings of the rated hot-seat games, by name
    std::vector<LocalPlayer> localPlayers;
    LocalPlayer* findLocalPlayer(const std::string& name);          // nullptr when unknown
    const LocalPlayer* findLocalPlayer(const std::string& name) const;
    LocalPlayer& localPlayer(const std::string& name);              // found, or added (1500, no games)
    // [viewer] last choices on the Watch a Game page (Stockfish vs Stockfish)
    int viewerWhitePreset = 5;    // index into ai::presets() (Custom excluded)
    int viewerBlackPreset = 4;
    int viewerTimeControl = 5;    // index into chess::timeControlPresets(), -1 = custom
    int viewerCustomBaseSeconds = 300;
    int viewerCustomIncrementSeconds = 3;
    int viewerCustomDelaySeconds = 0;
    bool viewerShowControls = true; // the controls hint overlay (H)
    // [online] the server of online play (Options > Online). Never any token: the network layer
    // keeps the sessions itself, per server.
    bool onlineCustomServer = false;  // false = the official server of this build (when it has one)
    std::string onlineHost;
    int onlineApiPort = 44664;        // HTTPS API (Scacelith servers use 44664 for both)
    int onlineWsPort = 0;             // WSS; 0 = the API port
    std::string onlinePin;            // SHA-256 of a self-signed community server's certificate
    // Last choices of the online pages
    std::string onlineCategory = "5+3";
    bool onlineRated = true;
    int onlineColor = 0;              // challenges and private games: 0 random, 1 White, 2 Black
    int onlineCustomBaseSeconds = 600, onlineCustomIncrementSeconds = 5;
    // [direct] direct match (no server)
    int directPort = 47100;
    bool directUpnp = true;
    int directTimeControl = 7;        // index into chess::timeControlPresets() (10+5), -1 = custom
    int directBaseSeconds = 600, directIncrementSeconds = 5;
    int directColor = 0;              // host's colour: 0 random, 1 White, 2 Black
    std::string directAddress;        // last address joined
    int directJoinPort = 47100;
    // [interface]
    std::string language;         // i18n code ("fr", "zh-Hant"...); "" = the OS language (first start)
    // [player] (written on the scoresheets)
    std::string playerName = "Human";
    ui::font::HandStyle handStyle = ui::font::HAND_CAVEAT;  // Latin/Cyrillic handwriting

    // Selects the UI language (i18n::setLanguage): "--lang <code>" on the command line for this
    // session, else 'language', else the OS language when supported, else English. load() calls
    // it; an empty 'language' receives the language chosen from the OS.
    void applyLanguage();

    render::RenderSettings renderSettings() const;
    bool load(const std::string& path);  // missing file = defaults
    bool save() const;                   // writes back to the loaded path (or user data dir)
    std::string path;
};

Settings& settings();

}  // namespace game
