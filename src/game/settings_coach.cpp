// The [coach] and [tts] sections of Scacelith.ini (see settings.h). Engine-free and GL-free: in the
// core library, so that tests/coach_scene_tests.cpp checks their round trip.
#include "settings.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace game {

std::string encodeCoachHistory(const std::vector<Settings::CoachGame>& games) {
    std::string out;
    size_t first = games.size() > size_t(Settings::kCoachHistoryMax) ? games.size() - size_t(Settings::kCoachHistoryMax) : 0;
    for (size_t i = first; i < games.size(); ++i) {
        const Settings::CoachGame& g = games[i];
        char buf[64];
        if (g.accuracy >= 0.0 && std::isfinite(g.accuracy))
            std::snprintf(buf, sizeof(buf), "%d:%d:%.1f", g.level, g.result, g.accuracy);
        else
            std::snprintf(buf, sizeof(buf), "%d:%d:-", g.level, g.result);
        if (!out.empty()) out += ' ';
        out += buf;
    }
    return out;
}

std::vector<Settings::CoachGame> decodeCoachHistory(const std::string& text) {
    std::vector<Settings::CoachGame> out;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = text.find(' ', i);
        if (end == std::string::npos) end = text.size();
        std::string entry = text.substr(i, end - i);
        i = end + 1;
        if (entry.empty()) continue;
        size_t c1 = entry.find(':'), c2 = c1 == std::string::npos ? std::string::npos : entry.find(':', c1 + 1);
        if (c2 == std::string::npos) continue;  // malformed: skipped
        Settings::CoachGame g;
        g.level = std::clamp(std::atoi(entry.substr(0, c1).c_str()), 0, 6);
        g.result = std::clamp(std::atoi(entry.substr(c1 + 1, c2 - c1 - 1).c_str()), -1, 1);
        std::string acc = entry.substr(c2 + 1);
        g.accuracy = acc.empty() || acc == "-" ? -1.0 : std::clamp(std::atof(acc.c_str()), 0.0, 100.0);
        out.push_back(g);
    }
    if (out.size() > size_t(Settings::kCoachHistoryMax)) out.erase(out.begin(), out.end() - Settings::kCoachHistoryMax);
    return out;
}

void readCoachSettings(const IniFile& ini, Settings& s) {
    s.coachLevel = std::max(0, ini.getInt("coach.level", s.coachLevel));  // the UI clamps to its list
    s.coachColour = std::clamp(ini.getInt("coach.colour", s.coachColour), 0, 2);
    s.coachNextColour = std::clamp(ini.getInt("coach.next_colour", s.coachNextColour), 0, 1);
    s.coachRulesDone = ini.getBool("coach.rules_done", s.coachRulesDone);
    if (ini.has("coach.history")) s.coachHistory = decodeCoachHistory(ini.getString("coach.history"));
    s.coachAccuracyExplained = ini.getBool("coach.accuracy_explained", s.coachAccuracyExplained);
    s.coachLessonChapter = std::max(0, ini.getInt("coach.lesson_chapter", s.coachLessonChapter));
    s.ttsThreads = std::clamp(ini.getInt("tts.threads", s.ttsThreads), 0, 16);
    s.ttsVoice = std::max(-1, ini.getInt("tts.voice", s.ttsVoice));
    s.ttsSteps = std::clamp(ini.getInt("tts.steps", s.ttsSteps), 1, 16);
    s.ttsArch = ini.getString("tts.arch", s.ttsArch);
    if (s.ttsArch.empty()) s.ttsArch = "auto";
}

void writeCoachSettings(IniFile& ini, const Settings& s) {
    ini.setInt("coach.level", s.coachLevel);
    ini.setInt("coach.colour", s.coachColour);
    ini.setInt("coach.next_colour", s.coachNextColour);
    ini.setBool("coach.rules_done", s.coachRulesDone);
    ini.set("coach.history", encodeCoachHistory(s.coachHistory));
    ini.setBool("coach.accuracy_explained", s.coachAccuracyExplained);
    ini.setInt("coach.lesson_chapter", s.coachLessonChapter);
    ini.setInt("tts.threads", s.ttsThreads);
    ini.setInt("tts.voice", s.ttsVoice);
    ini.setInt("tts.steps", s.ttsSteps);
    ini.set("tts.arch", s.ttsArch);
}

}  // namespace game
