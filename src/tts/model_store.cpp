#include "model_store.h"
#include "core/embedded.h"
#include "core/log.h"
#include "net/crypto.h"
#include "net/download.h"
#include "net/net_sys.h"
#include "scacelith_version.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace tts {

namespace {

#ifdef _WIN32
constexpr char kSep = '\\';
#else
constexpr char kSep = '/';
#endif

std::mutex g_folderMu;
std::string g_folderOverride;

std::string withSeparator(std::string d) {
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += kSep;
    return d;
}

std::string mb(uint64_t b) {
    char s[32];
    std::snprintf(s, sizeof s, "%.1f MB", double(b) / 1e6);
    return s;
}

}  // namespace

// ---- Manifest --------------------------------------------------------------------------------------

uint64_t ModelManifest::totalBytes() const {
    uint64_t t = 0;
    for (const ManifestFile& f : files) t += f.size;
    return t;
}

const ManifestFile* ModelManifest::find(const std::string& name) const {
    for (const ManifestFile& f : files)
        if (f.name == name) return &f;
    return nullptr;
}

const ModelManifest& supertonicManifest() {
    static const ModelManifest m = [] {
        ModelManifest r;
        // The files the runtime reads (Engine::kOfficial), as listed on Hugging Face; the same sizes and
        // digests at both revisions below. The folder is flat: the repository's onnx/ and
        // voice_styles/ prefixes are dropped.
        r.files = {
            {"tts.json", 8253, "42078d3aef1cd43ab43021f3c54f47d2d75ceb4e75f627f118890128b06a0d09", "onnx/tts.json"},
            {"unicode_indexer.json", 277676, "9bf7346e43883a81f8645c81224f786d43c5b57f3641f6e7671a7d6c493cb24f", "onnx/unicode_indexer.json"},
            {"M3.json", 290198, "ea1ac35ccb91b0d7ecad533a2fbd0eec10c91513d8951e3b25fbba99954e159b", "voice_styles/M3.json"},
            {"duration_predictor.onnx", 3700147, "c3eb91414d5ff8a7a239b7fe9e34e7e2bf8a8140d8375ffb14718b1c639325db",
             "onnx/duration_predictor.onnx"},
            {"text_encoder.onnx", 36416150, "c7befd5ea8c3119769e8a6c1486c4edc6a3bc8365c67621c881bbb774b9902ff",
             "onnx/text_encoder.onnx"},
            {"vector_estimator.onnx", 256534781, "883ac868ea0275ef0e991524dc64f16b3c0376efd7c320af6b53f5b780d7c61c", "onnx/vector_estimator.onnx"},
            {"vocoder.onnx", 101424195, "085de76dd8e8d5836d6ca66826601f615939218f90e519f70ee8a36ed2a4c4ba", "onnx/vocoder.onnx"},
        };
        // Supertone's repository at the revision its Python SDK pins (supertonic 1.3.1, config.py),
        // then Supertone's archive copy at the revision its GitHub README pins.
        r.sources = {
            {"https://huggingface.co/Supertone/supertonic-3/resolve/724fb5abbf5502583fb520898d45929e62f02c0b/",
             "huggingface.co/Supertone/supertonic-3"},
            {"https://huggingface.co/supertone-oss-archive/supertonic-3/resolve/aafc6e32416a594460b32413efc49d7fe4ce6d46/",
             "huggingface.co/supertone-oss-archive/supertonic-3"},
        };
        return r;
    }();
    return m;
}

