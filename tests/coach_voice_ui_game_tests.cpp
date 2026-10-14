// The coach's voice model in the game (src/game/coach_model.cpp) on the headless game, without a
// download: the prompt that the title page's Coach entry opens when the voice is on and its model
// missing (its licence, "Not now" that switches the voice off and is remembered), the prompt
// opened again by switching the voice back on in Options > Audio, the Analysis mode's offer, the
// update form shown when files of the old INT8 model are in the folder, and, with every file of the
// old model there (empty files of its sizes, which do not hash right), the check made at start-up
// (no update offered), the update row of Options > Audio and its prompt ("Not now" keeps the old
// voice), and the files that did not load (offered again). The model folders are empty folders of
// the test or those fake files; no button that would start a download is ever pressed.
#include "test.h"
#include "coach_mode_support.h"
#include "core/ini.h"
#include "game/coach_model.h"
#include "game/settings.h"
#include "i18n/i18n.h"
#include "tts/model_store.h"
#include "ui/ui_internal.h"
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

std::string T(const char* key) { return i18n::tr(key); }

// The empty voice folder of the run, the title page.
const std::vector<std::string>& emptyArgs() {
    static const std::vector<std::string> args = {"--no-intro", "--coach-dir", coachtest::voiceFolder()};
    return args;
}

bool titlePage(headless::App& app) { return app.waitForLabel(T("menu.new_game"), 20000); }

// The prompt's buttons that would download: never pressed here, only looked for.
std::string downloadButton() {
    char mb[32];
    std::snprintf(mb, sizeof mb, "%.1f", double(tts::supertonicManifest().totalBytes()) / 1e6);
    return i18n::trf("coach.download.download", {mb});
}
std::string updateButton() {
    char mb[32];
    std::snprintf(mb, sizeof mb, "%.1f", double(tts::supertonicManifest().totalBytes()) / 1e6);
    return i18n::trf("coach.update.download", {mb});
}
int countLabel(const headless::App& app, const std::string& label) {
    int n = 0;
    for (const auto& it : app.items()) n += it.label == label ? 1 : 0;
    return n;
}
bool promptShown(const headless::App& app) { return app.hasLabel(T("coach.download.not_now")); }

bool savedBool(const char* key, bool def) {
    IniFile ini;
    return ini.load(game::settings().path) ? ini.getBool(key, def) : def;
}

// The Coach page from the title page, and back.
bool openCoachPage(headless::App& app) {
    return app.clickLabel(T("menu.coach")) && app.waitForLabel(T("coach.colour"), 60);
}
bool backToTitle(headless::App& app) { return app.clickLabel(T("common.back")) && titlePage(app); }

// Options > Audio from the title page.
bool openAudioOptions(headless::App& app) {
    ui::debug::setOptionsTab(2);
    return app.clickLabel(T("menu.options")) && app.waitForLabel(T("options.coach_voice"), 60);
}

// Leaves the Options page (Discard, should it ask about a change).
void leaveOptions(headless::App& app) {
    app.key(plat::KEY_ESCAPE);
    app.frames(3);
    if (app.hasLabel(T("common.discard"))) {
        app.clickLabel(T("common.discard"));
        app.frames(3);
    }
}

// A file of 'size' bytes that takes no room (sparse where the file system allows it).
bool sizedFile(const std::string& path, uint64_t size) {
    if (!coachtest::writeFile(path, std::string())) return false;
    std::error_code ec;
    std::filesystem::resize_file(std::filesystem::u8path(path), size, ec);
    return !ec;
}

// The old INT8 model's files, empty but of their sizes, in 'dir' ('bigEmptied': those of a
// megabyte or more empty).
bool fakeLegacyModel(const headless::TempDir& dir, bool bigEmptied = false) {
    bool ok = true;
    for (const tts::ManifestFile& f : tts::legacyManifest().files) {
        const uint64_t size = bigEmptied && f.size >= 1000000u ? 0u : f.size;
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::u8path(dir.file(f.name)), ec)) {
            std::filesystem::resize_file(std::filesystem::u8path(dir.file(f.name)), size, ec);
            ok = !ec && ok;
        } else {
            ok = sizedFile(dir.file(f.name), size) && ok;
        }
    }
    return ok;
}

}  // namespace

