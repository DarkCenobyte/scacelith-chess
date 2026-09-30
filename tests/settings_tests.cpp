// Settings: the inline rules of settings.h that the menus and the game share (the .ini reading
// and writing live in the game target, which the tests do not link).
#include "test.h"
#include "game/settings.h"

// Coach mode: the colour the player takes in the next coach game.
TEST(settings_coach_player_colour) {
    game::Settings s;
    CHECK_EQ(s.coachLevel, 1);
    CHECK_EQ(s.coachColour, 2);  // alternating by default
    CHECK_EQ(s.coachNextColour, 0);
    CHECK_EQ(s.coachPlayerColour(), 0);  // alternating starts with White
    s.coachNextColour = 1;
    CHECK_EQ(s.coachPlayerColour(), 1);
    // A fixed colour ignores the alternation.
    s.coachColour = 0;
    CHECK_EQ(s.coachPlayerColour(), 0);
    s.coachColour = 1;
    s.coachNextColour = 0;
    CHECK_EQ(s.coachPlayerColour(), 1);
    // The rules lesson (level 0) is always played with White.
    s.coachLevel = 0;
    CHECK_EQ(s.coachPlayerColour(), 0);
    s.coachColour = 2;
    s.coachNextColour = 1;
    CHECK_EQ(s.coachPlayerColour(), 0);
    // An out-of-range colour behaves as alternating.
    s.coachLevel = 4;
    s.coachColour = 7;
    CHECK_EQ(s.coachPlayerColour(), 1);
}

TEST(settings_coach_defaults) {
    game::Settings s;
    CHECK(s.voiceVolume > 0.99f && s.voiceVolume <= 1.0f);
    CHECK_EQ(s.subtitles, int(game::SubtitlesAuto));
    CHECK(!s.coachRulesDone);
}

// Subtitles: Automatic shows them when the voice does not speak the menus' language (Chinese
// menus: the voice speaks English); without the voice files they are always shown.
TEST(settings_coach_subtitles_shown) {
    using game::coachSubtitlesShown;
    CHECK(!coachSubtitlesShown(game::SubtitlesAuto, "en", "en", true));
    CHECK(!coachSubtitlesShown(game::SubtitlesAuto, "fr", "fr", true));
    CHECK(coachSubtitlesShown(game::SubtitlesAuto, "zh-Hans", "en", true));
    CHECK(coachSubtitlesShown(game::SubtitlesAuto, "zh-Hant", "en", true));
    CHECK(coachSubtitlesShown(game::SubtitlesOn, "en", "en", true));
    CHECK(!coachSubtitlesShown(game::SubtitlesOff, "en", "en", true));
    CHECK(!coachSubtitlesShown(game::SubtitlesOff, "zh-Hans", "en", true));
    // No voice: the subtitles are the coach's only way to speak, whatever the option says.
    CHECK(coachSubtitlesShown(game::SubtitlesOff, "en", "en", false));
    CHECK(coachSubtitlesShown(game::SubtitlesAuto, "de", "de", false));
}
