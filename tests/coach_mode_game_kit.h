// Helpers of the tests of the coach games of levels 1 to 6 in the game scene
// (tests/coach_mode_game_*.cpp, src/game/game_scene_coach.cpp), on top of coach_mode_support.h:
//   - the command line of a coach game started at once (--start --coach), its voice model folder
//     empty (the coach speaks through subtitles);
//   - the voice model's prompt declined should it be open (never its download, and never Space
//     while it is open);
//   - frames that skip what the coach says (Space), bounded by the wall clock;
//   - frames whose game time never runs ahead of the wall clock, for the coach's review of a move:
//     it waits a few seconds of game time for the analyses of Stockfish, which searches in real
//     time (frames on the null OpenGL run many times faster than the game's clock).
#pragma once
#include "coach_mode_support.h"
#include "fake_platform.h"
#include "platform/platform.h"
#include <chrono>
#include <functional>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

namespace coachgame {

// --no-intro --start --coach --coach-dir <the empty voice folder>, then 'more'.
inline std::vector<std::string> gameArgs(std::initializer_list<const char*> more) {
    std::vector<std::string> args = {"--no-intro", "--start", "--coach", "--coach-dir", coachtest::voiceFolder()};
    for (const char* a : more) args.push_back(a);
    return args;
}

// The voice model's prompt is on the screen (its "Not now" button).
inline bool voicePromptShown(const headless::App& app) {
    return coachtest::findId(app, coachtest::idIn({"modelprompt"}, "coach.download.not_now")) != nullptr;
}

// The voice model's prompt answered "Not now" should it be open: the coach then speaks through
// subtitles, its voice switched off. A test that switches the voice off for its frames and puts
// the settings back when it ends (gstest::SettingsGuard) leaves the prompt to open on the next
// frame of the same App, as if the player had switched the voice on in the Options. Its default
// button, which Space or Enter would press, would start the model's download.
inline bool declineVoicePrompt(headless::App& app) {
    app.frames(2);
    if (!voicePromptShown(app)) return true;
    app.frames(20);   // faded in: its buttons answer
    if (!coachtest::clickId(app, coachtest::idIn({"modelprompt"}, "coach.download.not_now"))) return false;
    app.frames(2);
    return !voicePromptShown(app);
}

// Frames of 'dt' until 'done', Space pressed every 'spaceEvery' frames (0: never), at most
// 'seconds' of wall-clock time (the engine plays and analyses in real time beside them). Never
// Space while the voice model's prompt is open (false at once).
inline bool skipping(headless::App& app, const std::function<bool()>& done, double seconds, int spaceEvery = 8,
                     float dt = 1.0f / 20.0f) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; !done(); ++i) {
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > seconds) return false;
        if (voicePromptShown(app)) return false;
        if (spaceEvery > 0 && i % spaceEvery == spaceEvery - 1) app.key(plat::KEY_SPACE);
        else if (!app.frame(dt)) return done();
    }
    return true;
}

// The same, the game's time never ahead of the wall clock: what the coach waits for in game time
// (the analyses of a review, kA0Wait and the like in coach/session.cpp) then has its time on any
// machine, however slow the engine.
inline bool paced(headless::App& app, const std::function<bool()>& done, double seconds, int spaceEvery = 8,
                  float dt = 1.0f / 30.0f) {
    const auto start = std::chrono::steady_clock::now();
    double game = 0.0;
    for (int i = 0; !done(); ++i) {
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (wall > seconds) return false;
        if (game > wall) std::this_thread::sleep_for(std::chrono::duration<double>(game - wall));
        if (voicePromptShown(app)) return false;
        if (spaceEvery > 0 && i % spaceEvery == spaceEvery - 1) {
            app.key(plat::KEY_SPACE);   // two frames
            game += 2.0 / 60.0;
        } else {
            if (!app.frame(dt)) return done();
            game += dt;
        }
    }
    return true;
}

// The coach's end card ("Play again"), after its closing words, the handshake and the appraisal
// (not skipped: the Space that skips them may answer the card in the frame it comes up, see
// coach_mode_game_mate_again_card_answered_at_once_level_suggested).
inline bool waitEndCard(headless::App& app, double seconds = 60.0) {
    return skipping(app, [&] { return app.hasLabel("Play again"); }, seconds, 0);
}

// Esc, then frames until the coach's pause menu shows ("Resume").
inline bool openPause(headless::App& app) {
    app.key(plat::KEY_ESCAPE);
    return app.waitForLabel("Resume", 60);
}

}  // namespace coachgame
