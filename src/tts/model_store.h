// The coach's voice model on disk: which files it is made of (the manifest), where they live, whether
// they are all there, and the background job that downloads them. Scacelith never ships the model
// (the weights are under OpenRAIL-M, kept out of the GPL-3.0 program): the game fetches it on the
// player's request, the first time Coach mode or the coach's voice needs it.
//
// Folder: <application data>/coach/ (Windows %APPDATA%\scacelith\coach\, Linux
// $XDG_DATA_HOME/scacelith/coach/, by default ~/.local/share/scacelith/coach/), or the folder given
// with setModelFolder() (the --coach-dir command-line option).
//
// Sources, in this order:
//   1. Hugging Face, file by file, no extraction:
//      https://huggingface.co/csukuangfj2/sherpa-onnx-supertonic-3-tts-int8-2026-05-11/resolve/main/<file>
//      (the resolve endpoint answers small files itself and redirects the large ones to its CDN);
//   2. when anything goes wrong there (network, an error status, a file that does not hash right,
//      a redirect to a non-HTTPS host), the sherpa-onnx release archive on GitHub
//      (.tar.bz2, 129 MB), checked against its SHA-256, extracted here (bzip2 and tar readers of
//      src/core, only the manifest's files), then deleted.
// Every file streams in as "<name>.part", is checked against the manifest (size and SHA-256) as it
// arrives, and is renamed only then. The job also writes README.txt (where the files came from,
// their licences, that Scacelith does not redistribute them) and the OpenRAIL-M text beside them.
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
    std::string name;
    uint64_t size = 0;
    std::string sha256;           // hex, lower-case
};

struct ModelManifest {
    std::vector<ManifestFile> files;
    std::string hubBase;          // URL prefix of a file on the hub ("https://huggingface.co/<repo>/resolve/main/")
    std::string hubLabel;         // shown in the progress panel ("huggingface.co/csukuangfj2/...")
    std::string archiveUrl;       // the release archive (.tar.bz2)
    std::string archiveLabel;     // shown in the progress panel ("github.com/k2-fsa/sherpa-onnx")
    uint64_t archiveSize = 0;
    std::string archiveSha256;
    std::string archiveFolder;    // the folder of the files inside the archive
    std::string archiveName() const;   // file name of the archive while it is on disk
    uint64_t totalBytes() const;       // of all the files
    const ManifestFile* find(const std::string& name) const;
};

// Supertonic 3 INT8, release sherpa-onnx-supertonic-3-tts-int8-2026-05-11: the nine files of the
// Hugging Face repository (identical to the archive's), 145,316,356 bytes.
const ModelManifest& supertonicManifest();

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
// The full check: hashes every present file (about a second for the real model). 'bad' receives
// the files missing, of another size or with another digest. Run after a download and when
// loading the model failed. 'progress' receives the bytes hashed so far and the bytes to hash.
ModelStatus verifyModel(const ModelManifest& m, const std::string& folder, std::vector<std::string>* bad = nullptr,
                        const std::atomic<bool>* cancel = nullptr,
                        const std::function<void(uint64_t done, uint64_t total)>& progress = nullptr);

// Extracts from a .tar.bz2 release archive the manifest's files named in 'names' (the base names
// inside m.archiveFolder; anything else in the archive is skipped) into 'folder'. Each file lands
// as <name>.part, is checked (size, SHA-256) as it streams out and is renamed once it matches.
// The archive is refused at the first entry whose path could leave its folder, or when a wanted
// file is not a regular file. 'progress' receives the archive bytes read so far, the archive size
// and the file being written ("" between files).
bool extractModelArchive(const ModelManifest& m, const std::string& archive, const std::string& folder,
                         const std::vector<std::string>& names, std::string& error, const std::atomic<bool>* cancel = nullptr,
                         const std::function<void(uint64_t read, uint64_t total, const std::string& file)>& progress = nullptr);

// Writes README.txt (where the files came from, their licences, that Scacelith does not
// redistribute them) and Supertonic-3-OpenRAIL-M.txt into 'folder'. 'source' is the sentence that
// names the source(s).
bool writeFolderNotices(const std::string& folder, const std::string& source);

// What the download job is doing, for the progress panel.
struct DownloadProgress {
    enum class Phase {
        Idle,
        Checking,     // hashing files already in the folder (kept when they match)
        Hub,          // fetching files from the hub
        Archive,      // fetching the release archive (the hub failed)
        Extracting,   // taking the files out of the archive
        Verifying,    // the final check of every file
        Done,
        Failed,
        Cancelled
    };
    Phase phase = Phase::Idle;
    // Bytes of the phase: Checking/Verifying = hashed / to hash; Hub = model bytes present /
    // model size; Archive = archive bytes / archive size; Extracting = archive bytes read /
    // archive size.
    uint64_t done = 0, total = 0;
    std::string file;             // the file in progress ("" = none)
    int fileIndex = 0, fileCount = 0;   // Hub: 1-based index of the file among those to fetch
    std::string sourceLabel;      // the host / repository in use (ModelManifest::hubLabel or archiveLabel)
    bool fromArchive = false;     // the release archive is in use (the hub failed or was skipped)
    std::string hubError;         // why the hub was given up ("" = it was not)
    // Failed: a short code ("network", "timeout", "http", "hash", "size", "io", "archive",
    // "verify", "unavailable", ...) and a detail for the log.
    std::string error, detail;
    bool finished() const { return phase == Phase::Done || phase == Phase::Failed || phase == Phase::Cancelled; }
};

// The background download job. One job at a time per object; the object outlives its thread
// (the destructor cancels and joins).
class ModelDownloader {
public:
    struct Options {
        bool useHub = true;           // false: straight to the archive (tests)
        bool useArchive = true;       // false: no fallback (tests)
        int timeoutMs = 30000;        // connect and each wait for data
        int retryDelayMs = 1000;
        std::string userAgent;        // "" = downloadUserAgent()
    };
    explicit ModelDownloader(ModelManifest m = supertonicManifest(), std::string folder = std::string());   // "" = modelFolder()
    ~ModelDownloader();
    ModelDownloader(const ModelDownloader&) = delete;
    ModelDownloader& operator=(const ModelDownloader&) = delete;

    // Starts the job on its own thread (below normal priority is not needed: it mostly waits).
    // False while a job is running.
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
