#include "settings.h"
#include "../core/ini.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "../platform/platform.h"
#include <algorithm>
#include <cstdio>

namespace game {

Settings& settings() {
    static Settings s;
    return s;
}

render::RenderSettings Settings::renderSettings() const {
    render::RenderSettings r;
    r.applyPreset(render::Quality(std::clamp(quality, 0, 3)));
    r.renderScale = std::clamp(renderScale, 0.5f, 2.0f);
    if (!motionBlur) r.motionBlur = false;
    if (!depthOfField) r.dof = false;
    return r;
}

bool Settings::load(const std::string& p) {
    path = p;
    IniFile ini;
    if (!ini.load(p)) {
        LOGI("no settings file at %s, using defaults", p.c_str());
        applyLanguage();
        return false;
    }
    displayWidth = ini.getInt("display.width", displayWidth);
    displayHeight = ini.getInt("display.height", displayHeight);
    fullscreen = ini.getBool("display.fullscreen", fullscreen);
    vsync = ini.getBool("display.vsync", vsync);
    renderScale = ini.getFloat("display.render_scale", renderScale);
    quality = ini.getInt("graphics.quality", quality);
    motionBlur = ini.getBool("graphics.motion_blur", motionBlur);
    depthOfField = ini.getBool("graphics.depth_of_field", depthOfField);
    brightness = ini.getFloat("graphics.brightness", brightness);
    masterVolume = ini.getFloat("audio.master_volume", masterVolume);
    effectsVolume = ini.getFloat("audio.effects_volume", effectsVolume);
    ambienceVolume = ini.getFloat("audio.ambience_volume", ambienceVolume);
    ambience = ini.getBool("audio.ambience", ambience);
    showLegalMoves = ini.getBool("gameplay.show_legal_moves", showLegalMoves);
    showCoordinates = ini.getBool("gameplay.show_coordinates", showCoordinates);
    mouseSensitivity = ini.getFloat("gameplay.mouse_sensitivity", mouseSensitivity);
    invertLook = ini.getBool("gameplay.invert_look", invertLook);
    gameCursor = ini.getBool("gameplay.game_cursor", gameCursor);
    nextColor = ini.getInt("gameplay.next_color", nextColor);
    handoverSeconds = std::clamp(ini.getFloat("gameplay.handover_seconds", handoverSeconds), 0.0f, 2.0f);
    if (handoverSeconds > 0.0f && handoverSeconds < 0.8f) handoverSeconds = 0.8f;
    opponent = std::clamp(ini.getInt("newgame.opponent", opponent), 0, 1);
    difficultyPreset = ini.getInt("newgame.difficulty", difficultyPreset);
    timeControlPreset = ini.getInt("newgame.time_control", timeControlPreset);
    customBaseSeconds = ini.getInt("newgame.custom_base_seconds", customBaseSeconds);
    customIncrementSeconds = ini.getInt("newgame.custom_increment_seconds", customIncrementSeconds);
    customDelaySeconds = ini.getInt("newgame.custom_delay_seconds", customDelaySeconds);
    customSkillLevel = ini.getInt("engine.skill_level", customSkillLevel);
    customLimitElo = ini.getBool("engine.limit_elo", customLimitElo);
    customElo = ini.getInt("engine.elo", customElo);
    customDepth = ini.getInt("engine.depth", customDepth);
    customMoveTimeMs = ini.getInt("engine.move_time_ms", customMoveTimeMs);
    customNodes = ini.getInt("engine.nodes", customNodes);
    engineThreads = ini.getInt("engine.threads", engineThreads);
    engineHashMB = ini.getInt("engine.hash_mb", engineHashMB);
    humanizeThinking = ini.getBool("engine.humanize", humanizeThinking);
    playerElo = ini.getInt("player.elo", playerElo);
    playerGames = ini.getInt("player.games", playerGames);
    playerWins = ini.getInt("player.wins", playerWins);
    playerDraws = ini.getInt("player.draws", playerDraws);
    playerLosses = ini.getInt("player.losses", playerLosses);
    playerPeakElo = std::max(playerElo, ini.getInt("player.peak", playerPeakElo));
    for (int i = 0; i < 2; ++i) {
        const char* side = i == 0 ? "white" : "black";
        hotseatNames[i] = ini.getString(std::string("hotseat.") + side + "_name", hotseatNames[i]);
        hotseatHands[i] = std::clamp(ini.getInt(std::string("hotseat.") + side + "_hand", hotseatHands[i]), -1,
                                     int(ui::font::HAND_STYLE_COUNT) - 1);
    }
    hotseatClockRightOf = std::clamp(ini.getInt("hotseat.clock_right_of", hotseatClockRightOf), 0, 1);
    hotseatRated = ini.getBool("hotseat.rated", hotseatRated);
    localPlayers.clear();
    for (int n = 1; n <= 256; ++n) {
        std::string sec = "local_player_" + std::to_string(n) + ".";
        if (!ini.has(sec + "name")) break;
        LocalPlayer p;
        p.name = ini.getString(sec + "name");
        p.record.rating = std::max(elo::kFloor, ini.getInt(sec + "elo", elo::kInitialRating));
        p.record.games = std::max(0, ini.getInt(sec + "games", 0));
        p.record.wins = std::max(0, ini.getInt(sec + "wins", 0));
        p.record.draws = std::max(0, ini.getInt(sec + "draws", 0));
        p.record.losses = std::max(0, ini.getInt(sec + "losses", 0));
        p.record.peak = std::max(p.record.rating, ini.getInt(sec + "peak", p.record.rating));
        if (!p.name.empty() && !findLocalPlayer(p.name)) localPlayers.push_back(p);
    }
    viewerWhitePreset = ini.getInt("viewer.white_preset", viewerWhitePreset);
    viewerBlackPreset = ini.getInt("viewer.black_preset", viewerBlackPreset);
    viewerTimeControl = ini.getInt("viewer.time_control", viewerTimeControl);
    viewerCustomBaseSeconds = ini.getInt("viewer.custom_base_seconds", viewerCustomBaseSeconds);
    viewerCustomIncrementSeconds = ini.getInt("viewer.custom_increment_seconds", viewerCustomIncrementSeconds);
    viewerCustomDelaySeconds = ini.getInt("viewer.custom_delay_seconds", viewerCustomDelaySeconds);
    viewerShowControls = ini.getBool("viewer.show_controls", viewerShowControls);
    onlineCustomServer = ini.getBool("online.custom_server", onlineCustomServer);
    onlineHost = ini.getString("online.host", onlineHost);
    onlineApiPort = std::clamp(ini.getInt("online.api_port", onlineApiPort), 1, 65535);
    onlineWsPort = std::clamp(ini.getInt("online.ws_port", onlineWsPort), 0, 65535);
    onlinePin = ini.getString("online.pinned_sha256", onlinePin);
    onlineCategory = ini.getString("online.category", onlineCategory);
    onlineRated = ini.getBool("online.rated", onlineRated);
    onlineColor = std::clamp(ini.getInt("online.color", onlineColor), 0, 2);
    onlineCustomBaseSeconds = ini.getInt("online.custom_base_seconds", onlineCustomBaseSeconds);
    onlineCustomIncrementSeconds = ini.getInt("online.custom_increment_seconds", onlineCustomIncrementSeconds);
    directPort = std::clamp(ini.getInt("direct.port", directPort), 1, 65535);
    directUpnp = ini.getBool("direct.upnp", directUpnp);
    directTimeControl = ini.getInt("direct.time_control", directTimeControl);
    directBaseSeconds = ini.getInt("direct.base_seconds", directBaseSeconds);
    directIncrementSeconds = ini.getInt("direct.increment_seconds", directIncrementSeconds);
    directColor = std::clamp(ini.getInt("direct.color", directColor), 0, 2);
    directAddress = ini.getString("direct.address", directAddress);
    directJoinPort = std::clamp(ini.getInt("direct.join_port", directJoinPort), 1, 65535);
    language = ini.getString("interface.language", language);
    playerName = ini.getString("player.name", playerName);
    if (playerName.empty()) playerName = "Human";
    handStyle = ui::font::HandStyle(std::clamp(ini.getInt("player.hand_style", int(handStyle)), 0, int(ui::font::HAND_STYLE_COUNT) - 1));
    applyLanguage();
    return true;
}

LocalPlayer* Settings::findLocalPlayer(const std::string& name) {
    for (LocalPlayer& p : localPlayers)
        if (p.name == name) return &p;
    return nullptr;
}

const LocalPlayer* Settings::findLocalPlayer(const std::string& name) const {
    for (const LocalPlayer& p : localPlayers)
        if (p.name == name) return &p;
    return nullptr;
}

LocalPlayer& Settings::localPlayer(const std::string& name) {
    if (LocalPlayer* p = findLocalPlayer(name)) return *p;
    LocalPlayer p;
    p.name = name;
    localPlayers.push_back(p);
    return localPlayers.back();
}

void Settings::applyLanguage() {
    if (language.empty() || i18n::languageIndex(language) < 0) {
        std::string os = plat::systemLanguage();
        language = i18n::matchLocale(os);
        LOGI("language: %s from the system locale '%s'", language.c_str(), os.c_str());
    }
    std::string code = language;
    const std::vector<std::string> args = plat::commandLine();
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == "--lang") code = i18n::matchLocale(args[i + 1]);
    i18n::setLanguage(code);
}

