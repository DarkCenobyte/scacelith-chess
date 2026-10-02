#include "model_store.h"
#include "core/bzip2.h"
#include "core/embedded.h"
#include "core/log.h"
#include "core/tar.h"
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

std::string ModelManifest::archiveName() const { return archiveFolder + ".tar.bz2"; }

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
        const std::string release = "sherpa-onnx-supertonic-3-tts-int8-2026-05-11";
        // The Hugging Face listing of the repository; same sizes and digests as in the archive.
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
        r.hubBase = "https://huggingface.co/csukuangfj2/" + release + "/resolve/main/";
        r.hubLabel = "huggingface.co/csukuangfj2/" + release;
        r.archiveUrl = "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/" + release + ".tar.bz2";
        r.archiveLabel = "github.com/k2-fsa/sherpa-onnx";   // its release "tts-models"
        r.archiveSize = 128774318;
        r.archiveSha256 = "82fa96f91c4ef8abaae3a14a3f4153facf88bed821d1f7331cec2700f432c427";
        r.archiveFolder = release;
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

// ---- Archive extraction ----------------------------------------------------------------------------

bool extractModelArchive(const ModelManifest& m, const std::string& archive, const std::string& folder,
                         const std::vector<std::string>& names, std::string& error, const std::atomic<bool>* cancel,
                         const std::function<void(uint64_t, uint64_t, const std::string&)>& progress) {
    error.clear();
    uint64_t archiveSize = 0;
    net::sys::fileSize(archive, archiveSize);
    std::FILE* in = net::sys::openFile(archive, "rb");
    if (!in) {
        error = "cannot open " + archive;
        return false;
    }
    bz2::Decoder bz([in](uint8_t* buf, size_t n) -> long {
        size_t r = std::fread(buf, 1, n, in);
        return r == 0 && std::ferror(in) ? -1 : long(r);
    });
    std::string bzError;
    tar::Reader tr([&](uint8_t* buf, size_t n) -> long {
        long r = bz.read(buf, n);
        if (r < 0) bzError = bz.error();
        return r;
    });
    std::vector<std::string> wanted = names;
    std::vector<uint8_t> buf(256 * 1024);
    std::string writing;   // the .part being written, removed on failure
    auto failWith = [&](const std::string& why) {
        error = why;
        return false;
    };
    bool ok = [&]() {
        tar::Entry e;
        while (!wanted.empty()) {
            if (cancel && cancel->load()) return failWith("cancelled");
            if (progress) progress(bz.consumed(), archiveSize, std::string());
            if (!tr.next(e)) {
                if (!tr.error().empty()) return failWith("damaged archive: " + (bzError.empty() ? tr.error() : bzError));
                std::string list;
                for (const std::string& w : wanted) list += (list.empty() ? "" : ", ") + w;
                return failWith("not in the archive: " + list);
            }
            std::string path = e.path;
            while (path.compare(0, 2, "./") == 0) path.erase(0, 2);
            if (path.empty()) continue;   // "./" itself
            if (!tar::safePath(path)) return failWith("unsafe path in the archive: " + e.path);
            std::string prefix = m.archiveFolder + "/";
            if (path.compare(0, prefix.size(), prefix) != 0) continue;
            std::string name = path.substr(prefix.size());
            auto it = std::find(wanted.begin(), wanted.end(), name);
            if (it == wanted.end()) continue;
            const ManifestFile* mf = m.find(name);
            if (!mf) continue;
            if (!e.regular()) return failWith(name + " is not a regular file in the archive");
            if (e.size != mf->size)
                return failWith(name + " has " + std::to_string(e.size) + " bytes in the archive, expected " + std::to_string(mf->size));
            writing = folder + name + ".part";
            std::FILE* out = net::sys::openFile(writing, "wb");
            if (!out) return failWith("cannot write " + writing);
            net::crypto::Sha256Stream h;
            bool written = true;
            for (;;) {
                if (cancel && cancel->load()) {
                    std::fclose(out);
                    return failWith("cancelled");
                }
                long n = tr.read(buf.data(), buf.size());
                if (n < 0) {
                    std::fclose(out);
                    return failWith("damaged archive: " + (bzError.empty() ? tr.error() : bzError));
                }
                if (n == 0) break;
                h.update(buf.data(), size_t(n));
                written = written && std::fwrite(buf.data(), 1, size_t(n), out) == size_t(n);
                if (progress) progress(bz.consumed(), archiveSize, name);
            }
            written = std::fclose(out) == 0 && written;
            if (!written) return failWith("cannot write " + writing);
            std::string digest = net::crypto::hex(h.finish());
            if (digest != mf->sha256) return failWith(name + " from the archive does not match its SHA-256");
            if (!net::sys::renameFile(writing, folder + name)) return failWith("cannot rename " + writing);
            writing.clear();
            wanted.erase(it);
        }
        return true;
    }();
    std::fclose(in);
    if (!writing.empty()) net::sys::removeFile(writing);
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

    // 1. Files already here: kept when they hash right (an earlier download, a copy), else removed.
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
    std::vector<std::string> sources;

    // 2. The hub, file by file.
    std::string hubError, hubDetail;
    if (!missing.empty() && o.useHub && !manifest_.hubBase.empty()) {
        uint64_t present = manifest_.totalBytes();
        for (const ManifestFile* f : missing) present -= f->size;
        update([&](DownloadProgress& p) {
            p.phase = Phase::Hub;
            p.sourceLabel = manifest_.hubLabel;
            p.done = present;
            p.total = manifest_.totalBytes();
            p.fileCount = int(missing.size());
        });
        LOGI("tts: fetching %d file(s) from %s", int(missing.size()), manifest_.hubLabel.c_str());
        std::vector<const ManifestFile*> still;
        for (size_t i = 0; i < missing.size(); ++i) {
            const ManifestFile* f = missing[i];
            if (!hubError.empty()) {   // given up: the archive brings the rest
                still.push_back(f);
                continue;
            }
            update([&](DownloadProgress& p) {
                p.file = f->name;
                p.fileIndex = int(i) + 1;
            });
            net::DownloadRequest rq;
            rq.url = manifest_.hubBase + f->name;
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
                LOGI("tts: %s from %s (%s, %.1f s%s)", f->name.c_str(), r.host.c_str(), mb(f->size).c_str(),
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count(), r.resumed ? ", continued" : "");
                continue;
            }
            if (r.error == "cancelled" || cancelled()) return finish(Phase::Cancelled, "cancelled", std::string());
            if (r.error == "io") return finish(Phase::Failed, "io", r.detail);   // the archive would not fare better
            hubError = r.error == "http" ? "http " + std::to_string(r.status) : r.error;
            hubDetail = f->name + ": " + (r.detail.empty() ? r.error : r.detail);
            LOGW("tts: the hub failed (%s), switching to the release archive", hubDetail.c_str());
            net::sys::removeFile(folder_ + f->name + ".part");
            still.push_back(f);
        }
        if (still.size() < missing.size()) sources.push_back("hub");
        missing = still;
    }

    // 3. The release archive: downloaded, checked, extracted, deleted.
    if (!missing.empty()) {
        if (!o.useArchive || manifest_.archiveUrl.empty())
            return finish(Phase::Failed, hubError.empty() ? "unavailable" : hubError, hubDetail);
        const std::string archive = folder_ + manifest_.archiveName();
        update([&](DownloadProgress& p) {
            p.phase = Phase::Archive;
            p.sourceLabel = manifest_.archiveLabel;
            p.fromArchive = true;
            p.hubError = hubError;
            p.file = manifest_.archiveName();
            p.fileIndex = p.fileCount = 0;
            p.done = 0;
            p.total = manifest_.archiveSize;
        });
        LOGI("tts: fetching the release archive %s", manifest_.archiveUrl.c_str());
        // A complete archive left by a cancelled extraction is used as it is (checked by download()).
        net::DownloadRequest rq;
        rq.url = manifest_.archiveUrl;
        rq.path = archive;
        rq.userAgent = agent;
        rq.expectedSize = manifest_.archiveSize;
        rq.sha256 = manifest_.archiveSha256;
        rq.timeoutMs = o.timeoutMs;
        rq.retryDelayMs = o.retryDelayMs;
        rq.onProgress = [&](uint64_t done, uint64_t) { update([&](DownloadProgress& p) { p.done = done; }); };
        uint64_t existing = 0;
        net::DownloadResult r;
        if (net::sys::fileSize(archive, existing) && existing == manifest_.archiveSize &&
            net::fileSha256(archive, &cancelFlag_) == manifest_.archiveSha256) {
            r.ok = true;
        } else {
            auto ts = std::chrono::steady_clock::now();
            r = net::download(rq, &cancel_);
            if (r.ok)
                LOGI("tts: archive from %s (%s, %.1f s)", r.host.c_str(), mb(r.bytes).c_str(),
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count());
        }
        if (!r.ok) {
            if (r.error == "cancelled" || cancelled()) return finish(Phase::Cancelled, "cancelled", std::string());
            return finish(Phase::Failed, r.error == "http" ? "http " + std::to_string(r.status) : r.error, r.detail);
        }
        update([&](DownloadProgress& p) {
            p.phase = Phase::Extracting;
            p.done = 0;
            p.total = manifest_.archiveSize;
        });
        std::vector<std::string> names;
        for (const ManifestFile* f : missing) names.push_back(f->name);
        std::string error;
        auto ts = std::chrono::steady_clock::now();
        bool ok = extractModelArchive(manifest_, archive, folder_, names, error, &cancelFlag_,
                                      [&](uint64_t read, uint64_t total, const std::string& file) {
                                          update([&](DownloadProgress& p) {
                                              p.done = read;
                                              p.total = total;
                                              p.file = file;
                                          });
                                      });
        if (!ok && cancelled()) return finish(Phase::Cancelled, "cancelled", std::string());   // the archive stays for next time
        net::sys::removeFile(archive);
        if (!ok) return finish(Phase::Failed, error.compare(0, 6, "cannot") == 0 ? "io" : "archive", error);
        LOGI("tts: %d file(s) extracted (%.1f s)", int(names.size()),
             std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count());
        sources.push_back("archive");
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
    bool hub = std::find(sources.begin(), sources.end(), "hub") != sources.end();
    bool arc = std::find(sources.begin(), sources.end(), "archive") != sources.end();
    const std::string hubUrl = "https://" + manifest_.hubLabel;
    if (hub && arc)
        source = "Scacelith downloaded them from Hugging Face (" + hubUrl + ") and from the sherpa-onnx release archive on GitHub (" +
                 manifest_.archiveUrl + ").";
    else if (hub)
        source = "Scacelith downloaded them from Hugging Face, " + hubUrl + ", file by file.";
    else if (arc)
        source = "Scacelith downloaded them from the sherpa-onnx release archive on GitHub, " + manifest_.archiveUrl +
                 ", and extracted them from it.";
    else
        source = "They were already in this folder when Scacelith checked it; they are published on Hugging Face (" + hubUrl +
                 ") and in the sherpa-onnx release archive on GitHub (" + manifest_.archiveUrl + ").";
    if (!writeFolderNotices(folder_, source)) LOGW("tts: cannot write the notices in %s", folder_.c_str());
    finish(Phase::Done, std::string(), std::string());
}

}  // namespace tts