// The Coach entry offers the model once the voice is on and the model missing: the prompt over the
// Coach page (its download form: nothing of an old model here), the page blocked under it, its
// licence one click away; the offer remembered at once. "Not now" (Esc) switches the voice off,
// saved; the page opens without the prompt from then on.
TEST(coach_voice_ui_game_coach_entry_offers_the_voice) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(emptyArgs());
    REQUIRE(app);
    REQUIRE(titlePage(*app));
    game::Settings& g = game::settings();
    REQUIRE(g.coachVoice);
    REQUIRE(!g.coachVoiceOffered);
    CHECK(!game::coachModelDownloading());
    CHECK(!game::coachModelUpdateAvailable());
    CHECK(!game::coachVoiceWanted());
    CHECK(game::coachModelNeedsPrompt());
    REQUIRE(app->clickLabel(T("menu.coach")));
    REQUIRE(app->waitForLabel(T("coach.download.not_now"), 30));
    CHECK(app->hasLabel(downloadButton()));
    CHECK(!app->hasLabel(updateButton()));
    CHECK(app->hasLabel(T("coach.download.read_licence")));
    CHECK(app->hasLabel(T("coach.colour")));   // the page under the card
    CHECK(g.coachVoiceOffered);                // shown: the Analysis mode will not offer it again
    CHECK(savedBool("coach.voice_offered", false));
    // The page under it is blocked: its Back does nothing.
    REQUIRE(app->clickLabel(T("common.back")));
    CHECK(promptShown(*app));
    CHECK(app->hasLabel(T("coach.colour")));

    // The licence, and back to the prompt.
    REQUIRE(app->clickLabel(T("coach.download.read_licence")));
    app->frames(3);
    CHECK(!promptShown(*app));
    CHECK(app->hasLabel(T("coach.download.back")));
    app->key(plat::KEY_ESCAPE);
    REQUIRE(app->waitForLabel(T("coach.download.not_now"), 30));
    CHECK(g.coachVoice);

    // Esc: Not now. The voice off and saved, the page still there.
    app->key(plat::KEY_ESCAPE);
    app->frames(3);
    CHECK(!promptShown(*app));
    CHECK(!g.coachVoice);
    CHECK(!savedBool("coach.voice", true));
    CHECK(log.has("coach: voice download declined; voice off (subtitles only)"));
    CHECK(app->hasLabel(T("coach.colour")));
    CHECK(!game::coachModelNeedsPrompt());
    REQUIRE(backToTitle(*app));
    REQUIRE(openCoachPage(*app));
    app->frames(5);
    CHECK(!promptShown(*app));
    REQUIRE(backToTitle(*app));
}

