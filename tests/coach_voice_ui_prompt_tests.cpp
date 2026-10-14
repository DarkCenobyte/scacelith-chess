// The coach's voice model download as the UI draws it (src/ui/ui_model_download.cpp), driven frame
// by frame (tests/ui_driver.h) with made-up ui::ModelPrompt and ui::ModelProgressView values: the
// prompt in its two forms (a first download, the update of the old INT8 model) with each of its
// answers, the licence view and its keys, the modal block it puts on what is drawn before it and
// after it; the progress panel in every phase (checking, removing, downloading from either source,
// failed) with its mouse-only buttons and its fade; the hooks of the title page's Coach entry and of
// the update row of Options > Audio. These functions only draw and report the player's answer:
// nothing here downloads anything (game/coach_model.cpp owns the job, tests/coach_voice_ui_game_tests.cpp
// covers it without one).
#include "test.h"
#include "ui_driver.h"
#include "game/settings.h"
#include "i18n/i18n.h"
#include <string>
#include <vector>

using ui::ModelPanelAction;
using ui::ModelPromptAction;
using ui::ModelProgressView;
using ui::gfx::Rect;

namespace {

std::string T(const char* key) { return i18n::tr(key); }

template <class A>
int count(const std::vector<A>& v, A a) {
    int n = 0;
    for (A x : v) n += x == a ? 1 : 0;
    return n;
}

// The label of the prompt's Download (or Update) button for these bytes, as the prompt writes it.
std::string downloadLabel(const char* key, const char* megabytes) { return i18n::trf(key, {megabytes}); }

// The settings and the language, and the hooks this file sets, restored when a test ends. The
// Coach entry's and the update row's hooks belong to the game (game::coachModelInit sets them
// again when its scene starts).
struct Restore {
    game::Settings settings = game::settings();
    ~Restore() {
        i18n::setLanguage("en");
        game::settings() = settings;
        game::settings().save();
        ui::setCoachEntryHook(nullptr);
        ui::setVoiceUpdateHooks(nullptr, nullptr);
    }
};

// The prompt drawn every frame while it is open (it closes when it answers), with a button drawn
// before it and one after it.
struct PromptScreen {
    ui::ModelPrompt p;
    bool open = true;
    std::vector<ModelPromptAction> answers;
    int behind = 0, above = 0;
    void attach(uitest::Driver& d) {
        d.screen = [this] {
            if (ui::im::button("Behind", Rect(40, 40, 200, 50))) ++behind;
            if (open) {
                ModelPromptAction a = ui::modelPrompt(p);
                if (a != ModelPromptAction::None) {
                    answers.push_back(a);
                    open = false;
                }
            }
            if (ui::im::button("Above", Rect(40, 990, 200, 50))) ++above;
        };
    }
};

}  // namespace

// The first download: its size (on the Download button, with the language's decimal separator),
// Not now and Download answers, Esc as Not now (not on the frame it appears), Enter on Download
// (the button the prompt focuses). While it is open nothing else can be clicked; once it has
// answered, the page under it can again.
TEST(coach_voice_ui_prompt_answers) {
    REQUIRE(uitest::app());
    Restore restore;
    uitest::Driver d;
    uitest::Sounds sounds;
    PromptScreen s;
    s.p.bytes = 398651400.0;
    s.p.folder = "/home/player/.local/share/scacelith/coach/";
    s.attach(d);
    d.frames(20);
    CHECK_EQ(sounds.count(ui::Sound::Open), 1);
    const std::string dl = downloadLabel("coach.download.download", "398.7");
    REQUIRE(d.has(dl));
    REQUIRE(d.has(T("coach.download.not_now")));
    REQUIRE(d.has(T("coach.download.read_licence")));
    CHECK(ui::wantsMouse());
    CHECK(ui::wantsKeyboard());
    // The page under it and the overlay over it are blocked.
    REQUIRE(d.click("Behind"));
    REQUIRE(d.click("Above"));
    CHECK_EQ(s.behind + s.above, 0);
    REQUIRE(d.click(T("coach.download.not_now")));
    CHECK_EQ(count(s.answers, ModelPromptAction::NotNow), 1);
    d.frame();
    REQUIRE(d.click("Behind"));
    REQUIRE(d.click("Above"));
    CHECK_EQ(s.behind, 1);
    CHECK_EQ(s.above, 1);

    // Again: Download, with its sound.
    s.open = true;
    d.frames(20);
    const int confirms = sounds.count(ui::Sound::Confirm);
    REQUIRE(d.click(dl));
    CHECK_EQ(count(s.answers, ModelPromptAction::Download), 1);
    CHECK_EQ(sounds.count(ui::Sound::Confirm), confirms + 1);
    // Esc: Not now, with the sound of going back.
    s.open = true;
    d.frames(5);
    const int backs = sounds.count(ui::Sound::Back);
    d.press(plat::KEY_ESCAPE);
    CHECK_EQ(count(s.answers, ModelPromptAction::NotNow), 2);
    CHECK_EQ(sounds.count(ui::Sound::Back), backs + 1);
    // The Esc of the frame it appears on answers nothing.
    d.frame();
    s.open = true;
    d.in.keyDown[plat::KEY_ESCAPE] = d.in.keyPressed[plat::KEY_ESCAPE] = true;
    d.frame();
    d.in.keyDown[plat::KEY_ESCAPE] = false;
    CHECK(s.open);
    // Enter: the button it focuses as it opens, Download.
    d.frames(3);
    ui::im::setKeyboardMode(true);
    d.press(plat::KEY_ENTER);
    CHECK_EQ(count(s.answers, ModelPromptAction::Download), 2);
    ui::im::setKeyboardMode(false);
    d.frame();
}