bool Settings::save() const {
    IniFile ini;
    ini.setInt("display.width", displayWidth);
    ini.setInt("display.height", displayHeight);
    ini.setBool("display.fullscreen", fullscreen);
    ini.setBool("display.vsync", vsync);
    ini.setFloat("display.render_scale", renderScale);
    ini.setInt("graphics.quality", quality);
    ini.setBool("graphics.motion_blur", motionBlur);
    ini.setBool("graphics.depth_of_field", depthOfField);
    ini.setFloat("graphics.brightness", brightness);
    ini.setFloat("audio.master_volume", masterVolume);
    ini.setFloat("audio.effects_volume", effectsVolume);
    ini.setFloat("audio.ambience_volume", ambienceVolume);
    ini.setBool("audio.ambience", ambience);
    ini.setBool("gameplay.show_legal_moves", showLegalMoves);
    ini.setBool("gameplay.show_coordinates", showCoordinates);
    ini.setFloat("gameplay.mouse_sensitivity", mouseSensitivity);
    ini.setBool("gameplay.invert_look", invertLook);
    ini.setBool("gameplay.game_cursor", gameCursor);
    ini.setInt("gameplay.next_color", nextColor);
    ini.setFloat("gameplay.handover_seconds", handoverSeconds);
    ini.setInt("newgame.opponent", opponent);
    ini.setInt("newgame.difficulty", difficultyPreset);
    ini.setInt("newgame.time_control", timeControlPreset);
    ini.setInt("newgame.custom_base_seconds", customBaseSeconds);
    ini.setInt("newgame.custom_increment_seconds", customIncrementSeconds);
    ini.setInt("newgame.custom_delay_seconds", customDelaySeconds);
    ini.setInt("engine.skill_level", customSkillLevel);
    ini.setBool("engine.limit_elo", customLimitElo);
    ini.setInt("engine.elo", customElo);
    ini.setInt("engine.depth", customDepth);
    ini.setInt("engine.move_time_ms", customMoveTimeMs);
    ini.setInt("engine.nodes", customNodes);
    ini.setInt("engine.threads", engineThreads);
    ini.setInt("engine.hash_mb", engineHashMB);
    ini.setBool("engine.humanize", humanizeThinking);
    ini.setInt("player.elo", playerElo);
    ini.setInt("player.games", playerGames);
    ini.setInt("player.wins", playerWins);
    ini.setInt("player.draws", playerDraws);
    ini.setInt("player.losses", playerLosses);
    ini.setInt("player.peak", playerPeakElo);
    ini.set("hotseat.white_name", hotseatNames[0]);
    ini.set("hotseat.black_name", hotseatNames[1]);
    ini.setInt("hotseat.white_hand", hotseatHands[0]);
    ini.setInt("hotseat.black_hand", hotseatHands[1]);
    ini.setInt("hotseat.clock_right_of", hotseatClockRightOf);
    ini.setBool("hotseat.rated", hotseatRated);
    for (size_t i = 0; i < localPlayers.size(); ++i) {
        const LocalPlayer& p = localPlayers[i];
        std::string sec = "local_player_" + std::to_string(i + 1) + ".";
        ini.set(sec + "name", p.name);
        ini.setInt(sec + "elo", p.record.rating);
        ini.setInt(sec + "games", p.record.games);
        ini.setInt(sec + "wins", p.record.wins);
        ini.setInt(sec + "draws", p.record.draws);
        ini.setInt(sec + "losses", p.record.losses);
        ini.setInt(sec + "peak", p.record.peak);
    }
    ini.setInt("viewer.white_preset", viewerWhitePreset);
    ini.setInt("viewer.black_preset", viewerBlackPreset);
    ini.setInt("viewer.time_control", viewerTimeControl);
    ini.setInt("viewer.custom_base_seconds", viewerCustomBaseSeconds);
    ini.setInt("viewer.custom_increment_seconds", viewerCustomIncrementSeconds);
    ini.setInt("viewer.custom_delay_seconds", viewerCustomDelaySeconds);
    ini.setBool("viewer.show_controls", viewerShowControls);
    ini.setBool("online.custom_server", onlineCustomServer);
    ini.set("online.host", onlineHost);
    ini.setInt("online.api_port", onlineApiPort);
    ini.setInt("online.ws_port", onlineWsPort);
    ini.set("online.pinned_sha256", onlinePin);
    ini.set("online.category", onlineCategory);
    ini.setBool("online.rated", onlineRated);
    ini.setInt("online.color", onlineColor);
    ini.setInt("online.custom_base_seconds", onlineCustomBaseSeconds);
    ini.setInt("online.custom_increment_seconds", onlineCustomIncrementSeconds);
    ini.setInt("direct.port", directPort);
    ini.setBool("direct.upnp", directUpnp);
    ini.setInt("direct.time_control", directTimeControl);
    ini.setInt("direct.base_seconds", directBaseSeconds);
    ini.setInt("direct.increment_seconds", directIncrementSeconds);
    ini.setInt("direct.color", directColor);
    ini.set("direct.address", directAddress);
    ini.setInt("direct.join_port", directJoinPort);
    ini.set("interface.language", language);
    ini.set("player.name", playerName);
    ini.setInt("player.hand_style", int(handStyle));
    if (!path.empty() && ini.save(path)) return true;
    std::string alt = plat::userDataDirectory() + "Scacelith.ini";
    if (ini.save(alt)) return true;
    LOGW("could not save settings");
    return false;
}

}  // namespace game