const ModelManifest& legacyManifest() {
    static const ModelManifest m = [] {
        ModelManifest r;
        r.files = {
            {"LICENSE", 1070, "0dfe0d0ba84416fe3879d9a34f4909d8d0137c78d1e95834177b0414ac096fa2"},
            {"README.md", 19518, "a96c347945f7c8bc1673bea3525b1ac8d36fdde556e1e0a6a186052429caf863"},
            {"duration_predictor.int8.onnx", 3700147, "c3eb91414d5ff8a7a239b7fe9e34e7e2bf8a8140d8375ffb14718b1c639325db"},
            {"text_encoder.int8.onnx", 36416150, "c7befd5ea8c3119769e8a6c1486c4edc6a3bc8365c67621c881bbb774b9902ff"},
            {"tts.json", 8253, "42078d3aef1cd43ab43021f3c54f47d2d75ceb4e75f627f118890128b06a0d09"},
            {"unicode_indexer.bin", 262144, "8402ca48e5189a8950138580b0fff64db6f072f24ac07cd54ba8b2fbb9883b30"},
            {"vector_estimator.int8.onnx", 78400833, "20cd86fa5c6effedfda0e7cffe5b0569ca401c440a0c3a1d72bf39286c0db3fd"},
            {"vocoder.int8.onnx", 25991073, "e923d60f53f95eb1ce235f1dc33ec56d9c057823c96fa6f8acf98f32b0da6152"},
            {"voice.bin", 517168, "67d5209b0ee8ce6c74105ffbe12fe6a7628aea3b4ba2fcb308a4a67938a93ce8"},
        };
        return r;
    }();
    return m;
}

// ---- Folder ----------------------------------------------------------------------------------------

std::string modelFolder() {
    {
        std::lock_guard<std::mutex> lk(g_folderMu);
        if (!g_folderOverride.empty()) return g_folderOverride;
    }
    return net::sys::appDataDirectory() + "coach" + kSep;
}

void setModelFolder(const std::string& dir) {
    std::lock_guard<std::mutex> lk(g_folderMu);
    g_folderOverride = withSeparator(dir);
}

std::string downloadUserAgent() {
#ifdef _WIN32
    return std::string("Scacelith/") + SCACELITH_VERSION + " (Windows)";
#else
    return std::string("Scacelith/") + SCACELITH_VERSION + " (Linux)";
#endif
}

// ---- Status ----------------------------------------------------------------------------------------

const char* statusName(ModelStatus s) {
    switch (s) {
        case ModelStatus::Ready: return "ready";
        case ModelStatus::Missing: return "missing";
        case ModelStatus::Incomplete: return "incomplete";
        case ModelStatus::Corrupt: return "corrupt";
    }
    return "?";
}

ModelStatus modelStatus(const ModelManifest& m, const std::string& folder) {
    int good = 0, present = 0;
    for (const ManifestFile& f : m.files) {
        uint64_t size = 0;
        if (!net::sys::fileSize(folder + f.name, size)) continue;
        ++present;
        if (size == f.size) ++good;
    }
    if (good == int(m.files.size())) return ModelStatus::Ready;
    return present == 0 ? ModelStatus::Missing : ModelStatus::Incomplete;
}

ModelStatus verifyModel(const ModelManifest& m, const std::string& folder, std::vector<std::string>* bad,
                        const std::atomic<bool>* cancel, const std::function<void(uint64_t, uint64_t)>& progress) {
    if (bad) bad->clear();
    uint64_t total = 0, done = 0;
    std::vector<bool> sized(m.files.size(), false);
    int present = 0;
    for (size_t i = 0; i < m.files.size(); ++i) {
        uint64_t size = 0;
        if (!net::sys::fileSize(folder + m.files[i].name, size)) continue;
        ++present;
        if (size == m.files[i].size) {
            sized[i] = true;
            total += size;
        }
    }
    bool missing = false, corrupt = false;
    for (size_t i = 0; i < m.files.size(); ++i) {
        const ManifestFile& f = m.files[i];
        if (!sized[i]) {
            missing = true;
            if (bad) bad->push_back(f.name);
            continue;
        }
        uint64_t base = done;
        std::string digest = net::fileSha256(folder + f.name, cancel, [&](uint64_t n) {
            if (progress) progress(base + n, total);
        });
        done = base + f.size;
        if (digest != f.sha256) {
            if (cancel && cancel->load()) return ModelStatus::Incomplete;   // not known
            LOGW("tts: %s%s does not match its SHA-256", folder.c_str(), f.name.c_str());
            corrupt = true;
            if (bad) bad->push_back(f.name);
        }
    }
    if (corrupt) return ModelStatus::Corrupt;
    if (!missing) return ModelStatus::Ready;
    return present == 0 ? ModelStatus::Missing : ModelStatus::Incomplete;
}

// ---- Which model, and the old one -----------------------------------------------------------------

