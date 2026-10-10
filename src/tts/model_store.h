// The coach's voice model on disk: which files it is made of (the manifest), where they live, whether
// they are all there, and the background job that downloads them. Scacelith never ships the model
// (the weights are under OpenRAIL-M, kept out of the GPL-3.0 program): the game fetches it on the
// player's request, the first time Coach mode or the coach's voice needs it.
//
// Folder: <application data>/coach/ (Windows %APPDATA%\scacelith\coach\, Linux
// $XDG_DATA_HOME/scacelith/coach/, by default ~/.local/share/scacelith/coach/), or the folder given
// with setModelFolder() (the --coach-dir command-line option).
//
// The model is the official Supertonic 3 release by Supertone (fp32 ONNX graphs, the character
// indexer and the voice style as JSON). Sources, file by file, one after the other, no archive and
// no extraction:
//   1. https://huggingface.co/Supertone/supertonic-3/resolve/<revision>/<path>
//   2. when anything goes wrong there (network, an error status, a file that does not hash right, a
//      redirect to a non-HTTPS host), the same revision of Supertone's archive copy:
//      https://huggingface.co/supertone-oss-archive/supertonic-3/resolve/<revision>/<path>
// (the resolve endpoint answers small files itself and redirects the large ones to its CDN).
// Every file streams in as "<name>.part", is checked against the manifest (size and SHA-256) as it
// arrives, and is renamed only then. The job also writes README.txt (where the files came from,
// their licences, that Scacelith does not redistribute them) and the OpenRAIL-M text beside them.
//
// Earlier versions of the game downloaded sherpa-onnx's INT8 conversion of the same model
// (legacyManifest()). The runtime still speaks with it while the player keeps it (src/tts/model.h),
// and the game offers the official files in its place (game/coach_model.h): the download job first
// deletes every file of the old version, then fetches the new ones.
#pragma once
#include "net/transport.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tts {

struct ManifestFile {
    std::string name;             // in the model folder
    uint64_t size = 0;
    std::string sha256;           // hex, lower-case
    std::string remote;           // path in the repository ("onnx/vocoder.onnx"); "" = name
    const std::string& remotePath() const { return remote.empty() ? name : remote; }
};

struct ModelSource {
    std::string base;             // URL prefix of a file ("https://huggingface.co/<repo>/resolve/<revision>/")
    std::string label;            // shown in the progress panel and the folder's README ("huggingface.co/<repo>")
};

struct ModelManifest {
    std::vector<ManifestFile> files;
    std::vector<ModelSource> sources;   // tried in this order; empty = cannot be downloaded
    uint64_t totalBytes() const;        // of all the files
    const ManifestFile* find(const std::string& name) const;
    std::string url(const ModelSource& s, const ManifestFile& f) const { return s.base + f.remotePath(); }
};

// The official Supertonic 3 (Supertone/supertonic-3, the revision pinned by Supertone's own Python
// SDK, and the same files in supertone-oss-archive/supertonic-3): the four ONNX graphs, tts.json,
// unicode_indexer.json and the voice style M3.json, 398,651,400 bytes.
const ModelManifest& supertonicManifest();
// The previous model: sherpa-onnx's INT8 conversion (release
// sherpa-onnx-supertonic-3-tts-int8-2026-05-11, 145,316,356 bytes, nine files), recognised by its
// sizes and SHA-256. No source: it is not downloaded any more.
const ModelManifest& legacyManifest();

// The model folder, with a trailing separator (not created here).
std::string modelFolder();
// --coach-dir: use this folder instead of <application data>/coach/ ("" = back to the default).
void setModelFolder(const std::string& dir);
// "Scacelith/<version> (<os>)", the User-Agent of the downloads.
std::string downloadUserAgent();

enum class ModelStatus {
    Ready,        // every file present with its size (the quick check)
    Missing,      // none of them
    Incomplete,   // some missing or of another size
    Corrupt       // verifyModel() only: a file of the right size whose SHA-256 differs
};
const char* statusName(ModelStatus s);

// The quick check: sizes only (a stat per file). Used before showing the coach page.
ModelStatus modelStatus(const ModelManifest& m = supertonicManifest(), const std::string& folder = modelFolder());
// The full check: hashes every present file (a few seconds for the real model). 'bad' receives
// the files missing, of another size or with another digest. Run after a download and when
// loading the model failed. 'progress' receives the bytes hashed so far and the bytes to hash.
ModelStatus verifyModel(const ModelManifest& m, const std::string& folder, std::vector<std::string>* bad = nullptr,
                        const std::atomic<bool>* cancel = nullptr,
                        const std::function<void(uint64_t done, uint64_t total)>& progress = nullptr);

