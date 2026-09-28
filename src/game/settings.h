// Persistent player settings (Scacelith.ini). Shared by the menus (ui/), the game and main.cpp.
#pragma once
#include "../render/renderer.h"
#include "../ui/ui_font.h"
#include <string>

namespace game {

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
    // [newgame] last choices on the new game screen
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