ModelKind installedModel(const std::string& folder) {
    if (modelStatus(supertonicManifest(), folder) == ModelStatus::Ready) return ModelKind::Official;
    if (modelStatus(legacyManifest(), folder) == ModelStatus::Ready) return ModelKind::Legacy;
    return ModelKind::None;
}

const char* kindName(ModelKind k) {
    switch (k) {
        case ModelKind::Official: return "official";
        case ModelKind::Legacy: return "legacy INT8";
        case ModelKind::None: return "none";
    }
    return "?";
}

namespace {

// The release archive the old download fell back to (left behind when an extraction was cancelled).
const char* const kLegacyArchive = "sherpa-onnx-supertonic-3-tts-int8-2026-05-11.tar.bz2";

// Every name the old model put in the folder; 'shared' adds those the official model uses too.
std::vector<std::string> legacyNames(bool shared) {
    std::vector<std::string> names;
    for (const ManifestFile& f : legacyManifest().files) {
        if (!shared && supertonicManifest().find(f.name)) continue;
        names.push_back(f.name);
        names.push_back(f.name + ".part");
    }
    names.push_back(kLegacyArchive);
    names.push_back(std::string(kLegacyArchive) + ".part");
    return names;
}

}  // namespace

bool legacyFilesPresent(const std::string& folder) {
    for (const std::string& n : legacyNames(false))
        if (net::sys::fileExists(folder + n)) return true;
    return false;
}

bool removeLegacyModel(const std::string& folder, uint64_t* removed, std::string* error) {
    std::vector<std::string> names = legacyNames(true);
    // Its notices: written again for the official files once they are all there.
    names.push_back("README.txt");
    names.push_back("Supertonic-3-OpenRAIL-M.txt");
    uint64_t freed = 0;
    bool ok = true;
    for (const std::string& n : names) {
        uint64_t size = 0;
        if (!net::sys::fileSize(folder + n, size)) continue;
        if (net::sys::removeFile(folder + n)) {
            freed += size;
        } else {
            LOGW("tts: cannot delete %s%s", folder.c_str(), n.c_str());
            if (ok && error) *error = "cannot delete " + folder + n;
            ok = false;
        }
    }
    if (removed) *removed = freed;
    return ok;
}

// ---- Notices ---------------------------------------------------------------------------------------

bool writeFolderNotices(const std::string& folder, const std::string& source) {
    std::string readme = embedded::text("assets/tts/coach-folder-README.txt");
    std::string licence = embedded::text("assets/licences/Supertonic-3-OpenRAIL-M.txt");
    size_t at = readme.find("{source}");
    if (at != std::string::npos) readme.replace(at, 8, source);
#ifdef _WIN32
    // Notepad of older Windows wants CR LF.
    for (std::string* s : {&readme, &licence}) {
        std::string t;
        for (char c : *s) {
            if (c == '\n' && (t.empty() || t.back() != '\r')) t += '\r';
            t += c;
        }
        *s = t;
    }
#endif
    bool ok = !readme.empty() && net::sys::writeFileAtomic(folder + "README.txt", readme, false);
    ok = !licence.empty() && net::sys::writeFileAtomic(folder + "Supertonic-3-OpenRAIL-M.txt", licence, false) && ok;
    return ok;
}

// ---- Download job ----------------------------------------------------------------------------------

ModelDownloader::ModelDownloader(ModelManifest m, std::string folder)
    : manifest_(std::move(m)), folder_(folder.empty() ? modelFolder() : withSeparator(folder)) {}

ModelDownloader::~ModelDownloader() {
    cancel();
    wait();
}

bool ModelDownloader::start(const Options& o) {
    if (running_.load()) return false;
    wait();
    cancelFlag_ = false;
    cancel_.reset();
    {
        std::lock_guard<std::mutex> lk(mu_);
        progress_ = DownloadProgress();
        progress_.phase = DownloadProgress::Phase::Checking;
    }
    running_ = true;
    thread_ = std::thread([this, o] { run(o); });
    return true;
}

void ModelDownloader::cancel() {
    cancelFlag_ = true;
    cancel_.cancel();
}

void ModelDownloader::wait() {
    if (thread_.joinable()) thread_.join();
}

