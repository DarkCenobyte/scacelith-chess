// Coach mode in the game scene: the engine-free parts (the command line, the [coach] and [tts]
// settings written to and read back from an .ini file).
#include "test.h"
#include "core/ini.h"
#include "game/coach_args.h"
#include "game/settings.h"
#include <algorithm>

using game::CoachArgs;
using game::parseCoachArgs;

TEST(coach_args_start_and_level) {
    CoachArgs c = parseCoachArgs({"scacelith", "--start", "--coach", "--coach-level", "4", "--coach-colour", "black"});
    CHECK(c.start);
    CHECK_EQ(c.level, 4);
    CHECK_EQ(c.colour, 1);
    CHECK(c.dir.empty());
    CHECK(!c.stageTest);
    CHECK(c.problems.empty());
    // --coach alone opens the menu as usual; --start alone is a game against Stockfish.
    CHECK(!parseCoachArgs({"--coach"}).start);
    CHECK(!parseCoachArgs({"--start"}).start);
    CHECK(parseCoachArgs({"--coach", "--start"}).start);
    // Defaults: the [coach] settings decide.
    CoachArgs d = parseCoachArgs({"--start", "--coach"});
    CHECK_EQ(d.level, -1);
    CHECK_EQ(d.colour, -1);
}

TEST(coach_args_lesson_colour_dir) {
    CoachArgs c = parseCoachArgs({"--start", "--coach", "--coach-level", "0", "--coach-color", "white", "--coach-dir",
                                  "/opt/voices/coach"});
    CHECK_EQ(c.level, 0);
    CHECK_EQ(c.colour, 0);
    CHECK_EQ(c.dir, std::string("/opt/voices/coach"));
    CHECK(c.start);
}

TEST(coach_args_stage_test_and_bad_values) {
    CoachArgs t = parseCoachArgs({"--coach-stage-test"});
    CHECK(t.stageTest);
    CHECK(t.start);  // the stage test is a coach game
    // Out of range, not a number, a missing value: ignored and reported, the next switch still read.
    CoachArgs b = parseCoachArgs({"--start", "--coach", "--coach-level", "9", "--coach-colour", "--coach-dir"});
    CHECK_EQ(b.level, -1);
    CHECK_EQ(b.colour, -1);
    CHECK(b.dir.empty());
    CHECK_EQ(int(b.problems.size()), 3);
    CHECK(b.start);
    CoachArgs n = parseCoachArgs({"--coach-level", "3x", "--start", "--coach"});
    CHECK_EQ(n.level, -1);
    CHECK(n.start);
    CoachArgs last = parseCoachArgs({"--start", "--coach", "--coach-level"});
    CHECK_EQ(last.level, -1);
    CHECK_EQ(int(last.problems.size()), 1);
    // Scripted runs: the takeback card answers itself.
    CHECK_EQ(parseCoachArgs({"--coach-auto-answer", "yes"}).autoAnswer, 1);
    CHECK_EQ(parseCoachArgs({"--coach-auto-answer", "no"}).autoAnswer, 0);
    CoachArgs bad = parseCoachArgs({"--coach-auto-answer", "maybe", "--start", "--coach"});
    CHECK_EQ(bad.autoAnswer, -1);
    CHECK_EQ(int(bad.problems.size()), 1);
    CHECK(bad.start);
}

