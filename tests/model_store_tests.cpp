// The voice model store (tts/model_store.h): the manifests, the status checks, the old INT8 model's
// files, and the download job against a loopback server that plays both Hugging Face repositories
// (resolve endpoint, 302 to a CDN path, Range). The fake model has the seven names and repository
// paths of the real one with small contents (their digests were computed by Python's hashlib); the
// checks that go by the real model's sizes use sparse files of those sizes.
#include "test.h"
#include "http_fake.h"
#include "core/ini.h"
#include "game/coach_model.h"
#include "game/settings.h"
#include "net/net_sys.h"
#include "tts/model_store.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---- The fake model ----
constexpr int kFiles = 7;
constexpr int kLarge = 5;   // vector_estimator.onnx: several 16 KiB pieces (cut, continued, cancelled)
const char* const kNames[kFiles] = {"tts.json",          "unicode_indexer.json",  "M3.json",     "duration_predictor.onnx",
                                    "text_encoder.onnx", "vector_estimator.onnx", "vocoder.onnx"};
const char* const kRemote[kFiles] = {"onnx/tts.json",          "onnx/unicode_indexer.json",  "voice_styles/M3.json",
                                     "onnx/duration_predictor.onnx", "onnx/text_encoder.onnx", "onnx/vector_estimator.onnx",
                                     "onnx/vocoder.onnx"};
const uint64_t kSizes[kFiles] = {825, 1777, 2901, 3700, 36416, 85511, 33808};
const uint64_t kTotal = 164938;
const char* const kHashes[kFiles] = {
    "fb09ebcbf27cbb6906cac411ae105fa81d84c769f068d9afaa569890d57b59a5", "8cc349780da6fa55684f444587b12fb3738aaa0e21b4428b7fe6b5f71334e820",
    "d730da013bdf188197ec2d54240e37a78e45083d5f3e2a37b932fb786544136a", "25faaaa86e13fff144946a92429acf6908e49eed013c434be9ec98f8c2de4bc5",
    "ec6d0024e59be859ba11b3bc64c4f37f66f88607e92292dbb554fa5d24f318e6", "4884a686aba5ec29ca1f51c14e30f0d346c888f0048ad2415b290b1cc5a6968f",
    "b3910254f802d7f00987d123d4d52e63b6a614f53afd908dbd6fd3fe8817262e"};
// The release archive the old model's fallback downloaded (model_store.cpp keeps the name to itself).
const char kLegacyArchive[] = "sherpa-onnx-supertonic-3-tts-int8-2026-05-11.tar.bz2";

// The contents of the generator: bytes((j % (97 + i) + i * 3) & 0xff for j in range(size)).
std::string fakeContent(int i) {
    std::string s(size_t(kSizes[i]), '\0');
    for (size_t j = 0; j < s.size(); ++j) s[j] = char((j % size_t(97 + i) + size_t(i) * 3) & 0xff);
    return s;
}
uint64_t bytesBefore(int file) {
    uint64_t n = 0;
    for (int i = 0; i < file; ++i) n += kSizes[i];
    return n;
}
// The fake model without a source (FakeHosts::manifest() adds its two repositories).
tts::ModelManifest fakeManifest() {
    tts::ModelManifest m;
    for (int i = 0; i < kFiles; ++i) m.files.push_back({kNames[i], kSizes[i], kHashes[i], kRemote[i]});
    return m;
}

