// The coach's voice model download in the game (src/game/coach_model.cpp) from the prompt's
// Download (or Update) button to its end, with the progress panel: drawn frame by frame
// (tests/ui_driver.h) through game::drawModelDownload as the scenes call it, and once in the
// headless game itself. No test reaches the model's known URLs: the job gets either a manifest
// without any source (it fails at once, as the whole run's does: tests/test_main.cpp) or a
// made-up model of three small files served by a loopback server (tests/http_fake.h), which can
// hold a file back until the test lets it go (the job is then fetching), or send other bytes of
// the right size (the file does not hash right). The model folders are folders of the test; no
// voice is loaded (the made-up files are not the model the voice reads).
#include "test.h"
#include "coach_mode_support.h"
#include "http_fake.h"
#include "ui_driver.h"
#include "game/coach_model.h"
#include "game/settings.h"
#include "i18n/i18n.h"
#include "tts/model_store.h"
#include "ui/ui_internal.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using ui::ModelProgressView;

namespace {

std::string T(const char* key) { return i18n::tr(key); }

// ---- The made-up model ----
constexpr int kFiles = 3;
const char* const kNames[kFiles] = {"voice-a.json", "voice-b.onnx", "voice-c.onnx"};
const char* const kRemote[kFiles] = {"voice-a.json", "onnx/voice-b.onnx", "onnx/voice-c.onnx"};
const uint64_t kSizes[kFiles] = {700, 400000, 200000};
// Their SHA-256 (computed by Python's hashlib from the generator below).
const char* const kHashes[kFiles] = {"cffb91be823b7f54cad8e34616c7304cab417761192981026370dfee88b91ed1",
                                     "9128c8fd0d756174846cbcea3e48a3c5777c0dd6c76ac159bab5516998dd5573",
                                     "d80da6436835ea0da5267997638e2fe76b448e677941e105510d6833342e0659"};
const char kRepo[] = "/dl/resolve/r1/";
const char kLabel[] = "127.0.0.1/dl";

// bytes(((j % (89 + i)) + i * 5) & 0xff for j in range(size))
std::string content(int i) {
    std::string s(size_t(kSizes[i]), '\0');
    for (size_t j = 0; j < s.size(); ++j) s[j] = char(((j % size_t(89 + i)) + size_t(i) * 5) & 0xff);
    return s;
}

// The made-up model without a source: its download fails at once ("unavailable").
tts::ModelManifest sourceless() {
    tts::ModelManifest m;
    for (int i = 0; i < kFiles; ++i) m.files.push_back({kNames[i], kSizes[i], kHashes[i], kRemote[i]});
    return m;
}

// A loopback server with the made-up model at http://127.0.0.1:<port>/dl/resolve/r1/<path>, a
// thread per connection. One file can be held back (its request waits until release(), or 10 s),
// one sent with other bytes.
struct FakeRepo {
    const std::vector<std::string> files = {content(0), content(1), content(2)};
    std::mutex mu;
    std::condition_variable cv;
    int held = -1;                  // the file whose request waits
    bool open = false;
    std::atomic<bool> holding{false};   // a request is waiting
    std::atomic<int> wrong{-1};     // the file sent with other bytes (its size kept)
    std::atomic<int> served{0};     // the files' requests answered or held
    fakehttp::Server srv;           // last: its thread starts once the rest is there

    FakeRepo()
        : srv([this](const fakehttp::Request& r) {
              for (int i = 0; i < kFiles; ++i) {
                  if (r.path != std::string(kRepo) + kRemote[i]) continue;
                  ++served;
                  if (i == heldFile()) {
                      holding = true;
                      std::unique_lock<std::mutex> lk(mu);
                      cv.wait_for(lk, std::chrono::seconds(10), [this] { return open; });
                  }
                  std::string data = files[size_t(i)];
                  if (i == wrong) data[data.size() / 2] = char(data[data.size() / 2] ^ 0x5a);
                  return fakehttp::fileReply(data, r);
              }
              fakehttp::Reply rep;
              rep.status = 404;
              return rep;
          },
          true) {}
    // The held request goes on before the server stops.
    ~FakeRepo() { release(); }

