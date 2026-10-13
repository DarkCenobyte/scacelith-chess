// The fonts and the drawing layer of the UI (src/ui/ui_font.cpp, src/ui/ui_draw.cpp and the data
// calls of src/ui/ui.cpp) on the font atlas the "ui" scene builds (tests/ui_driver.h): the
// standalone distance fields of the 3D markings, the atlas cleared when it fills up, line wrapping
// (CJK included), measures and carets, clipping, the shapes, and the preset names and lists the
// game gives the menus. The shaping itself is in tests/ui_text_shape_tests.cpp.
#include "test.h"
#include "ui_driver.h"
#include "i18n/i18n.h"
#include "ui/ui_draw.h"
#include "ui/ui_font.h"
#include <cmath>
#include <string>
#include <vector>

namespace uitest {
// For tests/ui_text_shape_tests.cpp, which cannot include the harness (see there).
bool fontsReady() { return app() != nullptr && ui::font::ready(); }
}  // namespace uitest

using namespace ui;
using gfx::Rect;
using gfx::TextStyle;

namespace {

// Restores English (and the glyphs of its strings) when a test switched the language.
struct English {
    ~English() { i18n::setLanguage("en"); }
};

bool nearly(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// "سلام" (peace): seen, lam, alef, meem.
const char* const kSalam = "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85";

}  // namespace

// The distance field of a line for the 3D markings: the ink centred, inside above 0.5 and outside
// below; refused for bad arguments, a line without ink and a line that does not fit.
TEST(ui_font_line_distance_field) {
    REQUIRE(uitest::app());
    std::vector<uint8_t> out;
    float ink = 0.0f;
    REQUIRE(font::renderLineSdf(font::FACE_TITLE, "H1", 24.0f, 4, 0.05f, 96, 48, out, &ink));
    REQUIRE(out.size() == size_t(96 * 48));
    CHECK(ink > 10.0f);
    CHECK(ink < 96.0f);
    CHECK(out[0] < 128);                         // a corner: outside
    uint8_t best = 0;
    for (uint8_t v : out) best = std::max(best, v);
    CHECK(best > 128);                           // somewhere inside the ink
    // Left and right margins alike: the ink is centred.
    int left = 0, right = 0;
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 48; ++x) left += out[size_t(y * 96 + x)] > 127 ? 1 : 0;
        for (int x = 48; x < 96; ++x) right += out[size_t(y * 96 + x)] > 127 ? 1 : 0;
    }
    CHECK(left > 0);
    CHECK(right > 0);
    CHECK(!font::renderLineSdf(-1, "H", 24.0f, 4, 0.0f, 96, 48, out));
    CHECK(out.empty());
    CHECK(!font::renderLineSdf(font::FACE_COUNT, "H", 24.0f, 4, 0.0f, 96, 48, out));
    CHECK(!font::renderLineSdf(font::FACE_TEXT, "H", 0.0f, 4, 0.0f, 96, 48, out));
    CHECK(!font::renderLineSdf(font::FACE_TEXT, "H", 24.0f, 0, 0.0f, 96, 48, out));
    CHECK(!font::renderLineSdf(font::FACE_TEXT, "H", 24.0f, 4, 0.0f, 0, 48, out));
    CHECK(!font::renderLineSdf(font::FACE_TEXT, "   ", 24.0f, 4, 0.0f, 96, 48, out));
    CHECK(!font::renderLineSdf(font::FACE_TEXT, "A line far too long", 24.0f, 4, 0.0f, 96, 48, out));
}

// Lines wrap at spaces (explicit line breaks kept) and between CJK characters; the widest line
// fits the width; textWrapped draws them and gives their count.
TEST(ui_font_wrapping) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    TextStyle st;
    st.size = 30.0f;
    const float oneTwo = gfx::textWidth("one two", st);
    CHECK_EQ(gfx::wrapLineCount("one two three four", oneTwo + 2.0f, st), 3);
    CHECK_EQ(gfx::wrapLineCount("one two three four", 10000.0f, st), 1);
    CHECK_EQ(gfx::wrapLineCount("one\ntwo\n\nthree", 10000.0f, st), 4);
    CHECK_EQ(gfx::wrapLineCount("", 100.0f, st), 1);
    CHECK(gfx::wrapWidth("one two three four", oneTwo + 2.0f, st) <= oneTwo + 2.0f);
    CHECK(nearly(gfx::wrapWidth("one two", 10000.0f, st), oneTwo, 0.01f));
    // A word longer than the width stays whole on its line.
    CHECK_EQ(gfx::wrapLineCount("unbreakable", 20.0f, st), 1);
    // Japanese has no spaces: it breaks between characters, never before "。".
    const std::string ja = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE6\x96\x87\xE7\xAB\xA0\xE3\x81\xA7\xE3\x81\x99\xE3\x80\x82";
    const float jaW = gfx::textWidth(ja, st);
    const int jaLines = gfx::wrapLineCount(ja, jaW * 0.4f, st);
    CHECK(jaLines >= 3);
    CHECK(gfx::wrapWidth(ja, jaW * 0.4f, st) <= jaW * 0.4f + 0.01f);
    TextStyle tracked = st;
    tracked.tracking = 0.1f;
    CHECK(gfx::wrapLineCount(ja, jaW * 0.4f, tracked) >= jaLines);
    // An Arabic paragraph wraps in its own direction.
    const std::string ar = std::string(kSalam) + " " + kSalam + " " + kSalam;
    CHECK_EQ(gfx::wrapLineCount(ar, gfx::textWidth(kSalam, st) + 5.0f, st), 3);
    int drawn = 0;
    d.screen = [&] {
        drawn = gfx::textWrapped("one two three four", 100.0f, 100.0f, oneTwo + 2.0f, st);
        gfx::textWrapped(ja, 100.0f, 400.0f, jaW * 0.4f, st, 40.0f);
    };
    CHECK(d.drawsOfFrame() >= 1);
    CHECK_EQ(drawn, 3);
}

