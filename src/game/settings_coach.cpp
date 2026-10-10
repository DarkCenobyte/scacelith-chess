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
        const double a = std::atof(acc.c_str());  // "nan" would pass the clamp: unknown, as "-"
        g.accuracy = acc.empty() || acc == "-" || std::isnan(a) ? -1.0 : std::clamp(a, 0.0, 100.0);
        out.push_back(g);
    }
    if (out.size() > size_t(Settings::kCoachHistoryMax)) out.erase(out.begin(), out.end() - Settings::kCoachHistoryMax);
    return out;
}

std::string encodeChallengeIds(const std::vector<std::string>& ids) {
    std::string out;
    for (const std::string& id : ids) {
        if (id.empty()) continue;
        if (!out.empty()) out += ',';
        out += id;
    }
    return out;
}

std::vector<std::string> decodeChallengeIds(const std::string& text) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= text.size()) {
        size_t end = text.find(',', i);
        if (end == std::string::npos) end = text.size();
        std::string id = text.substr(i, end - i);
        i = end + 1;
        size_t a = id.find_first_not_of(" \t"), b = id.find_last_not_of(" \t");
        if (a == std::string::npos) continue;
        id = id.substr(a, b - a + 1);
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
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
    s.coachTab = std::clamp(ini.getInt("coach.tab", s.coachTab), 0, 1);
    s.coachChallenge = ini.getString("coach.challenge", s.coachChallenge);
    if (ini.has("coach.challenges_done")) s.coachChallengesDone = decodeChallengeIds(ini.getString("coach.challenges_done"));
    s.ttsThreads = std::clamp(ini.getInt("tts.threads", s.ttsThreads), 0, 16);
    s.ttsVoice = std::max(-1, ini.getInt("tts.voice", s.ttsVoice));
    s.ttsSteps = std::clamp(ini.getInt("tts.steps", s.ttsSteps), Settings::kTtsStepsMin, Settings::kTtsStepsMax);
    s.ttsArch = ini.getString("tts.arch", s.ttsArch);
    if (s.ttsArch.empty()) s.ttsArch = "auto";
    s.coachVoice = ini.getBool("coach.voice", s.coachVoice);  // the voice model download (W12)
    s.coachVoiceOffered = ini.getBool("coach.voice_offered", s.coachVoiceOffered);
    s.coachVoiceUpdateOffered = ini.getBool("coach.voice_update_offered", s.coachVoiceUpdateOffered);
    s.analysisComments = ini.getBool("analysis.comments", s.analysisComments);
    s.analysisVoice = ini.getBool("analysis.voice", s.analysisVoice);
    s.analysisArrows = ini.getBool("analysis.arrows", s.analysisArrows);
}

void writeCoachSettings(IniFile& ini, const Settings& s) {
    ini.setInt("coach.level", s.coachLevel);
    ini.setInt("coach.colour", s.coachColour);
    ini.setInt("coach.next_colour", s.coachNextColour);
    ini.setBool("coach.rules_done", s.coachRulesDone);
    ini.set("coach.history", encodeCoachHistory(s.coachHistory));
    ini.setBool("coach.accuracy_explained", s.coachAccuracyExplained);
    ini.setInt("coach.lesson_chapter", s.coachLessonChapter);
    ini.setInt("coach.tab", s.coachTab);
    ini.set("coach.challenge", s.coachChallenge);
    ini.set("coach.challenges_done", encodeChallengeIds(s.coachChallengesDone));
    ini.setInt("tts.threads", s.ttsThreads);
    ini.setInt("tts.voice", s.ttsVoice);
    ini.setInt("tts.steps", s.ttsSteps);
    ini.set("tts.arch", s.ttsArch);
    ini.setBool("coach.voice", s.coachVoice);  // the voice model download (W12)
    ini.setBool("coach.voice_offered", s.coachVoiceOffered);
    ini.setBool("coach.voice_update_offered", s.coachVoiceUpdateOffered);
    ini.setBool("analysis.comments", s.analysisComments);
    ini.setBool("analysis.voice", s.analysisVoice);
    ini.setBool("analysis.arrows", s.analysisArrows);
}

}  // namespace game
