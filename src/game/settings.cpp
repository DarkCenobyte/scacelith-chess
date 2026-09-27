#include "settings.h"
#include "../core/ini.h"
#include "../core/log.h"
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
    nextColor = ini.getInt("gameplay.next_color", nextColor);
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
    return true;
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
    ini.setInt("gameplay.next_color", nextColor);
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
    if (!path.empty() && ini.save(path)) return true;
    std::string alt = plat::userDataDirectory() + "Scacelith.ini";
    if (ini.save(alt)) return true;
    LOGW("could not save settings");
    return false;
}

}  // namespace game