// The update form (the old INT8 model replaced): its own Update button with the size, the same
// answers; a long folder in small print; in a right-to-left language Not now stands on the right.
// A small window keeps the card inside it.
TEST(coach_voice_ui_prompt_update_form) {
    REQUIRE(uitest::app());
    Restore restore;
    uitest::Driver d;
    PromptScreen s;
    s.p.update = true;
    s.p.bytes = 398651400.0;
    s.p.oldBytes = 145316356.0;
    s.p.folder = "C:\\Users\\A player with a rather long name\\AppData\\Roaming\\scacelith\\coach\\voice\\models\\supertonic\\";
    s.attach(d);
    d.frames(20);
    const std::string up = downloadLabel("coach.update.download", "398.7");
    REQUIRE(d.has(up));
    CHECK(!d.has(downloadLabel("coach.download.download", "398.7")));
    const Rect notNow = d.find(T("coach.download.not_now"))->r;
    CHECK(notNow.x < d.find(up)->r.x);
    REQUIRE(d.click(T("coach.download.not_now")));
    CHECK_EQ(count(s.answers, ModelPromptAction::NotNow), 1);

    // German writes the size with a comma.
    REQUIRE(i18n::setLanguage("de"));
    s.open = true;
    d.frames(20);
    REQUIRE(d.has(downloadLabel("coach.update.download", "398,7")));
    REQUIRE(d.click(downloadLabel("coach.update.download", "398,7")));
    CHECK_EQ(count(s.answers, ModelPromptAction::Download), 1);

    // Arabic: mirrored.
    REQUIRE(i18n::setLanguage("ar"));
    s.open = true;
    s.p.update = false;
    d.frames(20);
    const ui::im::TracedItem* later = d.find(T("coach.download.not_now"));
    REQUIRE(later != nullptr);
    CHECK(later->r.x > 960.0f);
    REQUIRE(d.click(T("coach.download.not_now")));
    CHECK_EQ(count(s.answers, ModelPromptAction::NotNow), 2);

    // A small window: the card fits in it.
    REQUIRE(i18n::setLanguage("en"));
    d.width = 800;
    d.height = 600;
    s.open = true;
    d.frames(20);
    const ui::im::TracedItem* nn = d.find(T("coach.download.not_now"));
    REQUIRE(nn != nullptr);
    CHECK(nn->r.b() <= 1080.0f);
    CHECK(nn->r.x >= 0.0f);
    d.press(plat::KEY_ESCAPE);
    CHECK_EQ(count(s.answers, ModelPromptAction::NotNow), 3);
    d.width = 1920;
    d.height = 1080;
    d.frame();
}