    int heldFile() {
        std::lock_guard<std::mutex> lk(mu);
        return held;
    }
    void hold(int file) {
        std::lock_guard<std::mutex> lk(mu);
        held = file;
        open = false;
        holding = false;
    }
    void release() {
        {
            std::lock_guard<std::mutex> lk(mu);
            open = true;
            held = -1;
        }
        cv.notify_all();
    }
    tts::ModelManifest manifest() const {
        tts::ModelManifest m = sourceless();
        m.sources.push_back({"http://127.0.0.1:" + std::to_string(srv.port()) + kRepo, kLabel});
        return m;
    }
};

// Whatever a test changed, back as it was when it ends: a job still running cancelled and joined
// (coachModelShutdown, which also forgets the rest), the job's previous manifest (the run's,
// without a source), the default model folder, the settings, the language, the hooks that
// game::coachModelInit set.
struct Restore {
    game::Settings settings = game::settings();
    const tts::ModelManifest* manifest;
    explicit Restore(const tts::ModelManifest* m) : manifest(game::setCoachModelManifest(m)) {}
    ~Restore() {
        game::coachModelShutdown();
        game::setCoachModelManifest(manifest);
        tts::setModelFolder(std::string());
        i18n::setLanguage("en");
        game::settings() = settings;
        game::settings().save();
        ui::setCoachEntryHook(nullptr);
        ui::setVoiceUpdateHooks(nullptr, nullptr);
        game::setVoiceReleaseHook(nullptr);
    }
};

// The prompt's buttons that start the job (the real model's size on them, as the prompt writes it).
std::string megabytes() {
    char mb[32];
    std::snprintf(mb, sizeof mb, "%.1f", double(tts::supertonicManifest().totalBytes()) / 1e6);
    return mb;
}
std::string downloadButton() { return i18n::trf("coach.download.download", {megabytes()}); }
std::string updateButton() { return i18n::trf("coach.update.download", {megabytes()}); }

// The game's prompt and panel, every frame.
void attach(uitest::Driver& d) {
    d.screen = [] { game::drawModelDownload(false); };
}

// Frames until 'done' (the job's thread works at its own pace): bounded by the wall clock.
template <class F>
bool until(uitest::Driver& d, F done, double seconds = 10.0) {
    const auto start = std::chrono::steady_clock::now();
    while (!done()) {
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > seconds) return false;
        d.frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Opens the prompt (as the Coach entry does) and presses 'button' on it.
bool pressOnPrompt(uitest::Driver& d, const std::string& button) {
    game::openModelPrompt();
    return d.waitFor(button, 10) && d.click(button);
}

// The job ended and the game took its end (the next frames).
bool jobEnded(uitest::Driver& d) {
    if (!until(d, [] { return !game::coachModelDownloading(); })) return false;
    d.frames(2);
    return true;
}

// Where the failure panel puts its Close button for this reason (the panel grows with the lines
// of the reason): the panel drawn alone with it, then the game's frames again.
float closeRowFor(uitest::Driver& d, const std::string& reason) {
    const std::function<void()> scene = d.screen;
    ModelProgressView v;
    v.state = ModelProgressView::State::Failed;
    v.error = reason;
    d.screen = [&v] { ui::modelProgressPanel(v); };
    d.frames(2);
    const ui::im::TracedItem* close = d.find(T("coach.download.close"));
    const float y = close ? close->r.y : -1.0f;
    d.screen = scene;
    d.frames(2);
    return y;
}
float closeRow(const uitest::Driver& d) {
    const ui::im::TracedItem* close = d.find(T("coach.download.close"));
    return close ? close->r.y : -2.0f;
}

bool panelFailed(const uitest::Driver& d) {
    return d.has(T("coach.download.close")) && d.has(T("coach.download.retry")) && !d.has(T("coach.download.cancel"));
}
bool panelWorking(const uitest::Driver& d) {
    return d.has(T("coach.download.cancel")) && !d.has(T("coach.download.retry"));
}
bool panelGone(const uitest::Driver& d) {
    return !d.has(T("coach.download.cancel")) && !d.has(T("coach.download.retry")) && !d.has(T("coach.download.close"));
}

}  // namespace

// Nowhere to download from: the prompt's Download starts the job, which fails at once; the panel
// shows the failure with the network reason, Retry starts it again (and it fails again), Close
// puts the panel away. A folder that cannot be made fails with the disk reason, which names it.
// Nothing installed either way.
TEST(coach_voice_ui_download_fails_without_a_source) {
    REQUIRE(uitest::app());
    const tts::ModelManifest none = sourceless();
    headless::TempDir folder("coach-dl-none");
    Restore restore(&none);
    tts::setModelFolder(folder.path);
    game::settings().coachVoice = true;
    coachtest::LogCapture log;
    uitest::Driver d;
    attach(d);
    d.frames(2);
    CHECK(log.has("coach: voice on, model none in " + folder.path));
    CHECK(panelGone(d));

    REQUIRE(pressOnPrompt(d, downloadButton()));
    CHECK(log.has("coach: voice model download started into " + folder.path));
    REQUIRE(jobEnded(d));
    CHECK(log.has("tts: voice model download failed: unavailable (no source to download from)"));
    REQUIRE(panelFailed(d));
    CHECK(!d.has(downloadButton()));   // the prompt answered
    const float network = closeRowFor(d, T("coach.download.error.network"));
    CHECK(network > 0.0f);
    CHECK(closeRow(d) == network);
    CHECK(!game::coachModelInstalled());

    // Retry: the job again, the same end.
    REQUIRE(d.click(T("coach.download.retry")));
    REQUIRE(jobEnded(d));
    CHECK_EQ(log.count("coach: voice model download started"), 2);
    CHECK_EQ(log.count("download failed: unavailable"), 2);
    REQUIRE(panelFailed(d));
    CHECK(closeRow(d) == network);
    // Close: the panel goes, nothing else happens.
    REQUIRE(d.click(T("coach.download.close")));
    d.wait(1.0f);
    CHECK(panelGone(d));
    CHECK_EQ(log.count("coach: voice model download started"), 2);

    // A model folder under a file: it cannot be made (the disk reason, with the folder in it).
    REQUIRE(coachtest::writeFile(folder.file("plain"), "a file"));
    const std::string blocked = folder.file("plain") + "/voice/";
    tts::setModelFolder(blocked);
    REQUIRE(pressOnPrompt(d, downloadButton()));
    REQUIRE(jobEnded(d));
    CHECK(log.has("tts: voice model download failed: io (cannot create " + blocked + ")"));
    REQUIRE(panelFailed(d));
    const float disk = closeRowFor(d, i18n::trf("coach.download.error.disk", {i18n::ltr(blocked)}));
    CHECK(disk > network);   // the folder makes it longer
    CHECK(closeRow(d) == disk);
    REQUIRE(d.click(T("coach.download.close")));
    d.wait(1.0f);
    CHECK(panelGone(d));
    CHECK(!game::coachModelInstalled());
}

// The made-up model from the loopback server. While a file is held back the job is fetching: the
// panel shows its progress with Cancel, Options > Audio's update row knows it runs, and the
// prompt's Download starts nothing more. Let go, the job ends: the model is ready, said once to the
// game with the files it fetched. A second job with a file of the old INT8 model in the folder
// (the prompt's update form) first lets the voice go (the release hook), removes the old file and
// checks the made-up files in place: nothing fetched.
TEST(coach_voice_ui_download_from_a_loopback_server) {
    REQUIRE(uitest::app());
    FakeRepo repo;
    REQUIRE(repo.srv.ok());
    const tts::ModelManifest served = repo.manifest();
    headless::TempDir folder("coach-dl-served");
    Restore restore(&served);
    tts::setModelFolder(folder.path);
    game::settings().coachVoice = true;
    coachtest::LogCapture log;
    uitest::Driver d;
    attach(d);
    d.frames(2);
    int released = 0;
    game::setVoiceReleaseHook([&released] { ++released; });

    repo.hold(1);
    REQUIRE(pressOnPrompt(d, downloadButton()));
    REQUIRE(until(d, [&repo] { return repo.holding.load(); }));
    d.frames(3);
    CHECK(game::coachModelDownloading());
    CHECK(panelWorking(d));
    CHECK(coachtest::readFile(folder.file(kNames[0])) == repo.files[0]);   // the first file is in
    const ui::VoiceUpdateRow row = ui::detail::voiceUpdateRow();
    CHECK(row.running);
    CHECK(!row.show);            // nothing old to update
    CHECK(!row.modelInstalled);
    CHECK(!game::coachModelNeedsPrompt());   // no offer while it runs
    CHECK(!game::coachVoiceWanted());
    // The prompt opened anyway (by its function): its Download starts no second job.
    REQUIRE(pressOnPrompt(d, downloadButton()));
    d.frames(2);
    CHECK_EQ(log.count("coach: voice model download started"), 1);
    CHECK(game::coachModelDownloading());

    repo.release();
    REQUIRE(jobEnded(d));
    CHECK(log.has("coach: the voice model is ready (" + std::string(kLabel) + ")"));
    CHECK_EQ(repo.served.load(), kFiles);
    for (int i = 0; i < kFiles; ++i) CHECK(coachtest::readFile(folder.file(kNames[i])) == repo.files[size_t(i)]);
    CHECK(coachtest::readFile(folder.file("README.txt")).find("https://" + std::string(kLabel)) != std::string::npos);
    int fetched = -1;
    CHECK(game::coachModelInstalled(&fetched));
    CHECK_EQ(fetched, kFiles);
    CHECK(!game::coachModelInstalled(&fetched));   // said once
    CHECK_EQ(fetched, 0);
    d.wait(1.0f);
    CHECK(panelGone(d));
    CHECK_EQ(released, 0);   // nothing old in the folder: the voice was not stopped

    // A part of the old INT8 model: the update form; the voice let go, the old file removed, the
    // files checked where they are.
    REQUIRE(coachtest::writeFile(folder.file("vocoder.int8.onnx.part"), "old"));
    REQUIRE(tts::legacyFilesPresent());
    REQUIRE(pressOnPrompt(d, updateButton()));
    CHECK_EQ(released, 1);
    REQUIRE(jobEnded(d));
    CHECK(log.has("coach: the voice model is ready (checked in place, the old INT8 model deleted)"));
    CHECK(!tts::legacyFilesPresent());
    CHECK_EQ(repo.served.load(), kFiles);   // nothing asked of the server
    CHECK(game::coachModelInstalled(&fetched));
    CHECK_EQ(fetched, 0);
    CHECK_EQ(log.count("coach: voice model download started"), 2);
}

// Cancel on the panel while a file is held back: the job stops, nothing installed. The next job
// keeps the file already there, and fails on a file sent with other bytes (the server's reason,
// with "hash" in it); Retry once the server sends it right fetches only the files still missing.
// The scene going (coachModelShutdown) while a job runs cancels and joins it.
TEST(coach_voice_ui_download_cancelled_and_refused) {
    REQUIRE(uitest::app());
    FakeRepo repo;
    REQUIRE(repo.srv.ok());
    const tts::ModelManifest served = repo.manifest();
    headless::TempDir folder("coach-dl-cancel");
    Restore restore(&served);
    tts::setModelFolder(folder.path);
    game::settings().coachVoice = true;
    coachtest::LogCapture log;
    uitest::Driver d;
    attach(d);
    d.frames(2);

    repo.hold(1);
    REQUIRE(pressOnPrompt(d, downloadButton()));
    REQUIRE(until(d, [&repo] { return repo.holding.load(); }));
    d.frames(3);
    REQUIRE(panelWorking(d));
    REQUIRE(d.click(T("coach.download.cancel")));
    REQUIRE(jobEnded(d));
    CHECK(log.has("tts: voice model download cancelled"));
    CHECK(!game::coachModelInstalled());
    CHECK(!log.has("coach: the voice model is ready"));
    d.wait(1.0f);
    CHECK(panelGone(d));
    repo.release();

    // The last file sent with other bytes: the job fails on it.
    repo.wrong = 2;
    const int before = repo.served;
    REQUIRE(pressOnPrompt(d, downloadButton()));
    REQUIRE(jobEnded(d));
    CHECK_EQ(repo.served - before, 2);   // the first file kept
    CHECK(log.has("tts: voice model download failed: hash (" + std::string(kNames[2]) + " from " + kLabel));
    REQUIRE(panelFailed(d));
    const float server = closeRowFor(d, i18n::trf("coach.download.error.server", {i18n::ltr("hash")}));
    CHECK(server > 0.0f);
    CHECK(closeRow(d) == server);
    CHECK(!coachtest::readFile(folder.file(kNames[1])).empty());
    CHECK(coachtest::filesIn(folder.path, ".part").empty());
    CHECK(!game::coachModelInstalled());

    // Sent right, Retry: only that file.
    repo.wrong = -1;
    REQUIRE(d.click(T("coach.download.retry")));
    REQUIRE(jobEnded(d));
    CHECK_EQ(repo.served - before, 3);
    int fetched = -1;
    CHECK(game::coachModelInstalled(&fetched));
    CHECK_EQ(fetched, 1);

    // A job running as the scene goes: cancelled and joined.
    for (int i = 0; i < kFiles; ++i) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(folder.file(kNames[i])), ec);
    }
    repo.hold(1);
    REQUIRE(pressOnPrompt(d, downloadButton()));
    REQUIRE(until(d, [&repo] { return repo.holding.load(); }));
    CHECK(game::coachModelDownloading());
    const int cancelled = log.count("tts: voice model download cancelled");
    game::coachModelShutdown();
    CHECK(!game::coachModelDownloading());
    CHECK_EQ(log.count("tts: voice model download cancelled"), cancelled + 1);
    CHECK(!game::coachModelInstalled());
    repo.release();
}

