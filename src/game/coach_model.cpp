// The coach's voice model in the game (see coach_model.h).
#include "coach_model.h"
#include "settings.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "../tts/model_store.h"
#include "../tts/tts.h"
#include "../ui/ui.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

namespace game {
namespace {

using Phase = tts::DownloadProgress::Phase;

// The start-up check of the old INT8 model: every file hashed on its own thread (a second or so),
// so that the update is offered only for the genuine old files.
struct LegacyCheck {
    std::thread thread;
    std::atomic<bool> done{false}, verified{false}, cancel{false};
    ~LegacyCheck() {
        cancel = true;
        if (thread.joinable()) thread.join();
    }
};

struct ModelUi {
    bool inited = false;
    bool promptOpen = false;
    bool promptUpdate = false;     // the prompt shows in its update form
    bool legacyInstalled = false;  // the old INT8 model is there and the official one is not (sizes)
    bool anyInstalled = false;     // either model is there (sizes): the voice quality applies
    std::unique_ptr<LegacyCheck> legacyCheck;
    std::function<void()> releaseHook;
    bool lastVoice = true;         // [coach] voice as last seen (switched on: offer the download)
    bool installed = false;        // coachModelInstalled() not yet read
    int fetched = 0;               // the files the last finished download wrote
    bool suspect = false;          // the worker could not load files that looked complete
    bool checked = false;          // a download job checked every file this session
    bool watching = false;         // a job's end has not been handled yet
    bool failed = false;           // the panel shows the failure (until Close or Retry)
    std::string failure;           // its reason, translated
    std::unique_ptr<tts::ModelDownloader> job;
};

ModelUi& state() {
    static ModelUi u;
    return u;
}

// What the folder holds, again (start-up, the end of a job).
void refreshInstalled() {
    ModelUi& u = state();
    const tts::ModelKind kind = tts::installedModel();
    u.legacyInstalled = kind == tts::ModelKind::Legacy;
    u.anyInstalled = kind != tts::ModelKind::None;
}

void startLegacyCheck() {
    ModelUi& u = state();
    if (!u.legacyInstalled || u.legacyCheck) return;
    u.legacyCheck.reset(new LegacyCheck());
    LegacyCheck* c = u.legacyCheck.get();
    const std::string folder = tts::modelFolder();
    c->thread = std::thread([c, folder] {
        tts::ModelStatus st = tts::verifyModel(tts::legacyManifest(), folder, nullptr, &c->cancel);
        c->verified = st == tts::ModelStatus::Ready;
        c->done = true;
        if (!c->cancel) LOGI("coach: old INT8 voice model in %s: %s", folder.c_str(), tts::statusName(st));
    });
}

void openPrompt(bool update) {
    ModelUi& u = state();
    u.promptOpen = true;
    u.promptUpdate = update;
}

void startDownload() {
    ModelUi& u = state();
    if (u.job && u.job->running()) return;
    // The job deletes the old model's files first: whatever maps them lets them go now.
    if (tts::legacyFilesPresent()) {
        u.legacyCheck.reset();   // it reads them too
        if (u.releaseHook) u.releaseHook();
    }
    u.job.reset(new tts::ModelDownloader());   // the folder as it is now (--coach-dir)
    u.failed = false;
    u.failure.clear();
    // Testing aid: SCACELITH_COACH_SOURCE=archive starts with Supertone's archive copy (the
    // fallback path), =official never falls back to it.
    tts::ModelDownloader::Options o;
    if (const char* src = std::getenv("SCACELITH_COACH_SOURCE")) {
        if (std::strcmp(src, "archive") == 0) o.firstSource = 1;
        if (std::strcmp(src, "official") == 0) o.fallback = false;
    }
    if (u.job->start(o)) {
        u.watching = true;
        LOGI("coach: voice model download started into %s", u.job->folder().c_str());
    }
}

// The reason of a failure, for the player.
std::string failureText(const tts::DownloadProgress& p, const std::string& folder) {
    const std::string& e = p.error;
    if (e == "network" || e == "timeout" || e == "truncated" || e == "unavailable") return i18n::tr("coach.download.error.network");
    if (e == "io") return i18n::trf("coach.download.error.disk", {i18n::ltr(folder)});
    return i18n::trf("coach.download.error.server", {i18n::ltr(e)});
}

// The end of a job, once.
void finishJob(const tts::DownloadProgress& p) {
    ModelUi& u = state();
    u.watching = false;
    refreshInstalled();
    switch (p.phase) {
        case Phase::Done:
            u.installed = true;
            u.fetched = p.fetched;
            u.checked = true;
            u.suspect = false;
            ui::notify(i18n::tr("coach.download.done"), 5.0f);
            LOGI("coach: the voice model is ready (%s%s)", p.sourceLabel.empty() ? "checked in place" : p.sourceLabel.c_str(),
                 p.removedLegacy ? ", the old INT8 model deleted" : "");
            break;
        case Phase::Cancelled:
            ui::notify(i18n::tr("coach.download.cancelled"), 4.0f);
            break;
        default:
            u.failed = true;
            u.failure = failureText(p, u.job ? u.job->folder() : tts::modelFolder());
            break;
    }
}

ui::ModelProgressView progressView() {
    ModelUi& u = state();
    ui::ModelProgressView v;
    if (u.failed) {
        v.state = ui::ModelProgressView::State::Failed;
        v.error = u.failure;
        return v;
    }
    if (!u.job || !u.watching) return v;   // Hidden
    tts::DownloadProgress p = u.job->progress();
    v.done = double(p.done);
    v.total = double(p.total);
    v.archiveCopy = p.sourceIndex > 0;
    v.sourceLabel = p.sourceLabel.empty() ? tts::supertonicManifest().sources.front().label : p.sourceLabel;
    v.file = p.file;
    switch (p.phase) {
        case Phase::Fetching: v.state = ui::ModelProgressView::State::Downloading; break;
        case Phase::Removing: v.state = ui::ModelProgressView::State::Removing; break;
        case Phase::Idle:
        case Phase::Checking:
        case Phase::Verifying: v.state = ui::ModelProgressView::State::Checking; break;
        default: break;   // finished: handled by drawModelDownload, hidden meanwhile
    }
    return v;
}

}  // namespace

void coachModelInit() {
    ModelUi& u = state();
    if (u.inited) return;
    u.inited = true;
    u.lastVoice = settings().coachVoice;
    ui::setCoachEntryHook([] {
        if (coachModelNeedsPrompt()) openModelPrompt();
    });
    ui::setVoiceUpdateHooks(
        [] {
            ui::VoiceUpdateRow r;
            r.show = coachModelUpdateAvailable() || (coachModelDownloading() && state().promptUpdate);
            r.running = coachModelDownloading();
            r.bytes = double(tts::supertonicManifest().totalBytes());
            r.modelInstalled = state().anyInstalled && !coachModelDownloading();
            return r;
        },
        [] { openModelUpdatePrompt(); });
    refreshInstalled();
    LOGI("coach: voice %s, model %s in %s", settings().coachVoice ? "on" : "off (subtitles only)",
         tts::kindName(tts::installedModel()), tts::modelFolder().c_str());
    // The old model: the update is offered once, after its files checked out.
    if (!settings().coachVoiceUpdateOffered) startLegacyCheck();
}

void setVoiceReleaseHook(std::function<void()> hook) { state().releaseHook = std::move(hook); }

bool coachModelUpdateAvailable() { return state().legacyInstalled && !coachModelDownloading(); }

void openModelUpdatePrompt() { openPrompt(true); }

bool coachModelDownloading() {
    ModelUi& u = state();
    return u.job && u.job->running();
}

bool coachModelNeedsPrompt() {
    ModelUi& u = state();
    if (!settings().coachVoice || coachModelDownloading()) return false;
    if (u.suspect) return true;
    return !tts::modelFilesPresent();
}

// Old files in the folder (an old model that failed to load, a part of one): the update form.
void openModelPrompt() { openPrompt(tts::legacyFilesPresent()); }

bool offerVoiceForAnalysis() {
    if (!settings().analysisVoice || settings().coachVoiceOffered || !coachModelNeedsPrompt()) return false;
    openModelPrompt();
    return true;
}

bool coachModelInstalled(int* fetched) {
    ModelUi& u = state();
    bool r = u.installed;
    u.installed = false;
    if (fetched) *fetched = r ? u.fetched : 0;
    return r;
}

bool coachVoiceWanted() { return settings().coachVoice && !coachModelDownloading() && tts::modelFilesPresent(); }

void coachModelLoadFailed() {
    ModelUi& u = state();
    // After a download that checked every file the files are not the cause: no new offer.
    if (u.checked || u.suspect) return;
    u.suspect = true;
    LOGW("coach: the voice model did not load although its files are there; the download will check them");
}

void coachModelShutdown() {
    ModelUi& u = state();
    if (u.job) {
        u.job->cancel();
        u.job->wait();
    }
    u.job.reset();
    u.watching = false;
    u.legacyCheck.reset();
    u.releaseHook = nullptr;   // its owner is going
}

void drawModelDownload(bool startupOffer) {
    ModelUi& u = state();
    if (!u.inited) coachModelInit();
    // Options > Audio > Coach voice switched on (and applied): offer the model when it is missing.
    bool voice = settings().coachVoice;
    if (voice && !u.lastVoice && coachModelNeedsPrompt()) u.promptOpen = true;
    u.lastVoice = voice;
    if (!voice && !u.promptUpdate) u.promptOpen = false;   // the update is offered with the voice off too

    if (u.job && u.watching) {
        tts::DownloadProgress p = u.job->progress();
        if (p.finished() && !u.job->running()) finishJob(p);
    }
    switch (ui::modelProgressPanel(progressView())) {
        case ui::ModelPanelAction::Cancel:
            if (u.job) u.job->cancel();
            break;
        case ui::ModelPanelAction::Retry: startDownload(); break;
        case ui::ModelPanelAction::Close:
            u.failed = false;
            u.failure.clear();
            break;
        case ui::ModelPanelAction::None: break;
    }

    // The old model checked out at start-up: the update is offered, once, on the title page.
    if (startupOffer && u.legacyCheck && u.legacyCheck->done) {
        bool offer = u.legacyCheck->verified && u.legacyInstalled && !coachModelDownloading() && !u.promptOpen &&
                     !settings().coachVoiceUpdateOffered;
        u.legacyCheck.reset();
        if (offer) {
            settings().coachVoiceUpdateOffered = true;
            settings().save();
            openPrompt(true);
            LOGI("coach: the update of the old INT8 voice model is offered");
        }
    }

    if (u.promptOpen && !settings().coachVoiceOffered) {
        // Remembered: the Analysis mode offers the voice only to a player who never saw this.
        settings().coachVoiceOffered = true;
        settings().save();
    }
    if (u.promptOpen) {
        ui::ModelPrompt mp;
        mp.bytes = double(tts::supertonicManifest().totalBytes());
        mp.folder = tts::modelFolder();
        mp.update = u.promptUpdate;
        mp.oldBytes = double(tts::legacyManifest().totalBytes());
        switch (ui::modelPrompt(mp)) {
            case ui::ModelPromptAction::Download:
                u.promptOpen = false;
                startDownload();
                break;
            case ui::ModelPromptAction::NotNow:
                u.promptOpen = false;
                // The old model still speaks: nothing changes, the update stays in Options > Audio.
                if (u.promptUpdate && u.legacyInstalled && !u.suspect) {
                    LOGI("coach: voice model update declined; the old INT8 model stays");
                    break;
                }
                // Remembered: the coach speaks through subtitles until the voice is switched on again.
                settings().coachVoice = false;
                u.lastVoice = false;
                settings().save();
                LOGI("coach: voice download declined; voice off (subtitles only)");
                break;
            case ui::ModelPromptAction::None: break;
        }
    }
}

}  // namespace game