// "Read the licence" turns the card into the OpenRAIL-M text: the prompt's buttons go, its Back
// comes; the wheel, the arrows, PageUp / PageDown, Home and End scroll the text and leave it open;
// Esc (and Back) return to the prompt without answering it. The ui viewer opens it on the licence
// directly (ui::debug::showModelLicence).
TEST(coach_voice_ui_prompt_licence) {
    REQUIRE(uitest::app());
    Restore restore;
    uitest::Driver d;
    uitest::Sounds sounds;
    PromptScreen s;
    s.p.folder = "/tmp/voice/";
    s.attach(d);
    d.frames(20);
    REQUIRE(d.click(T("coach.download.read_licence")));
    d.frames(3);
    CHECK(!d.has(T("coach.download.not_now")));
    REQUIRE(d.has(T("coach.download.back")));
    // The text is longer than the card: it scrolls, and the licence stays open whatever the key.
    const Rect back = d.find(T("coach.download.back"))->r;
    d.move(back.cx(), back.y - 300.0f);
    for (int i = 0; i < 4; ++i) d.wheel(-2.0f);
    for (int key : {plat::KEY_PAGEDOWN, plat::KEY_DOWN, plat::KEY_END, plat::KEY_UP, plat::KEY_PAGEUP, plat::KEY_HOME}) {
        d.press(key);
        d.frames(10);
        CHECK(d.has(T("coach.download.back")));
    }
    for (int i = 0; i < 4; ++i) d.wheel(2.0f);
    CHECK(s.answers.empty());
    // Esc: back to the prompt (not an answer).
    const int backs = sounds.count(ui::Sound::Back);
    d.press(plat::KEY_ESCAPE);
    d.frame();
    CHECK_EQ(sounds.count(ui::Sound::Back), backs + 1);
    CHECK(s.answers.empty());
    REQUIRE(d.has(T("coach.download.not_now")));
    // Its Back button likewise.
    REQUIRE(d.click(T("coach.download.read_licence")));
    d.frames(3);
    REQUIRE(d.click(T("coach.download.back")));
    CHECK(d.has(T("coach.download.read_licence")));
    CHECK(s.answers.empty());
    REQUIRE(d.click(T("coach.download.not_now")));
    CHECK_EQ(count(s.answers, ModelPromptAction::NotNow), 1);

    // Opened on the licence; Back then shows the prompt; the next opening is the prompt's own.
    ui::debug::showModelLicence();
    s.open = true;
    d.frames(5);
    CHECK(d.has(T("coach.download.back")));
    CHECK(!d.has(T("coach.download.not_now")));
    REQUIRE(d.click(T("coach.download.back")));
    REQUIRE(d.click(T("coach.download.not_now")));
    s.open = true;
    d.frames(5);
    CHECK(d.has(T("coach.download.not_now")));
    d.press(plat::KEY_ESCAPE);
    CHECK_EQ(count(s.answers, ModelPromptAction::NotNow), 3);
    d.frame();
}

// The progress panel: hidden, nothing (not even a draw); checking the files, removing the old
// version and downloading (from Hugging Face or its archive copy, with the megabytes and the file
// in progress) show Cancel, mouse only, which answers Cancel; a failure shows its reason with Close
// and Retry. Hidden again, it fades out with what it showed, its buttons dead. It stands in the top
// end corner: at the right, at the left in Arabic.
TEST(coach_voice_ui_progress_panel) {
    REQUIRE(uitest::app());
    Restore restore;
    uitest::Driver d;
    ModelProgressView v;
    std::vector<ModelPanelAction> acts;
    d.screen = [&] {
        ModelPanelAction a = ui::modelProgressPanel(v);
        if (a != ModelPanelAction::None) acts.push_back(a);
    };
    d.wait(0.5f);
    CHECK_EQ(d.drawsOfFrame(), 0u);
    CHECK(!d.has(T("coach.download.cancel")));

    v.state = ModelProgressView::State::Checking;
    v.done = 1.0e6;
    v.total = 4.0e6;
    v.sourceLabel = "example.org/voice-model";
    d.wait(0.5f);
    const ui::im::TracedItem* cancel = d.find(T("coach.download.cancel"));
    REQUIRE(cancel != nullptr);
    CHECK((cancel->flags & ui::im::ITEM_MOUSE_ONLY) != 0);
    CHECK(cancel->r.x > 960.0f && cancel->r.y < 400.0f);
    CHECK(d.drawsOfFrame() >= 2u);   // the panel, and the source's line in a clip of its own
    REQUIRE(d.click(T("coach.download.cancel")));
    CHECK_EQ(count(acts, ModelPanelAction::Cancel), 1);
    const Rect checking = d.find(T("coach.download.cancel"))->r;

    v.state = ModelProgressView::State::Removing;
    d.wait(0.2f);
    REQUIRE(d.has(T("coach.download.cancel")));
    v.state = ModelProgressView::State::Downloading;
    v.done = 163.2e6;
    v.total = 398651400.0;
    v.file = "vector_estimator.onnx";
    d.wait(0.3f);
    v.archiveCopy = true;
    v.done = 20.0e6;   // the archive copy starts again: the bar goes back at once
    v.file = "a-file-with-a-very-long-name-that-has-to-be-set-smaller-in-the-panel.onnx";
    d.wait(0.3f);
    REQUIRE(d.has(T("coach.download.cancel")));
    CHECK(!d.has(T("coach.download.retry")));
    CHECK(d.find(T("coach.download.cancel"))->r.y == checking.y);   // the same panel

    // Failed: the reason (a long one is cut to four lines), Close and Retry.
    v = ModelProgressView();
    v.state = ModelProgressView::State::Failed;
    v.error = T("coach.download.error.network");
    d.wait(0.2f);
    CHECK(!d.has(T("coach.download.cancel")));
    REQUIRE(d.has(T("coach.download.close")));
    REQUIRE(d.has(T("coach.download.retry")));
    REQUIRE(d.click(T("coach.download.close")));
    REQUIRE(d.click(T("coach.download.retry")));
    CHECK_EQ(count(acts, ModelPanelAction::Close), 1);
    CHECK_EQ(count(acts, ModelPanelAction::Retry), 1);
    const float shortY = d.find(T("coach.download.retry"))->r.y;
    for (int i = 0; i < 8; ++i) v.error += " " + T("coach.download.error.network");
    d.frame();
    CHECK(d.find(T("coach.download.retry"))->r.y > shortY);

    // Hidden: it fades out as it was; a click on its buttons meanwhile does nothing.
    v = ModelProgressView();
    d.frame();
    REQUIRE(d.has(T("coach.download.retry")));
    REQUIRE(d.click(T("coach.download.retry")));
    CHECK_EQ(count(acts, ModelPanelAction::Retry), 1);
    d.wait(0.6f);
    CHECK(!d.has(T("coach.download.retry")));
    CHECK_EQ(d.drawsOfFrame(), 0u);

    // Arabic: at the left.
    REQUIRE(i18n::setLanguage("ar"));
    v.state = ModelProgressView::State::Downloading;
    v.done = 1.0e6;
    v.total = 2.0e6;
    d.wait(0.5f);
    const ui::im::TracedItem* left = d.find(T("coach.download.cancel"));
    REQUIRE(left != nullptr);
    CHECK(left->r.r() < 960.0f);
    v = ModelProgressView();
    d.wait(0.6f);
    CHECK_EQ(d.drawsOfFrame(), 0u);
    CHECK_EQ(acts.size(), size_t(3));
}