TEST(coach_settings_history_codec) {
    std::vector<game::Settings::CoachGame> games = {{3, 1, 81.44}, {3, -1, -1.0}, {0, 0, 55.0}};
    std::string text = game::encodeCoachHistory(games);
    CHECK_EQ(text, std::string("3:1:81.4 3:-1:- 0:0:55.0"));
    std::vector<game::Settings::CoachGame> back = game::decodeCoachHistory(text);
    CHECK_EQ(int(back.size()), 3);
    CHECK_EQ(back[0].level, 3);
    CHECK_EQ(back[0].result, 1);
    CHECK(back[0].accuracy > 81.39 && back[0].accuracy < 81.41);
    CHECK(back[1].accuracy < 0.0);
    CHECK_EQ(back[1].result, -1);
    CHECK_EQ(back[2].level, 0);
    // Malformed entries are skipped, values clamped.
    std::vector<game::Settings::CoachGame> odd = game::decodeCoachHistory("  9:5:120 garbage 2:0:  4:-3:12.5 ");
    CHECK_EQ(int(odd.size()), 3);
    CHECK_EQ(odd[0].level, 6);
    CHECK_EQ(odd[0].result, 1);
    CHECK(odd[0].accuracy == 100.0);
    CHECK(odd[1].accuracy < 0.0);
    CHECK_EQ(odd[2].result, -1);
    // A NaN accuracy (a hand-edited file) is unknown, not an accurate game. glibc reads "nan" as NaN
    // (unknown) and "inf" as infinity (100); msvcrt reads both as 0: never NaN.
    std::vector<game::Settings::CoachGame> nan = game::decodeCoachHistory("3:1:nan 2:0:inf 4:1:50");
    CHECK_EQ(int(nan.size()), 3);
    CHECK(nan[0].accuracy == -1.0 || nan[0].accuracy == 0.0);
    CHECK(nan[1].accuracy == 100.0 || nan[1].accuracy == 0.0);
    CHECK(nan[2].accuracy == 50.0);
    // Only the last kCoachHistoryMax games are kept.
    std::vector<game::Settings::CoachGame> many;
    for (int i = 0; i < game::Settings::kCoachHistoryMax + 5; ++i) many.push_back({1 + i % 6, 0, double(i)});
    std::vector<game::Settings::CoachGame> kept = game::decodeCoachHistory(game::encodeCoachHistory(many));
    CHECK_EQ(int(kept.size()), game::Settings::kCoachHistoryMax);
    CHECK(kept.front().accuracy > 4.9 && kept.front().accuracy < 5.1);
}

TEST(coach_settings_round_trip) {
    game::Settings a;
    a.coachLevel = 5;
    a.coachColour = 1;
    a.coachNextColour = 1;
    a.coachRulesDone = true;
    a.coachHistory = {{5, 1, 88.2}, {5, 0, -1.0}};
    a.coachAccuracyExplained = true;
    a.coachLessonChapter = 7;
    a.ttsThreads = 3;
    a.ttsVoice = 6;
    a.ttsSteps = 8;
    a.ttsArch = "avx2";
    IniFile out;
    game::writeCoachSettings(out, a);
    const char* path = "/tmp/scacelith_coach_settings_test.ini";
    CHECK(out.save(path));
    IniFile in;
    CHECK(in.load(path));
    game::Settings b;
    game::readCoachSettings(in, b);
    CHECK_EQ(b.coachLevel, 5);
    CHECK_EQ(b.coachColour, 1);
    CHECK_EQ(b.coachNextColour, 1);
    CHECK(b.coachRulesDone);
    CHECK_EQ(int(b.coachHistory.size()), 2);
    CHECK_EQ(b.coachHistory[0].result, 1);
    CHECK(b.coachHistory[0].accuracy > 88.1 && b.coachHistory[0].accuracy < 88.3);
    CHECK(b.coachHistory[1].accuracy < 0.0);
    CHECK(b.coachAccuracyExplained);
    CHECK_EQ(b.coachLessonChapter, 7);
    CHECK_EQ(b.ttsThreads, 3);
    CHECK_EQ(b.ttsVoice, 6);
    CHECK_EQ(b.ttsSteps, 8);
    CHECK_EQ(b.ttsArch, std::string("avx2"));
    std::remove(path);
    // Voice quality: a hand-edited step count outside Options' range is brought back into it.
    for (int steps : {1, 3, 4, 9, 10, 99}) {
        IniFile e;
        e.setInt("tts.steps", steps);
        game::Settings d;
        game::readCoachSettings(e, d);
        CHECK_EQ(d.ttsSteps, std::clamp(steps, game::Settings::kTtsStepsMin, game::Settings::kTtsStepsMax));
    }
    CHECK(game::Settings::kTtsStepsMin == 4 && game::Settings::kTtsStepsMax == 9 && game::Settings().ttsSteps == 5);

    // A file from before these keys: the defaults stay.
    IniFile old;
    old.setInt("coach.level", 2);
    game::Settings c;
    game::readCoachSettings(old, c);
    CHECK_EQ(c.coachLevel, 2);
    CHECK(c.coachHistory.empty());
    CHECK(!c.coachAccuracyExplained);
    CHECK_EQ(c.coachLessonChapter, 0);
    CHECK_EQ(c.ttsThreads, 0);
    CHECK_EQ(c.ttsVoice, -1);
    CHECK_EQ(c.ttsSteps, 5);
    CHECK_EQ(c.ttsArch, std::string("auto"));
}