// Measures: the text width shaped, a size that fits a width (never under its floor), the base
// direction, the cap height of a hand, and the carets of the text fields.
TEST(ui_font_measures_and_carets) {
    REQUIRE(uitest::app());
    English english;
    TextStyle st;
    st.size = 40.0f;
    const float w = gfx::textWidth("Scacelith", st);
    CHECK(w > 100.0f);
    CHECK_EQ(gfx::textWidth("", st), 0.0f);
    CHECK_EQ(gfx::fitSize("Scacelith", st, w + 10.0f), 40.0f);
    CHECK(nearly(gfx::fitSize("Scacelith", st, w * 0.8f), 32.0f, 0.01f));
    CHECK(nearly(gfx::fitSize("Scacelith", st, w * 0.1f, 0.5f), 20.0f));
    CHECK_EQ(gfx::textDirection(kSalam, st), 1);
    CHECK_EQ(gfx::textDirection(std::string("abc ") + kSalam, st), 0);
    TextStyle forced = st;
    forced.dir = 1;
    CHECK_EQ(gfx::textDirection("abc", forced), 1);
    REQUIRE(i18n::setLanguage("ar"));
    // In a right-to-left UI a line holding Arabic reads right to left whatever comes first.
    CHECK_EQ(gfx::textDirection(std::string("abc ") + kSalam, st), 1);
    CHECK_EQ(gfx::textDirection("abc", st), 0);
    i18n::setLanguage("en");
    TextStyle hand = st;
    hand.hand = font::HAND_MARCK;
    CHECK(nearly(gfx::capHeight(hand), font::metrics(font::FACE_HAND_MARCK).capHeight * 40.0f));
    CHECK(nearly(gfx::capHeight(st), font::metrics(font::FACE_TEXT).capHeight * 40.0f));
    CHECK(gfx::textWidth("Ann", hand) > 0.0f);
    CHECK_EQ(gfx::caretOffset("abc", st, 0), 0.0f);
    CHECK(nearly(gfx::caretOffset("abc", st, 3), gfx::textWidth("abc", st), 0.01f));
    CHECK_EQ(gfx::caretAt("abc", st, gfx::textWidth("abc", st)), 3);
    CHECK_EQ(gfx::caretAt("abc", st, gfx::caretOffset("abc", st, 1) + 0.5f), 1);
    TextStyle none = st;
    none.size = 0.0f;
    CHECK_EQ(gfx::caretAt("abc", none, 10.0f), 0);
}