// The hooks the game gives the UI: the Coach entry of the title page calls its hook as the page
// opens; the update row of Options > Audio shows what its query says (its size on the button,
// greyed out while a download runs) and its button calls the open hook; Voice quality is greyed
// out until a model is installed. Without hooks, neither the row nor a call.
TEST(coach_voice_ui_entry_and_update_row_hooks) {
    REQUIRE(uitest::app());
    Restore restore;
    game::Settings& g = game::settings();
    g.coachVoice = true;
    int entries = 0, opened = 0;
    ui::VoiceUpdateRow row;
    ui::setCoachEntryHook([&] { ++entries; });
    ui::setVoiceUpdateHooks([&] { return row; }, [&] { ++opened; });
    uitest::Driver d;
    ui::NewGameSetup setup;
    ui::WatchSetup watch;
    ui::CoachSetup coach;
    ui::LibrarySetup library;
    d.screen = [&] { ui::mainMenu(setup, watch, coach, library); };
    d.frames(3);
    REQUIRE(d.click(T("menu.coach")));
    CHECK_EQ(entries, 1);
    d.frames(30);
    d.press(plat::KEY_ESCAPE);
    d.frames(3);
    ui::detail::coachEntryOpened();
    CHECK_EQ(entries, 2);

    // Options > Audio: no model, no row; Voice quality greyed out.
    const std::string button = i18n::trf("options.voice_update.button", {"398.7"});
    ui::debug::setOptionsTab(2);
    REQUIRE(d.click(T("menu.options")));
    d.frames(30);
    REQUIRE(d.has(T("options.voice_quality")));
    CHECK(d.disabled(T("options.voice_quality")));
    CHECK(!d.has(button));
    // The old model installed: the row and its button with the new model's size; quality on.
    row.show = true;
    row.modelInstalled = true;
    row.bytes = 398651400.0;
    d.frames(3);
    REQUIRE(d.has(button));
    CHECK(!d.disabled(T("options.voice_quality")));
    REQUIRE(d.click(button));
    CHECK_EQ(opened, 1);
    ui::detail::openVoiceUpdate();
    CHECK_EQ(opened, 2);
    // A download under way: the button greyed out, quality too.
    row.running = true;
    row.modelInstalled = false;
    d.frames(3);
    CHECK(d.disabled(button));
    CHECK(d.disabled(T("options.voice_quality")));
    REQUIRE(d.click(button));
    CHECK_EQ(opened, 2);

    // Without hooks: no row, and the calls do nothing.
    ui::setCoachEntryHook(nullptr);
    ui::setVoiceUpdateHooks(nullptr, nullptr);
    d.frames(3);
    CHECK(!d.has(button));
    CHECK(d.disabled(T("options.voice_quality")));
    ui::detail::coachEntryOpened();
    ui::detail::openVoiceUpdate();
    CHECK_EQ(entries, 2);
    CHECK_EQ(opened, 2);
    d.press(plat::KEY_ESCAPE);
    d.frames(3);
}