// Which model the folder holds, by the quick check: the official one when all its files are
// there, else the old INT8 one when all of its are, else none.
enum class ModelKind { None, Official, Legacy };
ModelKind installedModel(const std::string& folder = modelFolder());
const char* kindName(ModelKind k);

// Anything of the old INT8 model in the folder: one of its files that the official model does not
// share, a .part of one, or the release archive its fallback downloaded (.tar.bz2 or its .part).
bool legacyFilesPresent(const std::string& folder = modelFolder());
// Deletes all of that, the files shared by name with the official model (tts.json) and the
// notices written for it (README.txt, Supertonic-3-OpenRAIL-M.txt). 'removed' receives the bytes
// freed. False when a file stays (in use, read-only); 'error' then names it.
bool removeLegacyModel(const std::string& folder, uint64_t* removed = nullptr, std::string* error = nullptr);

// Writes README.txt (where the files came from, their licences, that Scacelith does not
// redistribute them) and Supertonic-3-OpenRAIL-M.txt into 'folder'. 'source' is the sentence that
// names the source(s).
bool writeFolderNotices(const std::string& folder, const std::string& source);

// What the download job is doing, for the progress panel.
struct DownloadProgress {
    enum class Phase {
        Idle,
        Removing,     // deleting the files of the old INT8 model
        Checking,     // hashing files already in the folder (kept when they match)
        Fetching,     // downloading the missing files, one after the other
        Verifying,    // the final check of every file
        Done,
        Failed,
        Cancelled
    };
    Phase phase = Phase::Idle;
    // Bytes of the phase: Checking/Verifying = hashed / to hash; Fetching = model bytes present /
    // model size.
    uint64_t done = 0, total = 0;
    std::string file;             // the file in progress ("" = none)
    int fileIndex = 0, fileCount = 0;   // Fetching: 1-based index of the file among those to fetch
    std::string sourceLabel;      // the repository in use (ModelSource::label)
    int sourceIndex = 0;          // its index in ModelManifest::sources (1 = the archive copy)
    int fetched = 0;              // Done: the files the job wrote (0 = every file was already there and right)
    bool removedLegacy = false;   // the job deleted the old INT8 model first
    std::string firstSourceError; // why the first source was given up ("" = it was not)
    // Failed: a short code ("network", "timeout", "http", "hash", "size", "io", "verify",
    // "unavailable", ...) and a detail for the log.
    std::string error, detail;
    bool finished() const { return phase == Phase::Done || phase == Phase::Failed || phase == Phase::Cancelled; }
};

// The background download job. One job at a time per object; the object outlives its thread
// (the destructor cancels and joins).
class ModelDownloader {
public:
    struct Options {
        int firstSource = 0;          // index of the first source tried (tests: 1 = the archive copy only)
        bool fallback = true;         // false: no later source (tests)
        int timeoutMs = 30000;        // connect and each wait for data
        int retryDelayMs = 1000;
        std::string userAgent;        // "" = downloadUserAgent()
    };
    explicit ModelDownloader(ModelManifest m = supertonicManifest(), std::string folder = std::string());   // "" = modelFolder()
    ~ModelDownloader();
    ModelDownloader(const ModelDownloader&) = delete;
    ModelDownloader& operator=(const ModelDownloader&) = delete;

    // Starts the job on its own thread. False while a job is running. Whoever maps the old model's
    // files (a TTS worker) must have let them go first: the job deletes them.
    bool start(const Options& o);
    bool start() { return start(Options()); }
    void cancel();                    // returns at once; the job ends within a moment (Cancelled)
    void wait();                      // joins the thread (tests, shutdown)
    bool running() const { return running_.load(); }
    DownloadProgress progress() const;
    const std::string& folder() const { return folder_; }

private:
    void run(Options o);
    void update(const std::function<void(DownloadProgress&)>& fn);
    bool cancelled() const { return cancelFlag_.load(); }

    ModelManifest manifest_;
    std::string folder_;
    std::thread thread_;
    std::atomic<bool> running_{false}, cancelFlag_{false};
    net::CancelToken cancel_;
    mutable std::mutex mu_;
    DownloadProgress progress_;
};

}  // namespace tts