// Clipping: nested clips intersect, an item outside the clip is not hovered, a clipped draw goes
// with its scissor rectangle and an empty clip draws nothing; every shape of the draw layer adds
// its quads (and a shape of no size none).
TEST(ui_font_clipping_and_shapes) {
    REQUIRE(uitest::app());
    uitest::Driver d;
    const Rect a(0, 0, 100, 100), b(50, 60, 100, 100);
    const Rect i = gfx::intersect(a, b);
    CHECK_EQ(i.x, 50.0f);
    CHECK_EQ(i.y, 60.0f);
    CHECK_EQ(i.w, 50.0f);
    CHECK_EQ(i.h, 40.0f);
    CHECK_EQ(gfx::intersect(a, Rect(200, 200, 10, 10)).w, 0.0f);
    int clicked = 0;
    bool insideSeen = false, outsideSeen = true;
    d.screen = [&] {
        gfx::pushClip(Rect(100, 100, 400, 400));
        gfx::pushClip(Rect(300, 300, 400, 400));   // the clip is now x 300..500, y 300..500
        insideSeen = gfx::clipContains(m::vec2(400, 400));
        outsideSeen = gfx::clipContains(m::vec2(600, 600));
        if (im::button("Clipped##clip", Rect(250, 250, 400, 400))) ++clicked;
        gfx::popClip();
        gfx::popClip();
        gfx::popClip();   // one too many: harmless
    };
    d.frames(2);
    CHECK(insideSeen);
    CHECK(!outsideSeen);
    d.clickAt(600, 600);   // on the button, outside the clip
    CHECK_EQ(clicked, 0);
    d.clickAt(400, 400);
    CHECK_EQ(clicked, 1);
    // Draw calls: one per layer and clip; an empty clip skipped.
    d.screen = [&] {
        gfx::fill(Rect(10, 10, 50, 50), m::vec4(1, 1, 1, 1));
        gfx::pushClip(Rect(100, 100, 200, 200));
        gfx::fill(Rect(120, 120, 50, 50), m::vec4(1, 0, 0, 1));
        gfx::popClip();
        gfx::pushClip(Rect(400, 400, 0, 0));
        gfx::fill(Rect(400, 400, 50, 50), m::vec4(1, 0, 0, 1));
        gfx::popClip();
        gfx::fill(Rect(500, 10, 50, 50), m::vec4(1, 1, 1, 1));
    };
    CHECK_EQ(d.drawsOfFrame(), 3u);   // unclipped, clipped, unclipped again
    d.screen = [&] {
        gfx::fill(Rect(10, 10, 0, 50), m::vec4(1, 1, 1, 1));   // no size
        gfx::line(m::vec2(5, 5), m::vec2(5, 5), m::vec4(1, 1, 1, 1), 2.0f);
    };
    CHECK_EQ(d.drawsOfFrame(), 0u);
    d.screen = [&] {
        gfx::setLayer(gfx::LAYER_BACK);
        gfx::pushAlpha(0.5f);
        gfx::fillV(Rect(10, 10, 100, 100), m::vec4(1, 1, 1, 1), m::vec4(0, 0, 0, 1), 4.0f);
        gfx::fillH(Rect(10, 120, 100, 100), m::vec4(1, 1, 1, 1), m::vec4(0, 0, 0, 1));
        gfx::pushAlpha(0.5f);
        gfx::stroke(Rect(10, 230, 100, 100), m::vec4(1, 1, 1, 1), 2.0f, 6.0f);
        gfx::stroke(Rect(10, 340, 100, 100), m::vec4(1, 1, 1, 1));
        gfx::popAlpha();
        gfx::popAlpha();
        gfx::popAlpha();   // one too many: harmless
        gfx::setLayer(gfx::LAYER_TOP);
        gfx::shadow(Rect(200, 10, 100, 100), 4.0f, 20.0f, m::vec4(0, 0, 0, 0.5f));
        gfx::hline(200, 400, 150, m::vec4(1, 1, 1, 1));
        gfx::hline(200, 400, 160, m::vec4(1, 1, 1, 1), 3.0f);
        gfx::vline(450, 10, 200, m::vec4(1, 1, 1, 1), 2.0f);
        gfx::hlineFade(200, 600, 300, m::vec4(1, 1, 1, 1), 0.3f, 2.0f);
        gfx::hlineFade(200, 210, 310, m::vec4(1, 1, 1, 1), 0.9f);
        gfx::line(m::vec2(600, 10), m::vec2(700, 200), m::vec4(1, 1, 1, 1), 2.0f);
        gfx::diamond(m::vec2(800, 100), 10.0f, m::vec4(1, 1, 1, 1), 1.5f);
        gfx::circle(m::vec2(900, 100), 20.0f, m::vec4(1, 1, 1, 1));
        gfx::circle(m::vec2(1000, 100), 20.0f, m::vec4(1, 1, 1, 1), 2.0f);
        gfx::radial(m::vec2(1100, 100), m::vec2(80, 40), m::vec4(0, 0, 0, 0.5f), 0.2f, 1.0f);
        CHECK(gfx::layer() == gfx::LAYER_TOP);
        gfx::setLayer(gfx::LAYER_MAIN);
    };
    CHECK_EQ(d.drawsOfFrame(), 2u);   // the back layer and the top layer
    CHECK_EQ(gfx::snap(10.4f), 10.0f);
    CHECK_EQ(gfx::px(), 1.0f);
}

