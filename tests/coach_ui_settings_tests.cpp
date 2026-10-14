// What the Coach page starts from besides its own clicks: the command line of Coach mode
// (src/game/coach_args.h: a challenge and the position to begin at, with the values it refuses)
// and the [coach] codecs of the settings it reads and writes (src/game/settings_coach.cpp) when an
// allocation fails halfway (tests/alloc_fail.h): the exception goes through and nothing is left
// half done, the next call works.
#include "test.h"
#include "alloc_fail.h"
#include "game/coach_args.h"
#include "game/settings.h"
#include <new>
#include <string>
#include <vector>

using game::CoachArgs;
using game::parseCoachArgs;

namespace {

// Whether 'call' throws std::bad_alloc when the next allocation of this thread fails.
template <class F>
bool throwsOutOfMemory(F call) {
    allocfail::failNextHere();
    try {
        call();
    } catch (const std::bad_alloc&) {
        return true;
    }
    return false;
}

}  // namespace

// --coach-challenge names the set a challenge starts with, --coach-challenge-position the position
// to begin at (1-based on the command line, 0-based in CoachArgs, 1..99). A missing value, one that
// is another switch, a position out of range or not a number: ignored and reported, the switches
// after them still read.
TEST(coach_ui_settings_challenge_command_line) {
    CoachArgs c = parseCoachArgs({"scacelith", "--start", "--coach", "--coach-challenge", "fork", "--coach-challenge-position", "3"});
    CHECK(c.start);
    CHECK_EQ(c.challenge, std::string("fork"));
    CHECK_EQ(c.challengePosition, 2);
    CHECK(c.problems.empty());
    CHECK_EQ(parseCoachArgs({"--coach-challenge-position", "1"}).challengePosition, 0);
    CHECK_EQ(parseCoachArgs({"--coach-challenge-position", "99"}).challengePosition, 98);

    // A switch where the id should be: reported, and that switch is read.
    CoachArgs sw = parseCoachArgs({"--coach-challenge", "--start", "--coach"});
    CHECK(sw.challenge.empty());
    CHECK(sw.start);
    CHECK_EQ(int(sw.problems.size()), 1);
    CHECK(sw.problems[0].find("--coach-challenge expects a challenge id") != std::string::npos);
    CoachArgs end = parseCoachArgs({"--coach-challenge"});
    CHECK(end.challenge.empty());
    CHECK_EQ(int(end.problems.size()), 1);
    CHECK_EQ(int(parseCoachArgs({"--coach-challenge", ""}).problems.size()), 1);

    // Positions out of range, not a number, missing, or a switch: reported, the default kept; a
    // value that is not a switch is skipped with it.
    for (const char* bad : {"0", "100", "3x", "-2", ""}) {
        CoachArgs p = parseCoachArgs({"--coach-challenge-position", bad, "--coach-level", "2"});
        CHECK_EQ(p.challengePosition, 0);
        CHECK_EQ(p.level, 2);
        REQUIRE(p.problems.size() == 1u);
        CHECK(p.problems[0] == "--coach-challenge-position expects 1..99, got '" + std::string(bad) + "'");
    }
    CoachArgs sw2 = parseCoachArgs({"--coach-challenge-position", "--coach-level", "4"});
    CHECK_EQ(sw2.challengePosition, 0);
    CHECK_EQ(sw2.level, 4);
    CHECK_EQ(int(sw2.problems.size()), 1);
    CoachArgs last = parseCoachArgs({"--coach-challenge-position"});
    CHECK_EQ(int(last.problems.size()), 1);
}

// Out of memory halfway through the [coach] codecs and the command line: std::bad_alloc goes
// through each of them (what they had built is freed), and the same call works afterwards.
TEST(coach_ui_settings_codecs_when_memory_runs_out) {
    if (!allocfail::available()) SKIP("AddressSanitizer build: no simulated out of memory");
    allocfail::Reset reset;
    const std::vector<game::Settings::CoachGame> games = {{3, 1, 81.44}, {2, -1, -1.0}, {0, 0, 55.0}, {6, 1, 99.0}};
    CHECK(throwsOutOfMemory([&] { game::encodeCoachHistory(games); }));
    CHECK_EQ(game::encodeCoachHistory(games), std::string("3:1:81.4 2:-1:- 0:0:55.0 6:1:99.0"));
    CHECK(throwsOutOfMemory([] { game::decodeCoachHistory("3:1:81.4 2:-1:- 0:0:55.0"); }));
    CHECK_EQ(game::decodeCoachHistory("3:1:81.4 2:-1:- 0:0:55.0").size(), size_t(3));
    const std::vector<std::string> ids = {"mate1", "backrank", "discovered", "intermezzo"};
    CHECK(throwsOutOfMemory([&] { game::encodeChallengeIds(ids); }));
    CHECK_EQ(game::encodeChallengeIds(ids), std::string("mate1,backrank,discovered,intermezzo"));
    CHECK(throwsOutOfMemory([] { game::decodeChallengeIds("mate1,fork"); }));
    CHECK_EQ(game::decodeChallengeIds("mate1,fork").size(), size_t(2));
    const std::vector<std::string> args = {"--coach-dir", "/a/folder/with/a/rather/long/name/coach", "--coach-level", "3"};
    CHECK(throwsOutOfMemory([&] { parseCoachArgs(args); }));
    CHECK_EQ(parseCoachArgs(args).level, 3);
}