bool lowerHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string slurp(const std::string& path) {
    std::string out;
    if (!net::sys::readFile(path, out, size_t(1) << 28)) return "<missing>";
    return out;
}
bool writeFile(const std::string& path, const std::string& data) {
    std::FILE* f = net::sys::openFile(path, "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}
// A file of 'size' bytes made by setting its length, nothing written (sparse where the file system
// allows it): the quick check reads sizes only, so the real models' half a gigabyte need not be written.
bool sizedFile(const std::string& path, uint64_t size) {
    if (!writeFile(path, std::string())) return false;
    std::error_code ec;
    std::filesystem::resize_file(std::filesystem::u8path(path), size, ec);
    return !ec;
}

// A model folder under the test executable's folder, removed with everything in it at both ends
// (some tests leave a directory where a file goes).
struct TempFolder {
    std::string dir;
    explicit TempFolder(const std::string& tag) : dir(net::sys::exeDirectory() + "mstest-" + tag + "/") {
        clean();
        net::sys::makeDirectories(dir);
    }
    ~TempFolder() { clean(); }
    void clean() {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::u8path(dir.substr(0, dir.size() - 1)), ec);
    }
    bool has(const std::string& name) const { return net::sys::fileExists(dir + name); }
};

// The two repositories of the fake manifest, laid out as the real ones, and their labels.
const char* const kRepos[2] = {"/Supertone/supertonic-3/resolve/724fb5ab/", "/supertone-oss-archive/supertonic-3/resolve/aafc6e32/"};
const char* const kLabels[2] = {"127.0.0.1/Supertone/supertonic-3", "127.0.0.1/supertone-oss-archive/supertonic-3"};

// One loopback server playing both repositories and their CDN:
//   <repository>/resolve/<revision>/<path>     small files: 200; larger ones: 302 to the line below
//   /cdn/<source>/<sha256>?X-Amz-Signature=...  the file, honouring Range
// A thread per connection: a job that sent two requests at once would have both served at once.
// Each repository can be told to get a file wrong, by name (404, other bytes, another size, a
// redirect to plain HTTP, a connection cut in the middle, silence), and every request is noted
// with what the job's folder and progress said at that moment.
struct FakeHosts {
    struct Seen {
        int source = 0, file = 0;
        bool cdn = false;
        std::string range;
        bool inOrder = true;              // the folder as a job fetching every file in order leaves it
        bool legacyLeft = false;          // something of the old INT8 model was still in the folder
        tts::DownloadProgress progress;   // of the watched job
    };
    std::mutex mu;
    std::set<std::string> missing[2], corrupt[2], resized[2], insecure[2], cut[2], silent[2];
    std::set<std::string> cutDone[2];
    bool cutEvery = false;   // cut every transfer of a 'cut' file, not only the first
    int pieceDelayMs = 0;
    std::function<void(int source, int file)> onRequest;
    std::vector<Seen> seen;
    // Set by Watch.
    const tts::ModelDownloader* job = nullptr;
    std::string folder;
    bool checkOrder = false;
    fakehttp::Server srv;   // last: its threads start once the rest is ready, and stop before it goes

    FakeHosts() : srv([this](const fakehttp::Request& r) { return answer(r); }, true) {}

    // Lets the server read the job's progress and folder while the guard lives. Declared after the
    // job, it ends first: a request of a cancelled job can still come in once the job is gone.
    // 'inOrder': the job starts from an empty folder, so each request must find every file before
    // its own complete and nothing of the others (no file, no .part).
    struct Watch {
        FakeHosts& h;
        Watch(FakeHosts& hosts, const tts::ModelDownloader& d, bool inOrder) : h(hosts) {
            std::lock_guard<std::mutex> lk(h.mu);
            h.job = &d;
            h.folder = d.folder();
            h.checkOrder = inOrder;
        }
        ~Watch() {
            std::lock_guard<std::mutex> lk(h.mu);
            h.job = nullptr;
            h.checkOrder = false;
        }
    };

    fakehttp::Reply answer(const fakehttp::Request& r) {
        std::lock_guard<std::mutex> lk(mu);
        fakehttp::Reply rep;
        rep.status = 404;
        int src = -1, file = -1;
        bool cdn = false;
        for (int s = 0; s < 2; ++s) {
            const std::string repo = kRepos[s], store = "/cdn/" + std::to_string(s) + "/";
            if (r.path.compare(0, repo.size(), repo) == 0) {
                src = s;
                for (int i = 0; i < kFiles; ++i)
                    if (r.path.substr(repo.size()) == kRemote[i]) file = i;
            } else if (r.path.compare(0, store.size(), store) == 0) {
                src = s;
                cdn = true;
                for (int i = 0; i < kFiles; ++i)
                    if (r.path.compare(store.size(), 64, kHashes[i]) == 0) file = i;
            }
        }
        if (file < 0) return rep;
        note(src, file, cdn, r);
        const std::string name = kNames[file];
        if (!cdn) {
            if (missing[src].count(name)) {
                rep.body = "Entry not found";
                return rep;
            }
            if (insecure[src].count(name)) {
                rep.status = 302;
                rep.headers.emplace_back("Location", std::string("http://example.com/") + kRemote[file]);
                return rep;
            }
            if (kSizes[file] >= 2000) {   // an LFS/Xet file: to the CDN with a signed URL
                rep.status = 302;
                rep.headers.emplace_back("Location", "http://127.0.0.1:" + std::to_string(srv.port()) + "/cdn/" + std::to_string(src) +
                                                         "/" + kHashes[file] + "?X-Amz-Signature=abc&X-Amz-Expires=3600");
                rep.headers.emplace_back("X-Linked-Size", std::to_string(kSizes[file]));
                return rep;
            }
        }
        // The file itself: from the resolve endpoint (a regular git file) or from the CDN.
        std::string data = fakeContent(file);
        if (corrupt[src].count(name)) data[data.size() / 2] ^= 0x40;
        if (resized[src].count(name)) data += '\n';
        rep = fakehttp::fileReply(data, r);
        rep.pieceDelayMs = pieceDelayMs;
        if (silent[src].count(name)) rep.silenceMs = 1500;
        if (cut[src].count(name) && (cutEvery || !cutDone[src].count(name))) {
            cutDone[src].insert(name);
            rep.cutAfter = rep.body.size() / 3;
        }
        return rep;
    }

    void note(int src, int file, bool cdn, const fakehttp::Request& r) {
        Seen s;
        s.source = src;
        s.file = file;
        s.cdn = cdn;
        s.range = r.get("range");
        if (checkOrder) {
            for (int i = 0; i < kFiles; ++i) {
                uint64_t size = 0;
                bool whole = net::sys::fileSize(folder + kNames[i], size) && size == kSizes[i];
                if ((i < file && !whole) || (i >= file && net::sys::fileExists(folder + kNames[i])) ||
                    (i != file && net::sys::fileExists(folder + kNames[i] + ".part")))
                    s.inOrder = false;
            }
        }
        s.legacyLeft = !folder.empty() && tts::legacyFilesPresent(folder);
        if (job) s.progress = job->progress();
        seen.push_back(s);
        if (onRequest) onRequest(src, file);
    }

    std::vector<Seen> log() {
        std::lock_guard<std::mutex> lk(mu);
        return seen;
    }
    int requestsTo(int src) {
        int n = 0;
        for (const Seen& s : log()) n += s.source == src;
        return n;
    }
    void orderCheck(bool on) {
        std::lock_guard<std::mutex> lk(mu);
        checkOrder = on;
    }

    tts::ModelManifest manifest() {
        tts::ModelManifest m = fakeManifest();
        for (int s = 0; s < 2; ++s) m.sources.push_back({srv.url(kRepos[s]), kLabels[s]});
        return m;
    }
};

struct JobRun {
    tts::DownloadProgress last;
    std::set<int> phases;     // those seen by polling: a fast job can pass one unseen, so only absences are checked
    std::vector<int> order;   // the same, in the order seen
    double seconds = 0;
};
tts::ModelDownloader::Options fastOptions() {
    tts::ModelDownloader::Options o;
    o.timeoutMs = 3000;
    o.retryDelayMs = 20;
    o.userAgent = "Scacelith/test";
    return o;
}
JobRun runJob(tts::ModelDownloader& d, const tts::ModelDownloader::Options& o = fastOptions(),
              const std::function<bool(const tts::DownloadProgress&)>& cancelWhen = nullptr) {
    JobRun r;
    auto t0 = std::chrono::steady_clock::now();
    auto seen = [&r](tts::DownloadProgress::Phase ph) {
        r.phases.insert(int(ph));
        if (r.order.empty() || r.order.back() != int(ph)) r.order.push_back(int(ph));
    };
    CHECK(d.start(o));
    while (d.running()) {
        tts::DownloadProgress p = d.progress();
        seen(p.phase);
        if (cancelWhen && cancelWhen(p)) d.cancel();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    d.wait();
    r.last = d.progress();
    seen(r.last.phase);
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}
bool allGood(const TempFolder& t) {
    for (int i = 0; i < kFiles; ++i)
        if (slurp(t.dir + kNames[i]) != fakeContent(i)) return false;
    return true;
}
using Phase = tts::DownloadProgress::Phase;

}  // namespace

TEST(model_store_manifests) {
    // The official release: seven files, flat in the folder, each fetched from its path in the repository.
    const tts::ModelManifest& m = tts::supertonicManifest();
    const char* const names[] = {"tts.json",          "unicode_indexer.json",  "M3.json",     "duration_predictor.onnx",
                                 "text_encoder.onnx", "vector_estimator.onnx", "vocoder.onnx"};
    const char* const remote[] = {"onnx/tts.json",          "onnx/unicode_indexer.json",  "voice_styles/M3.json",
                                  "onnx/duration_predictor.onnx", "onnx/text_encoder.onnx", "onnx/vector_estimator.onnx",
                                  "onnx/vocoder.onnx"};
    const uint64_t sizes[] = {8253, 277676, 290198, 3700147, 36416150, 256534781, 101424195};
    REQUIRE(m.files.size() == 7);
    std::set<std::string> digests;
    for (size_t i = 0; i < 7; ++i) {
        const tts::ManifestFile& f = m.files[i];
        CHECK_EQ(f.name, std::string(names[i]));
        CHECK_EQ(f.remotePath(), std::string(remote[i]));
        CHECK_EQ(f.size, sizes[i]);
        CHECK(lowerHex64(f.sha256));
        CHECK(f.name.find_first_of("/\\") == std::string::npos);
        CHECK(m.find(names[i]) == &f);
        digests.insert(f.sha256);
    }
    CHECK_EQ(digests.size(), size_t(7));   // no digest copied onto another file
    CHECK_EQ(m.totalBytes(), uint64_t(398651400));
    CHECK(!m.find("onnx/tts.json") && !m.find("voice.bin") && !m.find("tts.json.part"));
    // Supertone's repository at the revision its Python SDK pins, then its archive copy.
    REQUIRE(m.sources.size() == 2);
    const std::string first = "https://huggingface.co/Supertone/supertonic-3/resolve/724fb5abbf5502583fb520898d45929e62f02c0b/";
    const std::string second =
        "https://huggingface.co/supertone-oss-archive/supertonic-3/resolve/aafc6e32416a594460b32413efc49d7fe4ce6d46/";
    CHECK_EQ(m.sources[0].base, first);
    CHECK_EQ(m.sources[0].label, std::string("huggingface.co/Supertone/supertonic-3"));
    CHECK_EQ(m.sources[1].base, second);
    CHECK_EQ(m.sources[1].label, std::string("huggingface.co/supertone-oss-archive/supertonic-3"));
    CHECK_EQ(m.url(m.sources[0], *m.find("M3.json")), first + "voice_styles/M3.json");
    CHECK_EQ(m.url(m.sources[1], *m.find("vocoder.onnx")), second + "onnx/vocoder.onnx");
    // A file without a repository path is fetched by its name.
    tts::ManifestFile plain{"voice.bin", 1, std::string(64, '0'), ""};
    CHECK_EQ(plain.remotePath(), std::string("voice.bin"));

    // The old INT8 model: recognised by its files, never downloaded.
    const tts::ModelManifest& old = tts::legacyManifest();
    const char* const oldNames[] = {"LICENSE",  "README.md",           "duration_predictor.int8.onnx", "text_encoder.int8.onnx",
                                    "tts.json", "unicode_indexer.bin", "vector_estimator.int8.onnx",   "vocoder.int8.onnx",
                                    "voice.bin"};
    REQUIRE(old.files.size() == 9);
    for (size_t i = 0; i < 9; ++i) {
        CHECK_EQ(old.files[i].name, std::string(oldNames[i]));
        CHECK_EQ(old.files[i].remotePath(), old.files[i].name);
        CHECK(lowerHex64(old.files[i].sha256));
    }
    CHECK_EQ(old.totalBytes(), uint64_t(145316356));
    CHECK(old.sources.empty());
    // tts.json is the only name the two share, and it is the same file in both: the old model's
    // leftovers are told by its other names.
    for (const tts::ManifestFile& f : old.files) {
        const tts::ManifestFile* same = m.find(f.name);
        CHECK_EQ(same != nullptr, f.name == "tts.json");
        if (same) CHECK(same->size == f.size && same->sha256 == f.sha256);
    }

    CHECK_EQ(std::string(tts::statusName(tts::ModelStatus::Corrupt)), std::string("corrupt"));
    CHECK_EQ(std::string(tts::kindName(tts::ModelKind::Official)), std::string("official"));
    CHECK_EQ(std::string(tts::kindName(tts::ModelKind::Legacy)), std::string("legacy INT8"));
    CHECK_EQ(std::string(tts::kindName(tts::ModelKind::None)), std::string("none"));
    // The folder: <application data>/coach/ unless set (--coach-dir).
    std::string def = tts::modelFolder();
    CHECK(def.size() > 6 && def.substr(def.size() - 6, 5) == "coach");
    CHECK(def.find(net::sys::appDataDirectory()) == 0);
    tts::setModelFolder("/some/where");
    std::string set = tts::modelFolder();
    CHECK(set.size() == 12 && set.compare(0, 11, "/some/where") == 0 && (set[11] == '/' || set[11] == '\\'));
    tts::setModelFolder(std::string());
    CHECK_EQ(tts::modelFolder(), def);
    CHECK(tts::downloadUserAgent().compare(0, 10, "Scacelith/") == 0);
}

TEST(model_store_status_and_verify) {
    const tts::ModelManifest m = fakeManifest();
    TempFolder t("status");
    CHECK_EQ(int(tts::modelStatus(m, t.dir)), int(tts::ModelStatus::Missing));
    CHECK_EQ(int(tts::verifyModel(m, t.dir)), int(tts::ModelStatus::Missing));
    // A .part and the notices are not model files.
    CHECK(writeFile(t.dir + "tts.json.part", fakeContent(0)));
    CHECK(writeFile(t.dir + "README.txt", "notes"));
    CHECK_EQ(int(tts::modelStatus(m, t.dir)), int(tts::ModelStatus::Missing));
    std::vector<std::string> bad;
    for (int i = 0; i < 3; ++i) CHECK(writeFile(t.dir + kNames[i], fakeContent(i)));
    CHECK_EQ(int(tts::modelStatus(m, t.dir)), int(tts::ModelStatus::Incomplete));
    CHECK_EQ(int(tts::verifyModel(m, t.dir, &bad)), int(tts::ModelStatus::Incomplete));
    CHECK(bad.size() == 4 && bad[0] == kNames[3] && bad[3] == kNames[6]);
    for (int i = 3; i < kFiles; ++i) CHECK(writeFile(t.dir + kNames[i], fakeContent(i)));
    CHECK_EQ(int(tts::modelStatus(m, t.dir)), int(tts::ModelStatus::Ready));
    uint64_t lastDone = 0, lastTotal = 0;
    CHECK_EQ(int(tts::verifyModel(m, t.dir, &bad, nullptr, [&](uint64_t d, uint64_t tot) { lastDone = d, lastTotal = tot; })),
             int(tts::ModelStatus::Ready));
    CHECK(bad.empty());
    CHECK_EQ(lastTotal, kTotal);
    CHECK_EQ(lastDone, lastTotal);
    // Same size, other bytes: the quick check cannot see it, the full one does.
    std::string c = fakeContent(6);
    c[100] ^= 1;
    CHECK(writeFile(t.dir + kNames[6], c));
    CHECK_EQ(int(tts::modelStatus(m, t.dir)), int(tts::ModelStatus::Ready));
    CHECK_EQ(int(tts::verifyModel(m, t.dir, &bad)), int(tts::ModelStatus::Corrupt));
    CHECK(bad.size() == 1 && bad[0] == kNames[6]);
    // Another size: incomplete for the quick check; the full one names both files, in the manifest's order.
    CHECK(writeFile(t.dir + kNames[2], "short"));
    CHECK_EQ(int(tts::modelStatus(m, t.dir)), int(tts::ModelStatus::Incomplete));
    CHECK_EQ(int(tts::verifyModel(m, t.dir, &bad)), int(tts::ModelStatus::Corrupt));
    CHECK(bad.size() == 2 && bad[0] == kNames[2] && bad[1] == kNames[6]);
    // A check cancelled before its end knows nothing: never Corrupt.
    for (int i = 0; i < kFiles; ++i) CHECK(writeFile(t.dir + kNames[i], fakeContent(i)));
    std::atomic<bool> stop{true};
    CHECK_EQ(int(tts::verifyModel(m, t.dir, &bad, &stop)), int(tts::ModelStatus::Incomplete));
}

TEST(model_store_installed_model) {
    // Which model the folder holds goes by the real manifests' sizes.
    TempFolder t("kind");
    auto fill = [&](const tts::ModelManifest& m) {
        for (const tts::ManifestFile& f : m.files) CHECK(sizedFile(t.dir + f.name, f.size));
    };
    CHECK_EQ(int(tts::installedModel(t.dir)), int(tts::ModelKind::None));
    fill(tts::legacyManifest());
    CHECK_EQ(int(tts::installedModel(t.dir)), int(tts::ModelKind::Legacy));
    fill(tts::supertonicManifest());   // tts.json, shared, stays as it is
    CHECK_EQ(int(tts::installedModel(t.dir)), int(tts::ModelKind::Official));   // both complete: the official one
    // The official model short of a file, or with one of another size: the old one again.
    CHECK(net::sys::removeFile(t.dir + "vocoder.onnx"));
    CHECK_EQ(int(tts::installedModel(t.dir)), int(tts::ModelKind::Legacy));
    CHECK(sizedFile(t.dir + "vocoder.onnx", 101424195 - 1));
    CHECK_EQ(int(tts::installedModel(t.dir)), int(tts::ModelKind::Legacy));
    // Neither complete.
    CHECK(net::sys::removeFile(t.dir + "voice.bin"));
    CHECK_EQ(int(tts::installedModel(t.dir)), int(tts::ModelKind::None));
    // The official one complete, the old one not.
    CHECK(sizedFile(t.dir + "vocoder.onnx", 101424195));
    CHECK_EQ(int(tts::installedModel(t.dir)), int(tts::ModelKind::Official));
    // The default folder is the one set (--coach-dir).
    tts::setModelFolder(t.dir);
    CHECK_EQ(int(tts::installedModel()), int(tts::ModelKind::Official));
    tts::setModelFolder(std::string());
}

TEST(model_store_old_model_files) {
    TempFolder t("legacy");
    CHECK(!tts::legacyFilesPresent(t.dir));
    // The official model (tts.json included, shared with the old one), its .part files and the
    // notices do not tell an old model.
    for (int i = 0; i < kFiles; ++i) CHECK(writeFile(t.dir + kNames[i], fakeContent(i)));
    for (const char* n : {"tts.json.part", "vocoder.onnx.part", "README.txt", "Supertonic-3-OpenRAIL-M.txt"}) CHECK(writeFile(t.dir + n, "x"));
    CHECK(!tts::legacyFilesPresent(t.dir));
    // Any one of its other files, a .part of one, or the release archive (or its .part) does.
    std::vector<std::string> marks;
    for (const tts::ManifestFile& f : tts::legacyManifest().files) {
        if (f.name == "tts.json") continue;
        marks.push_back(f.name);
        marks.push_back(f.name + ".part");
    }
    marks.push_back(kLegacyArchive);
    marks.push_back(std::string(kLegacyArchive) + ".part");
    CHECK_EQ(marks.size(), size_t(18));
    for (const std::string& n : marks) {
        CHECK(writeFile(t.dir + n, "x"));
        if (!tts::legacyFilesPresent(t.dir)) std::fprintf(stderr, "  %s not seen\n", n.c_str());
        CHECK(tts::legacyFilesPresent(t.dir));
        CHECK(net::sys::removeFile(t.dir + n));
    }
    CHECK(!tts::legacyFilesPresent(t.dir));

    // removeLegacyModel(): every file of the old model (tts.json too), their .part files, the
    // archive and the notices; the official files, their .part files and anything else stay. Small
    // stand-ins of known sizes: the bytes freed are theirs.
    TempFolder t2("legacyremove");
    uint64_t expected = 0;
    std::vector<std::string> gone, kept;
    auto put = [&](const std::string& name, size_t size, bool goes) {
        CHECK(writeFile(t2.dir + name, std::string(size, 'o')));
        (goes ? gone : kept).push_back(name);
        if (goes) expected += size;
    };
    size_t k = 0;
    for (const tts::ManifestFile& f : tts::legacyManifest().files) put(f.name, 100 + 7 * k++, true);
    put("voice.bin.part", 33, true);
    put("tts.json.part", 21, true);
    put(kLegacyArchive, 2000, true);
    put(std::string(kLegacyArchive) + ".part", 1234, true);
    put("README.txt", 900, true);
    put("Supertonic-3-OpenRAIL-M.txt", 1500, true);
    put("vocoder.onnx", 77, false);
    put("M3.json", 55, false);
    put("vector_estimator.onnx.part", 99, false);
    put("notes.txt", 5, false);
    uint64_t removed = 0;
    std::string error;
    CHECK(tts::removeLegacyModel(t2.dir, &removed, &error));
    CHECK_EQ(removed, expected);
    for (const std::string& n : gone) {
        if (t2.has(n)) std::fprintf(stderr, "  %s left\n", n.c_str());
        CHECK(!t2.has(n));
    }
    for (const std::string& n : kept) CHECK(t2.has(n));
    CHECK(!tts::legacyFilesPresent(t2.dir));
    // Nothing left to delete: done, nothing freed.
    CHECK(tts::removeLegacyModel(t2.dir, &removed));
    CHECK_EQ(removed, uint64_t(0));
    for (const std::string& n : kept) CHECK(t2.has(n));
}

TEST(model_store_downloads_file_by_file) {
    FakeHosts hosts;
    TempFolder t("fetch");
    tts::ModelDownloader d(hosts.manifest(), t.dir);
    FakeHosts::Watch w(hosts, d, true);
    CHECK_EQ(d.folder(), t.dir);
    CHECK(!d.running());
    CHECK_EQ(int(d.progress().phase), int(Phase::Idle));
    JobRun r = runJob(d);
    CHECK_EQ(int(r.last.phase), int(Phase::Done));
    CHECK(r.last.finished());
    CHECK_EQ(r.last.error, std::string(""));
    CHECK_EQ(r.last.fetched, kFiles);
    CHECK_EQ(r.last.sourceIndex, 0);
    CHECK_EQ(r.last.sourceLabel, std::string(kLabels[0]));
    CHECK(r.last.firstSourceError.empty() && !r.last.removedLegacy);
    CHECK(!r.phases.count(int(Phase::Removing)));   // nothing of the old model here
    CHECK(allGood(t));
    for (int i = 0; i < kFiles; ++i) CHECK(!t.has(std::string(kNames[i]) + ".part"));
    CHECK_EQ(int(tts::verifyModel(fakeManifest(), t.dir)), int(tts::ModelStatus::Ready));
    // One file after the other in the manifest's order, all from the first repository: each request
    // found the files before its own complete and nothing of the others, and the progress named it.
    std::vector<FakeHosts::Seen> seen = hosts.log();
    CHECK_EQ(seen.size(), size_t(kFiles + 5));   // the five larger files: the resolve endpoint, then the CDN
    std::set<int> files;
    int last = 0;
    for (const FakeHosts::Seen& s : seen) {
        CHECK_EQ(s.source, 0);
        CHECK(s.inOrder);
        CHECK(s.file >= last);
        last = s.file;
        files.insert(s.file);
        CHECK(s.range.empty());
        CHECK_EQ(int(s.progress.phase), int(Phase::Fetching));
        CHECK_EQ(s.progress.file, std::string(kNames[s.file]));
        CHECK_EQ(s.progress.fileIndex, s.file + 1);
        CHECK_EQ(s.progress.fileCount, kFiles);
        CHECK_EQ(s.progress.sourceIndex, 0);
        CHECK_EQ(s.progress.sourceLabel, std::string(kLabels[0]));
        CHECK_EQ(s.progress.done, bytesBefore(s.file));
        CHECK_EQ(s.progress.total, kTotal);
    }
    CHECK_EQ(files.size(), size_t(kFiles));
    // The notices: where the files came from, the licences.
    std::string readme = slurp(t.dir + "README.txt");
    CHECK(readme.find("{source}") == std::string::npos);
    CHECK(readme.find(std::string("https://") + kLabels[0]) != std::string::npos);
    CHECK(readme.find(kLabels[1]) == std::string::npos);
    CHECK(readme.find("does not redistribute") != std::string::npos);
    CHECK(slurp(t.dir + "Supertonic-3-OpenRAIL-M.txt").find("Open RAIL-M") != std::string::npos);
    for (const fakehttp::Request& q : hosts.srv.requests()) {
        CHECK_EQ(q.get("user-agent"), std::string("Scacelith/test"));
        CHECK(!q.has("authorization") && !q.has("cookie"));
    }

    // Again: every file is there and right (checked, kept), nothing is fetched.
    hosts.orderCheck(false);   // from here on the folder holds files after the one requested
    size_t before = hosts.log().size();
    r = runJob(d);
    CHECK_EQ(int(r.last.phase), int(Phase::Done));
    CHECK(!r.phases.count(int(Phase::Fetching)));
    CHECK_EQ(hosts.log().size(), before);
    CHECK_EQ(r.last.fetched, 0);
    CHECK(slurp(t.dir + "README.txt").find("already in this folder") != std::string::npos);

    // A file with other bytes, one of another size and a missing one: those three are fetched again,
    // in the manifest's order (the damaged one is found last, by its digest); the others are kept.
    std::string c = fakeContent(4);
    c[7] ^= 1;
    CHECK(writeFile(t.dir + kNames[4], c));
    CHECK(writeFile(t.dir + kNames[1], "short"));
    CHECK(net::sys::removeFile(t.dir + kNames[6]));
    r = runJob(d);
    CHECK_EQ(int(r.last.phase), int(Phase::Done));
    CHECK(allGood(t));
    CHECK_EQ(r.last.fetched, 3);
    seen = hosts.log();
    const int refetched[] = {1, 4, 6};
    std::vector<int> asked;
    for (size_t i = before; i < seen.size(); ++i) {
        const FakeHosts::Seen& s = seen[i];
        if (asked.empty() || asked.back() != s.file) asked.push_back(s.file);
        int at = int(std::find(std::begin(refetched), std::end(refetched), s.file) - std::begin(refetched));
        CHECK_EQ(s.progress.fileIndex, at + 1);
        CHECK_EQ(s.progress.fileCount, 3);
        uint64_t toCome = 0;   // the bytes not there yet
        for (int j = at; j < 3; ++j) toCome += kSizes[refetched[j]];
        CHECK_EQ(s.progress.done, kTotal - toCome);
    }
    CHECK(asked == std::vector<int>(std::begin(refetched), std::end(refetched)));
}

TEST(model_store_falls_back_to_the_archive_copy) {
    // What makes the job give up the first repository, on one file: that file and the ones after it
    // come from the archive copy (which continues the .part a cut transfer left).
    struct Case {
        const char* what;
        int file;
        std::function<void(FakeHosts&, const std::string&)> setup;
        const char* error;   // firstSourceError
    } cases[] = {
        {"404", 0, [](FakeHosts& h, const std::string& n) { h.missing[0].insert(n); }, "http 404"},
        {"hash mismatch", 6, [](FakeHosts& h, const std::string& n) { h.corrupt[0].insert(n); }, "hash"},
        {"another size", 2, [](FakeHosts& h, const std::string& n) { h.resized[0].insert(n); }, "size"},
        {"redirect to plain HTTP", 4, [](FakeHosts& h, const std::string& n) { h.insecure[0].insert(n); }, "insecure"},
        {"connection cut, again and again", kLarge,
         [](FakeHosts& h, const std::string& n) {
             h.cut[0].insert(n);
             h.cutEvery = true;
         },
         "truncated"},
        {"silence", 3, [](FakeHosts& h, const std::string& n) { h.silent[0].insert(n); }, "timeout"},
    };
    for (const Case& c : cases) {
        FakeHosts hosts;
        c.setup(hosts, kNames[c.file]);
        TempFolder t("fallback");
        tts::ModelDownloader d(hosts.manifest(), t.dir);
        FakeHosts::Watch w(hosts, d, true);
        tts::ModelDownloader::Options o = fastOptions();
        if (std::string(c.error) == "timeout") o.timeoutMs = 300;
        JobRun r = runJob(d, o);
        if (r.last.phase != Phase::Done || r.last.firstSourceError != c.error)
            std::fprintf(stderr, "  %s: '%s' (%s), first source '%s'\n", c.what, r.last.error.c_str(), r.last.detail.c_str(),
                         r.last.firstSourceError.c_str());
        CHECK_EQ(int(r.last.phase), int(Phase::Done));
        CHECK_EQ(r.last.firstSourceError, std::string(c.error));
        CHECK_EQ(r.last.sourceIndex, 1);
        CHECK_EQ(r.last.sourceLabel, std::string(kLabels[1]));
        CHECK_EQ(r.last.fetched, kFiles);
        CHECK(allGood(t));
        // Where each file came from (the last repository asked for it), still one after the other.
        int from[kFiles];
        std::fill(std::begin(from), std::end(from), -1);
        bool continued = false;
        for (const FakeHosts::Seen& s : hosts.log()) {
            CHECK(s.inOrder);
            from[s.file] = s.source;
            CHECK_EQ(s.progress.sourceIndex, s.source);   // the progress panel names the repository in use
            if (s.source == 1 && s.file == c.file && s.cdn && !s.range.empty()) continued = true;
        }
        for (int i = 0; i < kFiles; ++i) CHECK_EQ(from[i], i < c.file ? 0 : 1);
        CHECK_EQ(continued, std::string(c.error) == "truncated");   // only a cut transfer leaves a .part
        // The README names the repositories the files came from.
        std::string readme = slurp(t.dir + "README.txt");
        CHECK(readme.find(kLabels[1]) != std::string::npos);
        CHECK_EQ(readme.find(kLabels[0]) != std::string::npos, c.file > 0);
    }

    // The first repository cannot be reached at all (nothing listens there).
    FakeHosts hosts;
    TempFolder t("unreachable");
    tts::ModelManifest m = hosts.manifest();
    uint16_t closedPort = 0;
    {
        fakehttp::Server gone([](const fakehttp::Request&) { return fakehttp::Reply(); });
        closedPort = gone.port();
    }
    m.sources[0].base = "http://127.0.0.1:" + std::to_string(closedPort) + kRepos[0];
    tts::ModelDownloader d(m, t.dir);
    FakeHosts::Watch w(hosts, d, true);
    JobRun r = runJob(d);
    CHECK_EQ(int(r.last.phase), int(Phase::Done));
    CHECK_EQ(r.last.firstSourceError, std::string("network"));
    CHECK_EQ(r.last.sourceIndex, 1);
    CHECK(allGood(t));
    for (const FakeHosts::Seen& s : hosts.log()) CHECK(s.source == 1 && s.inOrder);
}

TEST(model_store_download_options) {
    // firstSource = 1: the archive copy alone, from the start.
    FakeHosts hosts;
    TempFolder t("archiveonly");
    tts::ModelDownloader d(hosts.manifest(), t.dir);
    tts::ModelDownloader::Options o = fastOptions();
    o.firstSource = 1;
    o.fallback = false;
    JobRun r = runJob(d, o);
    CHECK_EQ(int(r.last.phase), int(Phase::Done));
    CHECK_EQ(r.last.sourceIndex, 1);
    CHECK_EQ(r.last.sourceLabel, std::string(kLabels[1]));
    CHECK(r.last.firstSourceError.empty());
    CHECK_EQ(r.last.fetched, kFiles);
    CHECK(allGood(t));
    CHECK_EQ(hosts.requestsTo(0), 0);
    CHECK(hosts.requestsTo(1) > 0);
    std::string readme = slurp(t.dir + "README.txt");
    CHECK(readme.find(kLabels[1]) != std::string::npos && readme.find(kLabels[0]) == std::string::npos);

    // fallback = false: the first repository's failure ends the job; the archive copy is never asked.
    FakeHosts h2;
    h2.corrupt[0].insert("tts.json");
    TempFolder t2("nofallback");
    tts::ModelDownloader d2(h2.manifest(), t2.dir);
    o = fastOptions();
    o.fallback = false;
    r = runJob(d2, o);
    CHECK_EQ(int(r.last.phase), int(Phase::Failed));
    CHECK_EQ(r.last.error, std::string("hash"));
    CHECK(r.last.detail.find("tts.json") != std::string::npos && r.last.detail.find(kLabels[0]) != std::string::npos);
    CHECK(r.last.firstSourceError.empty());
    CHECK_EQ(r.last.sourceIndex, 0);
    CHECK_EQ(h2.requestsTo(1), 0);
    CHECK(!t2.has("tts.json") && !t2.has("tts.json.part"));

    // The archive copy first and failing: nothing comes after it, fallback or not.
    FakeHosts h3;
    h3.missing[1].insert("M3.json");
    TempFolder t3("archivefails");
    tts::ModelDownloader d3(h3.manifest(), t3.dir);
    o = fastOptions();
    o.firstSource = 1;
    r = runJob(d3, o);
    CHECK_EQ(int(r.last.phase), int(Phase::Failed));
    CHECK_EQ(r.last.error, std::string("http 404"));
    CHECK(r.last.firstSourceError.empty());
    CHECK_EQ(h3.requestsTo(0), 0);

    // A first source past the list: nothing to download from, nothing asked.
    o.firstSource = 2;
    size_t before = h3.log().size();
    r = runJob(d3, o);
    CHECK_EQ(int(r.last.phase), int(Phase::Failed));
    CHECK_EQ(r.last.error, std::string("unavailable"));
    CHECK_EQ(h3.log().size(), before);
}

TEST(model_store_download_failures) {
    // Both repositories fail on the same file: the second one's error. The files completed before
    // it stay, and the next job fetches only the others.
    FakeHosts hosts;
    hosts.missing[0].insert("M3.json");
    hosts.missing[1].insert("M3.json");
    TempFolder t("bothfail");
    tts::ModelDownloader d(hosts.manifest(), t.dir);
    JobRun r = runJob(d);
    CHECK_EQ(int(r.last.phase), int(Phase::Failed));
    CHECK_EQ(r.last.error, std::string("http 404"));
    CHECK_EQ(r.last.firstSourceError, std::string("http 404"));
    CHECK(r.last.detail.find("M3.json") != std::string::npos && r.last.detail.find(kLabels[1]) != std::string::npos);
    CHECK_EQ(r.last.sourceIndex, 1);
    CHECK(slurp(t.dir + kNames[0]) == fakeContent(0) && slurp(t.dir + kNames[1]) == fakeContent(1));
    CHECK(!t.has("M3.json") && !t.has("M3.json.part"));
    for (const FakeHosts::Seen& s : hosts.log()) CHECK(s.file <= 2);   // nothing after it was asked for
    {
        std::lock_guard<std::mutex> lk(hosts.mu);
        hosts.missing[0].clear();
    }
    r = runJob(d);
    CHECK_EQ(int(r.last.phase), int(Phase::Done));
    CHECK_EQ(r.last.fetched, kFiles - 2);
    CHECK(allGood(t));

    // A disk that cannot take a file (a directory where its .part goes): "io", and no use trying
    // the other repository.
    FakeHosts h2;
    TempFolder t2("io");
    CHECK(net::sys::makeDirectories(t2.dir + kNames[4] + ".part/"));
    tts::ModelDownloader d2(h2.manifest(), t2.dir);
    r = runJob(d2);
    CHECK_EQ(int(r.last.phase), int(Phase::Failed));
    CHECK_EQ(r.last.error, std::string("io"));
    CHECK(r.last.firstSourceError.empty());
    CHECK_EQ(r.last.sourceIndex, 0);
    CHECK_EQ(h2.requestsTo(1), 0);
    CHECK(t2.has(kNames[3]) && !t2.has(kNames[4]));

    // The final check finds a file changed under the job (rewritten while the last one was being
    // fetched): "verify", that file deleted, the others kept; the next job fetches it alone.
    FakeHosts h3;
    TempFolder t3("verify");
    h3.onRequest = [&t3](int, int file) {
        if (file != kFiles - 1) return;
        std::string other = fakeContent(0);
        other[10] = char(other[10] ^ 0x55);
        writeFile(t3.dir + kNames[0], other);
    };
    tts::ModelDownloader d3(h3.manifest(), t3.dir);
    r = runJob(d3);
    CHECK_EQ(int(r.last.phase), int(Phase::Failed));
    CHECK_EQ(r.last.error, std::string("verify"));
    CHECK(!t3.has(kNames[0]));
    for (int i = 1; i < kFiles; ++i) CHECK(t3.has(kNames[i]));
    {
        std::lock_guard<std::mutex> lk(h3.mu);
        h3.onRequest = nullptr;
    }
    r = runJob(d3);
    CHECK_EQ(int(r.last.phase), int(Phase::Done));
    CHECK_EQ(r.last.fetched, 1);
    CHECK(allGood(t3));

    // A manifest without a source: nothing to download from...
    TempFolder t4("nosource");
    tts::ModelDownloader d4(fakeManifest(), t4.dir);
    r = runJob(d4);
    CHECK_EQ(int(r.last.phase), int(Phase::Failed));
    CHECK_EQ(r.last.error, std::string("unavailable"));
    // ... unless every file is already there: checked and kept.
    for (int i = 0; i < kFiles; ++i) CHECK(writeFile(t4.dir + kNames[i], fakeContent(i)));
    r = runJob(d4);
    CHECK_EQ(int(r.last.phase), int(Phase::Done));
    CHECK_EQ(r.last.fetched, 0);
    CHECK(slurp(t4.dir + "README.txt").find("already in this folder") != std::string::npos);
}

TEST(model_store_removes_the_old_model_first) {
    // A folder of the old INT8 model (small stand-ins under its names; tts.json, shared, right for
    // the new one), its notices, the .part of its release archive, one file of the new model
    // already right, and a file of the player's.
    FakeHosts hosts;
    TempFolder t("replace");
    for (const tts::ManifestFile& f : tts::legacyManifest().files)
        if (f.name != "tts.json") CHECK(writeFile(t.dir + f.name, "old " + f.name));
    CHECK(writeFile(t.dir + "tts.json", fakeContent(0)));
    CHECK(writeFile(t.dir + kLegacyArchive + std::string(".part"), "part of the archive"));
    CHECK(writeFile(t.dir + "README.txt", "the old notice"));
    CHECK(writeFile(t.dir + "Supertonic-3-OpenRAIL-M.txt", "the old licence"));
    CHECK(writeFile(t.dir + kNames[6], fakeContent(6)));
    CHECK(writeFile(t.dir + "notes.txt", "mine"));
    REQUIRE(tts::legacyFilesPresent(t.dir));
    tts::ModelDownloader d(hosts.manifest(), t.dir);
    FakeHosts::Watch w(hosts, d, false);
    JobRun r = runJob(d);
    CHECK_EQ(int(r.last.phase), int(Phase::Done));
    CHECK(r.last.removedLegacy);
    CHECK(allGood(t));
    // Everything of the old model went before the first request, tts.json with it (fetched again);
    // the new model's file stayed.
    CHECK_EQ(r.last.fetched, kFiles - 1);
    std::vector<FakeHosts::Seen> seen = hosts.log();
    REQUIRE(!seen.empty());
    CHECK_EQ(seen.front().file, 0);
    for (const FakeHosts::Seen& s : seen) {
        CHECK(!s.legacyLeft);
        CHECK(s.file != kFiles - 1);
    }
    // The removal is the job's first phase: once the job was seen fetching, never again.
    auto removing = std::find(r.order.begin(), r.order.end(), int(Phase::Removing));
    auto fetching = std::find(r.order.begin(), r.order.end(), int(Phase::Fetching));
    CHECK(fetching != r.order.end());
    CHECK(removing == r.order.end() || removing < fetching);
    CHECK(!tts::legacyFilesPresent(t.dir));
    for (const tts::ManifestFile& f : tts::legacyManifest().files)
        if (f.name != "tts.json") CHECK(!t.has(f.name));
    CHECK(!t.has(kLegacyArchive + std::string(".part")));
    CHECK_EQ(slurp(t.dir + "notes.txt"), std::string("mine"));
    // The notices are the new model's.
    CHECK(slurp(t.dir + "README.txt").find(kLabels[0]) != std::string::npos);
    CHECK(slurp(t.dir + "Supertonic-3-OpenRAIL-M.txt").find("Open RAIL-M") != std::string::npos);
    // The next job finds nothing of the old model.
    r = runJob(d);
    CHECK_EQ(int(r.last.phase), int(Phase::Done));
    CHECK(!r.last.removedLegacy);
    CHECK(!r.phases.count(int(Phase::Removing)));
    CHECK_EQ(r.last.fetched, 0);
}

TEST(model_store_resumes_and_cancels) {
    // A connection cut in the middle of a large file: continued with Range, from the same repository.
    FakeHosts hosts;
    hosts.cut[0].insert(kNames[kLarge]);
    TempFolder t("resume");
    tts::ModelDownloader d(hosts.manifest(), t.dir);
    JobRun r = runJob(d);
    CHECK_EQ(int(r.last.phase), int(Phase::Done));
    CHECK(r.last.firstSourceError.empty());
    CHECK(allGood(t));
    bool ranged = false;
    for (const FakeHosts::Seen& s : hosts.log()) {
        CHECK_EQ(s.source, 0);
        if (s.file == kLarge && s.cdn && s.range == "bytes=" + std::to_string(kSizes[kLarge] / 3) + "-") ranged = true;
    }
    CHECK(ranged);

    // The repository keeps breaking off and there is no fallback: the job fails, the .part stays
    // and the next job continues it (the files before it are kept).
    FakeHosts flaky;
    flaky.cut[0].insert(kNames[kLarge]);
    flaky.cutEvery = true;
    TempFolder t4("keeppart");
    tts::ModelDownloader d4(flaky.manifest(), t4.dir);
    tts::ModelDownloader::Options o = fastOptions();
    o.fallback = false;
    r = runJob(d4, o);
    CHECK_EQ(int(r.last.phase), int(Phase::Failed));
    CHECK_EQ(r.last.error, std::string("truncated"));
    uint64_t kept = 0;
    CHECK(net::sys::fileSize(t4.dir + kNames[kLarge] + ".part", kept) && kept > 0 && kept < kSizes[kLarge]);
    {
        std::lock_guard<std::mutex> lk(flaky.mu);
        flaky.cut[0].clear();
    }
    size_t before = flaky.log().size();
    r = runJob(d4, o);
    CHECK_EQ(int(r.last.phase), int(Phase::Done));
    CHECK_EQ(r.last.fetched, 2);
    CHECK(allGood(t4));
    bool fromKept = false;
    std::vector<FakeHosts::Seen> seen = flaky.log();
    for (size_t i = before; i < seen.size(); ++i)
        if (seen[i].file == kLarge && seen[i].cdn && seen[i].range == "bytes=" + std::to_string(kept) + "-") fromKept = true;
    CHECK(fromKept);

    // Cancel while a slow file comes in: Cancelled at once, its .part kept, nothing after it
    // started; the next job continues it.
    FakeHosts slow;
    slow.pieceDelayMs = 30;
    TempFolder t2("cancel");
    tts::ModelDownloader d2(slow.manifest(), t2.dir);
    const std::string part = t2.dir + kNames[kLarge] + ".part";
    std::chrono::steady_clock::time_point asked{};
    r = runJob(d2, fastOptions(), [&](const tts::DownloadProgress& p) {
        uint64_t s = 0;
        if (asked == std::chrono::steady_clock::time_point{} && p.phase == Phase::Fetching && p.file == kNames[kLarge] &&
            net::sys::fileSize(part, s) && s > 20000) {
            asked = std::chrono::steady_clock::now();
            return true;
        }
        return false;
    });
    double stopMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - asked).count();
    CHECK_EQ(int(r.last.phase), int(Phase::Cancelled));
    CHECK_EQ(r.last.error, std::string("cancelled"));
    CHECK(stopMs < 600.0);
    CHECK(net::sys::fileExists(part));
    CHECK(!t2.has(kNames[kLarge]) && !t2.has(kNames[kFiles - 1]) && !t2.has(std::string(kNames[kFiles - 1]) + ".part"));
    {
        std::lock_guard<std::mutex> lk(slow.mu);
        slow.pieceDelayMs = 0;
    }
    before = slow.log().size();
    r = runJob(d2);
    CHECK_EQ(int(r.last.phase), int(Phase::Done));
    CHECK_EQ(r.last.fetched, 2);
    CHECK(allGood(t2));
    bool continued = false;
    seen = slow.log();
    for (size_t i = before; i < seen.size(); ++i)
        if (seen[i].file == kLarge && seen[i].cdn && !seen[i].range.empty()) continued = true;
    CHECK(continued);

    // One job at a time, and the destructor of a running job cancels and joins.
    FakeHosts slow2;
    slow2.pieceDelayMs = 50;
    TempFolder t3("destroy");
    auto t0 = std::chrono::steady_clock::now();
    {
        tts::ModelDownloader d3(slow2.manifest(), t3.dir);
        CHECK(d3.start(fastOptions()));
        CHECK(!d3.start(fastOptions()));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 1.5);
}