// The atlas is one texture: when a session's glyphs no longer fit (several CJK languages), it is
// cleared at the start of the next frame and the glyphs are packed again as they are used; the
// text drawn afterwards still has its glyphs.
TEST(ui_font_atlas_clears_when_full) {
    REQUIRE(uitest::app());
    English english;
    uitest::Driver d;
    TextStyle st;
    st.size = 30.0f;
    d.screen = [&] { gfx::text("Scacelith", 100.0f, 100.0f, st); };
    d.frame();
    const int before = font::atlasGeneration();
    // Han characters of the two Chinese hands (those they have), packed a batch at a time until
    // one does not fit: its glyph stays without a quad until the clear.
    std::vector<std::pair<int, uint32_t>> han;
    for (uint32_t cp = 0x4E00; cp < 0x9FA0 && han.size() < 6000; ++cp)
        for (int f : {int(font::FACE_HAND_SC), int(font::FACE_HAND_TC)})
            if (font::resolveFace(f, cp) == f) han.push_back({f, cp});
    REQUIRE(han.size() >= 4000u);
    size_t fitted = 0;
    bool full = false;
    while (!full && fitted < han.size()) {
        const size_t end = std::min(han.size(), fitted + 200);
        font::prewarm(std::vector<std::pair<int, uint32_t>>(han.begin() + long(fitted), han.begin() + long(end)));
        for (size_t i = fitted; i < end && !full; ++i) {
            const font::Glyph* g = font::glyph(han[i].first, han[i].second);
            REQUIRE(g != nullptr);
            if (g->hasQuad) ++fitted;
            else full = true;
        }
    }
    REQUIRE(full);
    CHECK_EQ(font::atlasGeneration(), before);
    d.frame();   // the clear happens at the start of a frame
    CHECK_EQ(font::atlasGeneration(), before + 1);
    // Text drawn after the clear has its glyphs back in the atlas; the Han glyphs are packed
    // again (from the distance fields kept in memory) when they are used.
    const font::Glyph* s = font::glyph(font::FACE_TEXT, 'S');
    REQUIRE(s != nullptr);
    CHECK(s->hasQuad);
    CHECK(d.drawsOfFrame() >= 1);
    const size_t again = fitted * 4 / 5;
    font::prewarm(std::vector<std::pair<int, uint32_t>>(han.begin(), han.begin() + long(again)));
    CHECK(font::glyph(han[again - 1].first, han[again - 1].second)->hasQuad);
    CHECK_EQ(font::atlasGeneration(), before + 1);
    // A language switch prewarms the new language's glyphs; with the atlas more than half full
    // (four fifths of what filled it) it is cleared first.
    REQUIRE(i18n::setLanguage("zh-Hant"));
    d.frame();
    CHECK_EQ(font::atlasGeneration(), before + 2);
    CHECK(!font::glyph(han[0].first, han[0].second) || font::glyph(han[0].first, han[0].second)->hasQuad);
    i18n::setLanguage("en");
    d.frame();
    CHECK_EQ(font::atlasGeneration(), before + 2);   // little in the atlas now: no clear
    CHECK(font::glyph(font::FACE_TEXT, 'S')->hasQuad);
}

// The data calls of ui.h: the opponent presets' names and descriptions in the UI language (an
// unknown one as given), "Unlimited" translated, and the lists the game gives (an empty list
// keeps the defaults).
TEST(ui_font_preset_names_and_lists) {
    REQUIRE(uitest::app());
    English english;
    const ui::detail::Data saved = ui::detail::data();
    CHECK_EQ(ui::presetName("Club Player"), std::string("Club Player"));
    CHECK_EQ(ui::presetName("Mystery Bot"), std::string("Mystery Bot"));
    CHECK_EQ(ui::presetDescription("Mystery Bot", "Plays at random."), std::string("Plays at random."));
    CHECK_EQ(ui::timeControlLabel("3+2"), std::string("3+2"));
    REQUIRE(i18n::setLanguage("fr"));
    CHECK_EQ(ui::presetName("Club Player"), std::string("Joueur de club"));
    CHECK(ui::presetDescription("Club Player", "x") != std::string("x"));
    CHECK_EQ(ui::timeControlLabel("Unlimited"), std::string("Illimit\xC3\xA9" "e"));
    i18n::setLanguage("en");
    ui::setDifficultyList({});
    ui::setTimeControlList({});
    ui::setResolutionList({});
    CHECK_EQ(ui::detail::data().difficulties.size(), saved.difficulties.size());
    CHECK_EQ(ui::detail::data().timeControls.size(), saved.timeControls.size());
    ui::setDifficultyList({{"Rookie", "New to chess.", 900}, {"Custom", "Your own engine settings.", 0}});
    ui::setTimeControlList({"Unlimited", "5+0"});
    ui::setResolutionList({{1280, 720}});
    ui::setVersionString("9.9.9-test");
    CHECK_EQ(ui::detail::data().difficulties.size(), size_t(2));
    CHECK_EQ(ui::detail::data().timeControls[1], std::string("5+0"));
    CHECK_EQ(ui::detail::data().resolutions.size(), size_t(1));
    CHECK_EQ(ui::detail::data().version, std::string("9.9.9-test"));
    ui::detail::data() = saved;
}