// In the headless game: the Coach entry's prompt, its Download, the panel over the Coach page and
// the job's end taken by the scene (GameScene::coachModelDownloaded: the files are not the model
// the voice reads, so the voice stays off).
TEST(coach_voice_ui_download_in_the_game) {
    FakeRepo repo;
    REQUIRE(repo.srv.ok());
    const tts::ModelManifest served = repo.manifest();
    headless::TempDir folder("coach-dl-game");
    coachtest::LogCapture log;
    struct Scene {
        const tts::ModelManifest* manifest;
        explicit Scene(const tts::ModelManifest* m) : manifest(game::setCoachModelManifest(m)) {}
        ~Scene() {
            headless::stopShared();   // its job (if any) cancelled and joined, its folder forgotten
            game::setCoachModelManifest(manifest);
        }
    } scene(&served);
    headless::App* app = headless::shared({"--no-intro", "--coach-dir", folder.path});
    REQUIRE(app);
    REQUIRE(app->waitForLabel(T("menu.new_game"), 20000));
    REQUIRE(app->clickLabel(T("menu.coach")));
    REQUIRE(app->waitForLabel(downloadButton(), 30));
    REQUIRE(app->clickLabel(downloadButton()));
    REQUIRE(coachtest::waitEngine(*app, [&log] { return log.has("coach: the voice model is ready"); }, 10.0));
    app->frames(3);
    CHECK(log.has("coach: the voice model is ready (" + std::string(kLabel) + ")"));
    CHECK(!game::coachModelInstalled());   // taken by the scene
    CHECK(!game::coachVoiceWanted());
    CHECK(app->hasLabel(T("coach.colour")));   // the Coach page under it
    for (int i = 0; i < kFiles; ++i) CHECK(coachtest::readFile(folder.file(kNames[i])) == repo.files[size_t(i)]);
}