// The development copy of the real model the build prepares in <build>/coach/
// (third_party/supertonic3): the manifest's sizes and digests against the real files.
TEST(model_store_real_files) {
    const std::string dir = net::sys::exeDirectory() + "coach/";
    const tts::ModelManifest& m = tts::supertonicManifest();
    // Required with --require=tts-model (tests/test.h; the CI's Linux job).
    if (tts::modelStatus(m, dir) == tts::ModelStatus::Missing) SKIP_WITHOUT("tts-model", "no model files in <build>/coach/");
    testing::uses("tts-model");
    CHECK_EQ(int(tts::modelStatus(m, dir)), int(tts::ModelStatus::Ready));
    CHECK_EQ(int(tts::installedModel(dir)), int(tts::ModelKind::Official));
    std::vector<std::string> bad;
    CHECK_EQ(int(tts::verifyModel(m, dir, &bad)), int(tts::ModelStatus::Ready));
    for (const std::string& b : bad) std::fprintf(stderr, "  %s does not match the manifest\n", b.c_str());
}

TEST(coach_voice_retry_after_a_download) {
    // A worker that failed (to load, or its warm-up) gets one more try after a download that wrote
    // files; one that found every file right changes nothing, and a working voice is left alone.
    CHECK(game::coachVoiceRetry(true, 1));
    CHECK(game::coachVoiceRetry(true, 9));
    CHECK(!game::coachVoiceRetry(true, 0));
    CHECK(!game::coachVoiceRetry(false, 2));
    CHECK(!game::coachVoiceRetry(false, 0));
}