DownloadProgress ModelDownloader::progress() const {
    std::lock_guard<std::mutex> lk(mu_);
    return progress_;
}

void ModelDownloader::update(const std::function<void(DownloadProgress&)>& fn) {
    std::lock_guard<std::mutex> lk(mu_);
    fn(progress_);
}

void ModelDownloader::run(Options o) {
    using Phase = DownloadProgress::Phase;
    const auto t0 = std::chrono::steady_clock::now();
    auto seconds = [&t0] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    const std::string agent = o.userAgent.empty() ? downloadUserAgent() : o.userAgent;
    auto finish = [&](Phase phase, const std::string& error, const std::string& detail) {
        update([&](DownloadProgress& p) {
            p.phase = phase;
            p.error = error;
            p.detail = detail;
            p.file.clear();
        });
        if (phase == Phase::Done) LOGI("tts: voice model ready in %s (%.1f s)", folder_.c_str(), seconds());
        else if (phase == Phase::Cancelled) LOGI("tts: voice model download cancelled");
        else LOGW("tts: voice model download failed: %s (%s)", error.c_str(), detail.c_str());
        running_ = false;
    };

    if (!net::sys::makeDirectories(folder_)) return finish(Phase::Failed, "io", "cannot create " + folder_);
    LOGI("tts: downloading the voice model into %s", folder_.c_str());

    // 1. The old INT8 model goes first: its files are not used once the new ones are there, and
    // the disk space they free counts for the new ones.
    if (legacyFilesPresent(folder_)) {
        update([&](DownloadProgress& p) { p.phase = Phase::Removing; });
        uint64_t freed = 0;
        std::string error;
        if (!removeLegacyModel(folder_, &freed, &error)) return finish(Phase::Failed, "io", error);
        update([&](DownloadProgress& p) { p.removedLegacy = true; });
        LOGI("tts: the old INT8 model was deleted (%s freed)", mb(freed).c_str());
    }

    // 2. Files already here: kept when they hash right (an earlier download, a copy), else removed.
    update([&](DownloadProgress& p) { p.phase = Phase::Checking; });
    std::vector<const ManifestFile*> missing;
    {
        uint64_t toHash = 0, hashed = 0;
        std::vector<const ManifestFile*> sized;
        for (const ManifestFile& f : manifest_.files) {
            uint64_t size = 0;
            if (net::sys::fileSize(folder_ + f.name, size) && size == f.size) {
                sized.push_back(&f);
                toHash += size;
            } else {
                missing.push_back(&f);
                if (net::sys::fileExists(folder_ + f.name)) net::sys::removeFile(folder_ + f.name);
            }
        }
        update([&](DownloadProgress& p) { p.total = toHash; });
        for (const ManifestFile* f : sized) {
            update([&](DownloadProgress& p) { p.file = f->name; });
            uint64_t base = hashed;
            std::string digest = net::fileSha256(folder_ + f->name, &cancelFlag_, [&](uint64_t n) {
                update([&](DownloadProgress& p) { p.done = base + n; });
            });
            if (cancelled()) return finish(Phase::Cancelled, "cancelled", std::string());
            hashed += f->size;
            if (digest != f->sha256) {
                LOGW("tts: %s does not match its SHA-256, fetching it again", f->name.c_str());
                net::sys::removeFile(folder_ + f->name);
                missing.push_back(f);
            }
        }
    }
    // In the manifest's order (the small files first), whatever the check found.
    std::sort(missing.begin(), missing.end());
    const int fetched = int(missing.size());

    // 3. The missing files, one after the other, from the first source; once it fails, from the
    // next one (for that file and the rest).
    std::vector<size_t> used;   // the sources that sent files
    std::string firstError, lastError, lastDetail;
    if (!missing.empty()) {
        size_t src = size_t(std::max(0, o.firstSource));
        if (src >= manifest_.sources.size()) return finish(Phase::Failed, "unavailable", "no source to download from");
        uint64_t present = manifest_.totalBytes();
        for (const ManifestFile* f : missing) present -= f->size;
        update([&](DownloadProgress& p) {
            p.phase = Phase::Fetching;
            p.sourceLabel = manifest_.sources[src].label;
            p.sourceIndex = int(src);
            p.done = present;
            p.total = manifest_.totalBytes();
            p.file.clear();
            p.fileCount = int(missing.size());
        });
        LOGI("tts: fetching %d file(s) from %s", int(missing.size()), manifest_.sources[src].label.c_str());
        for (size_t i = 0; i < missing.size(); ++i) {
            const ManifestFile* f = missing[i];
            update([&](DownloadProgress& p) {
                p.file = f->name;
                p.fileIndex = int(i) + 1;
            });
            for (;;) {
                const ModelSource& source = manifest_.sources[src];
                net::DownloadRequest rq;
                rq.url = manifest_.url(source, *f);
                rq.path = folder_ + f->name;
                rq.userAgent = agent;
                rq.expectedSize = f->size;
                rq.sha256 = f->sha256;
                rq.timeoutMs = o.timeoutMs;
                rq.retryDelayMs = o.retryDelayMs;
                uint64_t base = present;
                rq.onProgress = [&](uint64_t done, uint64_t) { update([&](DownloadProgress& p) { p.done = base + done; }); };
                auto ts = std::chrono::steady_clock::now();
                net::DownloadResult r = net::download(rq, &cancel_);
                if (r.ok) {
                    present += f->size;
                    if (std::find(used.begin(), used.end(), src) == used.end()) used.push_back(src);
                    LOGI("tts: %s from %s (%s, %.1f s%s)", f->name.c_str(), r.host.c_str(), mb(f->size).c_str(),
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count(), r.resumed ? ", continued" : "");
                    break;
                }
                if (r.error == "cancelled" || cancelled()) return finish(Phase::Cancelled, "cancelled", std::string());
                lastError = r.error == "http" ? "http " + std::to_string(r.status) : r.error;
                lastDetail = f->name + " from " + source.label + ": " + (r.detail.empty() ? r.error : r.detail);
                // A disk that cannot take the file fares no better with another source.
                if (r.error == "io") return finish(Phase::Failed, "io", r.detail);
                if (!o.fallback || src + 1 >= manifest_.sources.size()) return finish(Phase::Failed, lastError, lastDetail);
                if (firstError.empty()) firstError = lastError;
                LOGW("tts: %s failed (%s), switching to %s", source.label.c_str(), lastDetail.c_str(),
                     manifest_.sources[src + 1].label.c_str());
                ++src;
                update([&](DownloadProgress& p) {
                    p.sourceLabel = manifest_.sources[src].label;
                    p.sourceIndex = int(src);
                    p.firstSourceError = firstError;
                });
            }
        }
    }

    // 4. Every file once more, then the notices.
    update([&](DownloadProgress& p) {
        p.phase = Phase::Verifying;
        p.done = 0;
        p.total = manifest_.totalBytes();
        p.file.clear();
    });
    std::vector<std::string> bad;
    ModelStatus st = verifyModel(manifest_, folder_, &bad, &cancelFlag_,
                                 [&](uint64_t done, uint64_t total) { update([&](DownloadProgress& p) { p.done = done; p.total = total; }); });
    if (cancelled()) return finish(Phase::Cancelled, "cancelled", std::string());
    if (st != ModelStatus::Ready) {
        for (const std::string& b : bad) net::sys::removeFile(folder_ + b);
        return finish(Phase::Failed, "verify", std::string(statusName(st)) + " after the download");
    }
    std::string source;
    auto address = [this](size_t i) { return "https://" + manifest_.sources[i].label; };
    if (used.size() > 1)
        source = "Scacelith downloaded them from Hugging Face, file by file, from " + address(used[0]) + " and " +
                 address(used[1]) + " (the same files).";
    else if (used.size() == 1)
        source = "Scacelith downloaded them from Hugging Face, " + address(used[0]) + ", file by file.";
    else if (!manifest_.sources.empty())
        source = "They were already in this folder when Scacelith checked it; they are published on Hugging Face (" +
                 address(0) + ").";
    else
        source = "They were already in this folder when Scacelith checked it.";
    if (!writeFolderNotices(folder_, source)) LOGW("tts: cannot write the notices in %s", folder_.c_str());
    update([&](DownloadProgress& p) { p.fetched = fetched; });
    finish(Phase::Done, std::string(), std::string());
}

}  // namespace tts
