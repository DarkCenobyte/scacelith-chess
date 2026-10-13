// The headless game (headless.h): the null OpenGL (null_gl.h) and the fake platform layer
// (fake_platform.h) run the real scenes without a GPU or a window.
#include "test.h"
#include "headless.h"
#include "null_gl.h"
#include "gl/gl46.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {
const char* g_dropped = nullptr;   // the entry point dropProc() does not resolve
void* dropProc(const char* name) { return g_dropped && std::strcmp(name, g_dropped) == 0 ? nullptr : nullgl::getProc(name); }
}  // namespace

// The null driver resolves every entry point the game loads, and gl46::load names the first one a
// driver lacks.
TEST(headless_null_gl_resolves_every_entry_point) {
    CHECK_EQ(nullgl::load(), 0);
    CHECK(nullgl::getProc("glDrawElements") != nullptr);
    CHECK(nullgl::getProc("glNotAnEntryPoint") == nullptr);
    g_dropped = "glFrontFace";
    const char* missing = nullptr;
    CHECK_EQ(gl46::load(&dropProc, &missing), 1);
    CHECK(missing && std::strcmp(missing, "glFrontFace") == 0);
    CHECK(glFrontFace == nullptr);
    g_dropped = nullptr;
    CHECK_EQ(nullgl::load(), 0);
    CHECK(glFrontFace != nullptr);
}

// The game scene loads its world (the hall, the chess set, the clock, the robots) and shows the
// title page with every entry, drawing through the renderer and the UI.
TEST(headless_game_reaches_the_title_page) {
    headless::App* app = headless::shared({"--no-intro"});
    REQUIRE(app);
    REQUIRE(app->waitForLabel("New Game", 20000));
    const std::vector<std::string> labels = app->labels();
    for (const char* entry : {"New Game", "Coach", "Play Online", "Watch a Game", "Saved Games", "Analysis", "Options",
                              "Credits", "Quit"})
        CHECK(std::find(labels.begin(), labels.end(), entry) != labels.end());
    const uint64_t draws = nullgl::stats().draws;
    app->frames(2);
    CHECK(nullgl::stats().draws > draws);   // every frame draws
    CHECK(nullgl::stats().programsLinked > 0);
}