// [coach] voice: on by default, kept off once the player declined the download.
TEST(coach_voice_setting_round_trip) {
    game::Settings fresh;
    CHECK(fresh.coachVoice);
    IniFile empty;
    game::readCoachSettings(empty, fresh);
    CHECK(fresh.coachVoice);   // a file from before the key: on
    game::Settings off;
    off.coachVoice = false;
    IniFile out;
    game::writeCoachSettings(out, off);
    CHECK_EQ(out.getString("coach.voice"), std::string("false"));
    game::Settings back;
    game::readCoachSettings(out, back);
    CHECK(!back.coachVoice);
    // [coach] voice_update_offered: the start-up offer of the official model, made once.
    CHECK(!fresh.coachVoiceUpdateOffered);
    game::Settings offered;
    offered.coachVoiceUpdateOffered = true;
    IniFile out2;
    game::writeCoachSettings(out2, offered);
    game::Settings back2;
    game::readCoachSettings(out2, back2);
    CHECK(back2.coachVoiceUpdateOffered);
}

// [coach] voice_offered and the [analysis] toggles: off / on by default, kept.
TEST(analysis_settings_round_trip) {
    game::Settings fresh;
    IniFile empty;
    game::readCoachSettings(empty, fresh);
    CHECK(!fresh.coachVoiceOffered);
    CHECK(fresh.analysisComments && fresh.analysisVoice && fresh.analysisArrows);
    game::Settings s;
    s.coachVoiceOffered = true;
    s.analysisComments = false;
    s.analysisArrows = false;
    IniFile out;
    game::writeCoachSettings(out, s);
    game::Settings back;
    game::readCoachSettings(out, back);
    CHECK(back.coachVoiceOffered);
    CHECK(!back.analysisComments);
    CHECK(back.analysisVoice);
    CHECK(!back.analysisArrows);
}

