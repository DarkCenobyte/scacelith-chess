// Command line of Coach mode (the switches are listed in game_scene.h). Header-only and engine-free
// so that the unit tests parse the same way as the game (tests/coach_scene_tests.cpp).
#pragma once
#include <cstdlib>
#include <string>
#include <vector>

namespace game {

struct CoachArgs {
    bool start = false;       // --start --coach (or --coach-stage-test): straight to a coach game
    int level = -1;           // --coach-level N: 0 = the rules lesson, 1..6; -1 = Settings [coach] level
    int colour = -1;          // --coach-colour white|black: 0 / 1; -1 = Settings (the lesson: White)
    std::string dir;          // --coach-dir <path>: the voice's model folder; "" = <exe dir>/coach/
    bool stageTest = false;   // --coach-stage-test: the scene's Stage performs a fixed sequence
    std::vector<std::string> problems;   // values that were ignored, for the log
};

inline CoachArgs parseCoachArgs(const std::vector<std::string>& args) {
    CoachArgs c;
    bool start = false, coach = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        bool hasValue = i + 1 < args.size();
        const std::string value = hasValue ? args[i + 1] : std::string();
        if (a == "--start") {
            start = true;
        } else if (a == "--coach") {
            coach = true;
        } else if (a == "--coach-stage-test") {
            c.stageTest = true;
        } else if (a == "--coach-level") {
            char* end = nullptr;
            long n = hasValue ? std::strtol(value.c_str(), &end, 10) : -1;
            if (hasValue && end && *end == '\0' && !value.empty() && n >= 0 && n <= 6) {
                c.level = int(n);
                ++i;
            } else {
                c.problems.push_back("--coach-level expects 0..6, got '" + value + "'");
                if (hasValue && value.compare(0, 2, "--") != 0) ++i;
            }
        } else if (a == "--coach-colour" || a == "--coach-color") {
            if (value == "white" || value == "black") {
                c.colour = value == "white" ? 0 : 1;
                ++i;
            } else {
                c.problems.push_back(a + " expects white or black, got '" + value + "'");
                if (hasValue && value.compare(0, 2, "--") != 0) ++i;
            }
        } else if (a == "--coach-dir") {
            if (hasValue && !value.empty() && value.compare(0, 2, "--") != 0) {
                c.dir = value;
                ++i;
            } else {
                c.problems.push_back("--coach-dir expects a folder");
            }
        }
    }
    c.start = (start && coach) || c.stageTest;
    return c;
}

}  // namespace game