// Switched back on in Options > Audio (and applied), the voice is offered again over the Options
// page; "Not now" switches it off again, and the page's box follows (nothing left to apply, Esc
// closes the page at once). The Analysis mode offers it only to a player who has never
// seen the offer, with its own voice on. The download form closes by itself when the voice is
// switched off while it is open.
TEST(coach_voice_ui_game_voice_switched_on_is_offered_again) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(emptyArgs());
    REQUIRE(app);
    REQUIRE(titlePage(*app));
    game::Settings& g = game::settings();
    g.coachVoice = false;   // as the test before leaves it, whichever ran
    app->frames(2);
    REQUIRE(openAudioOptions(*app));
    REQUIRE(app->clickLabel(T("options.coach_voice")));
    app->frames(2);
    CHECK(!promptShown(*app));   // not before Apply
    REQUIRE(app->clickLabel(T("common.apply")));
    REQUIRE(app->waitForLabel(T("coach.download.not_now"), 30));
    CHECK(g.coachVoice);
    CHECK(app->hasLabel(T("options.coach_voice")));   // over the Options page
    REQUIRE(coachtest::clickLast(*app, T("coach.download.not_now")));
    app->frames(3);
    CHECK(!promptShown(*app));
    CHECK(!g.coachVoice);
    CHECK_EQ(log.count("coach: voice download declined"), 1);
    app->key(plat::KEY_ESCAPE);
    app->frames(3);
    CHECK(!app->hasLabel(T("common.discard")));
    if (app->hasLabel(T("common.discard"))) app->clickLabel(T("common.discard"));
    REQUIRE(titlePage(*app));
    CHECK(!g.coachVoice);

    // The Analysis mode's offer: only with its voice on, to a player who never saw the prompt,
    // while the coach's voice is on and its model missing.
    g.coachVoice = true;
    g.analysisVoice = true;
    g.coachVoiceOffered = true;
    CHECK(!game::offerVoiceForAnalysis());
    g.coachVoiceOffered = false;
    g.analysisVoice = false;
    CHECK(!game::offerVoiceForAnalysis());
    g.analysisVoice = true;
    CHECK(game::offerVoiceForAnalysis());
    REQUIRE(app->waitForLabel(T("coach.download.not_now"), 30));
    CHECK(g.coachVoiceOffered);
    // The voice switched off meanwhile: the download form closes, unanswered.
    g.coachVoice = false;
    app->frames(3);
    CHECK(!promptShown(*app));
    CHECK_EQ(log.count("coach: voice download declined"), 1);
    CHECK(!game::offerVoiceForAnalysis());
    // Switched on again (as Options > Audio does): offered again, then answered.
    g.coachVoice = true;
    app->frames(2);
    REQUIRE(app->waitForLabel(T("coach.download.not_now"), 30));
    REQUIRE(app->clickLabel(T("coach.download.not_now")));
    app->frames(2);
    CHECK(!g.coachVoice);
    CHECK_EQ(log.count("coach: voice download declined"), 2);
}

// Files of the old INT8 model in the folder (a part of one here, so no model is installed): the
// voice switched on is offered in the prompt's update form, since the download would delete those
// files first. That form stays open with the voice off (the update is offered whatever the
// option); "Not now" switches the voice off (no old model speaks: there is none).
TEST(coach_voice_ui_game_old_files_show_the_update_form) {
    coachtest::VoiceFolder voice;
    coachtest::LogCapture log;
    headless::App* app = headless::shared(emptyArgs());
    REQUIRE(app);
    REQUIRE(titlePage(*app));
    game::Settings& g = game::settings();
    g.coachVoice = false;
    app->frames(2);
    headless::TempDir old("coach-voice-old-part");
    REQUIRE(coachtest::writeFile(old.file("vocoder.int8.onnx.part"), "partial"));
    tts::setModelFolder(old.path);
    REQUIRE(tts::legacyFilesPresent());
    REQUIRE(tts::installedModel() == tts::ModelKind::None);
    g.coachVoice = true;   // as Options > Audio does once applied
    REQUIRE(app->waitForLabel(T("coach.download.not_now"), 30));
    CHECK(app->hasLabel(updateButton()));
    CHECK(!app->hasLabel(downloadButton()));
    g.coachVoice = false;
    app->frames(3);
    CHECK(promptShown(*app));
    g.coachVoice = true;
    app->key(plat::KEY_ESCAPE);
    app->frames(2);
    CHECK(!promptShown(*app));
    CHECK(!g.coachVoice);
    CHECK(log.has("coach: voice download declined; voice off (subtitles only)"));
    CHECK(!log.has("old INT8 model stays"));
    // The voice off: the Coach entry offers nothing.
    REQUIRE(openCoachPage(*app));
    app->frames(5);
    CHECK(!promptShown(*app));
    REQUIRE(backToTitle(*app));
    tts::setModelFolder(coachtest::voiceFolder());
}