// The real thing, once, by hand (needs the network and about 400 MB; not run by default):
//   SCACELITH_NET_TESTS=1 ./scacelith_tests model_store_real_download
// downloads the seven files from Supertone's repository into a temporary folder and checks them;
// SCACELITH_NET_TESTS=archive takes them from the archive copy alone.
TEST(model_store_real_download) {
    const char* env = std::getenv("SCACELITH_NET_TESTS");
    if (!env || !*env) SKIP("SCACELITH_NET_TESTS not set");
    TempFolder t("real");
    tts::ModelDownloader d(tts::supertonicManifest(), t.dir);
    tts::ModelDownloader::Options o;
    if (std::string(env) == "archive") {
        o.firstSource = 1;
        o.fallback = false;
    }
    auto t0 = std::chrono::steady_clock::now();
    CHECK(d.start(o));
    Phase lastPhase = Phase::Idle;
    std::string lastFile;
    while (d.running()) {
        tts::DownloadProgress p = d.progress();
        if (p.phase != lastPhase || p.file != lastFile) {
            double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::fprintf(stderr, "  %6.1f s: phase %d %s %s\n", s, int(p.phase), p.sourceLabel.c_str(), p.file.c_str());
            lastPhase = p.phase;
            lastFile = p.file;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    d.wait();
    tts::DownloadProgress p = d.progress();
    std::fprintf(stderr, "  result: phase %d error '%s' %s; first source error '%s'; %.1f s\n", int(p.phase), p.error.c_str(),
                 p.detail.c_str(), p.firstSourceError.c_str(),
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    CHECK_EQ(int(p.phase), int(Phase::Done));
    CHECK_EQ(int(tts::verifyModel(tts::supertonicManifest(), t.dir)), int(tts::ModelStatus::Ready));
}
