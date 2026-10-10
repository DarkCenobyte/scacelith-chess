// The coach's voice model in the game (see coach_model.h).
#include "coach_model.h"
#include "settings.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "../tts/model_store.h"
#include "../tts/tts.h"
#include "../ui/ui.h"
#include <cstdlib>
#include <cstring>
#include <memory>

namespace game {
namespace {

using Phase = tts::DownloadProgress::Phase;

struct ModelUi {
    bool inited = false;
    bool promptOpen = false;
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

void startDownload() {
    ModelUi& u = state();
    if (u.job && u.job->running()) return;
    u.job.reset(new tts::ModelDownloader());   // the folder as it is now (--coach-dir)
    u.failed = false;
    u.failure.clear();
    // Testing aid: SCACELITH_COACH_SOURCE=github skips Hugging Face (the fallback path),
    // =hub never falls back to the release archive.
    tts::ModelDownloader::Options o;
    if (const char* src = std::getenv("SCACELITH_COACH_SOURCE")) {
        if (std::strcmp(src, "github") == 0) o.useHub = false;
        if (std::strcmp(src, "hub") == 0) o.useArchive = false;
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
    switch (p.phase) {
        case Phase::Done:
            u.installed = true;
            u.fetched = p.fetched;
            u.checked = true;
            u.suspect = false;
            ui::notify(i18n::tr("coach.download.done"), 5.0f);
            LOGI("coach: the voice model is ready (%s)", p.fromArchive ? "from the release archive" : "from the hub");
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
    v.github = p.fromArchive;
    v.sourceLabel = p.sourceLabel.empty() ? tts::supertonicManifest().hubLabel : p.sourceLabel;
    v.file = p.file;
    switch (p.phase) {
        case Phase::Hub:
        case Phase::Archive: v.state = ui::ModelProgressView::State::Downloading; break;
        case Phase::Extracting: v.state = ui::ModelProgressView::State::Extracting; break;
        case Phase::Idle:
        case Phase::Checking:
        case Phase::Verifying: v.state = ui::ModelProgressView::State::Checking; break;
        default: break;   // finished: handled by drawModelDownload, hidden meanwhile
    }
    if (v.state == ui::ModelProgressView::State::Extracting) v.file.clear();
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
    LOGI("coach: voice %s, model %s in %s", settings().coachVoice ? "on" : "off (subtitles only)",
         tts::statusName(tts::modelStatus()), tts::modelFolder().c_str());
}

bool coachModelDownloading() {
    ModelUi& u = state();
    return u.job && u.job->running();
}

bool coachModelNeedsPrompt() {
    ModelUi& u = state();
    if (!settings().coachVoice || coachModelDownloading()) return false;
    if (u.suspect) return true;
    return tts::modelStatus() != tts::ModelStatus::Ready;
}

void openModelPrompt() { state().promptOpen = true; }

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
}

void drawModelDownload() {
    ModelUi& u = state();
    if (!u.inited) coachModelInit();
    // Options > Audio > Coach voice switched on (and applied): offer the model when it is missing.
    bool voice = settings().coachVoice;
    if (voice && !u.lastVoice && coachModelNeedsPrompt()) u.promptOpen = true;
    u.lastVoice = voice;
    if (!voice) u.promptOpen = false;

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

    if (u.promptOpen && !settings().coachVoiceOffered) {
        // Remembered: the Analysis mode offers the voice only to a player who never saw this.
        settings().coachVoiceOffered = true;
        settings().save();
    }
    if (u.promptOpen) {
        ui::ModelPrompt mp;
        mp.bytes = double(tts::supertonicManifest().totalBytes());
        mp.folder = tts::modelFolder();
        switch (ui::modelPrompt(mp)) {
            case ui::ModelPromptAction::Download:
                u.promptOpen = false;
                startDownload();
                break;
            case ui::ModelPromptAction::NotNow:
                // Remembered: the coach speaks through subtitles until the voice is switched on again.
                u.promptOpen = false;
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