// Every file of the old model there with its size (empty files: they do not hash right). The scene
// sees an old model installed and checks its files on a thread of its own as it starts: they do not
// check out, so the update is not offered on the title page. Options > Audio shows the update row
// (Voice quality on, a model being there), whose button opens the prompt's update form; "Not now"
// there keeps the old voice. The Coach entry does not offer anything (a model is there), until the
// game reports that the files did not load: then it offers the download, and "Not now" switches
// the voice off. Reported twice, it is reported once.
TEST(coach_voice_ui_game_old_model_installed) {
    headless::TempDir folder("coach-voice-legacy");
    // However the test ends: the scene goes before its folder, the default folder back.
    struct SceneGone {
        ~SceneGone() {
            headless::stopShared();
            tts::setModelFolder(std::string());
        }
    } gone;
    REQUIRE(fakeLegacyModel(folder));
    REQUIRE(tts::installedModel(folder.path) == tts::ModelKind::Legacy);
    coachtest::LogCapture log;
    headless::App* app = headless::shared({"--no-intro", "--coach-dir", folder.path});
    REQUIRE(app);
    CHECK(log.has("coach: voice on, model legacy INT8 in"));
    // The check of the old files runs on its thread from the scene's start. The four large files
    // are emptied meanwhile, so that it hashes a few bytes rather than 145 MB (seconds); whether it
    // finds them wrong or missing, they do not check out.
    fakeLegacyModel(folder, true);   // best effort: where a file cannot be emptied, the check takes longer
    REQUIRE(coachtest::waitEngine(*app, [&] { return log.has("coach: old INT8 voice model in"); }, 60.0));
    CHECK(!log.has("coach: old INT8 voice model in " + folder.path + ": ready"));
    REQUIRE(fakeLegacyModel(folder));
    CHECK(game::coachModelUpdateAvailable());
    CHECK(game::coachVoiceWanted());
    CHECK(!game::coachModelNeedsPrompt());
    REQUIRE(titlePage(*app));
    app->frames(5);
    CHECK(!promptShown(*app));
    CHECK(!log.has("update of the old INT8 voice model is offered"));
    game::Settings& g = game::settings();
    CHECK(!g.coachVoiceUpdateOffered);

    // Options > Audio: the update row, Voice quality on.
    REQUIRE(openAudioOptions(*app));
    REQUIRE(app->waitForLabel(updateButton(), 10));
    CHECK_EQ(countLabel(*app, updateButton()), 1);
    CHECK(!coachtest::disabled(app->find(T("options.voice_quality"))));
    REQUIRE(app->clickLabel(updateButton()));   // the row's button: it opens the prompt
    REQUIRE(app->waitForLabel(T("coach.download.not_now"), 30));
    CHECK_EQ(countLabel(*app, updateButton()), 2);   // the row's, blocked, and the prompt's
    REQUIRE(app->clickLabel(T("coach.download.not_now")));
    app->frames(3);
    CHECK(!promptShown(*app));
    CHECK(log.has("coach: voice model update declined; the old INT8 model stays"));
    CHECK(g.coachVoice);
    // The row's hook opens it too (the game's update call).
    ui::detail::openVoiceUpdate();
    REQUIRE(app->waitForLabel(T("coach.download.not_now"), 30));
    app->key(plat::KEY_ESCAPE);
    app->frames(3);
    CHECK(!promptShown(*app));
    CHECK_EQ(log.count("the old INT8 model stays"), 2);
    CHECK(g.coachVoice);
    leaveOptions(*app);
    REQUIRE(titlePage(*app));

    // The Coach entry: nothing to offer.
    REQUIRE(openCoachPage(*app));
    app->frames(5);
    CHECK(!promptShown(*app));
    REQUIRE(backToTitle(*app));
    // The files did not load: the next Coach entry offers the download, whose "Not now" switches
    // the voice off this time.
    game::coachModelLoadFailed();
    game::coachModelLoadFailed();
    CHECK_EQ(log.count("did not load although its files are there"), 1);
    CHECK(game::coachModelNeedsPrompt());
    REQUIRE(app->clickLabel(T("menu.coach")));
    REQUIRE(app->waitForLabel(T("coach.download.not_now"), 30));
    CHECK(app->hasLabel(updateButton()));
    REQUIRE(app->clickLabel(T("coach.download.not_now")));
    app->frames(3);
    CHECK(!g.coachVoice);
    CHECK(log.has("coach: voice download declined; voice off (subtitles only)"));
    REQUIRE(backToTitle(*app));
    // The scene goes (before its folder): everything forgotten, the default folder back.
    headless::stopShared();
    tts::setModelFolder(std::string());
    CHECK(!game::coachModelUpdateAvailable());
}
