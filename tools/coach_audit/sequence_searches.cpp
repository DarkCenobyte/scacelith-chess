// coach_audit review --vcache FILE: the searches a Coach game runs to confirm the sequences its
// reasons show (coach/review.h: ReviewInput::verified), each position searched once with the
// embedded Stockfish and kept in FILE, so that a rerun with the same reviews searches nothing.
#include "audit.h"

#include "ai/engine.h"
#include "core/files.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

namespace audit {

struct SequenceSearches::Impl {
    ai::Engine engine;
    bool ok = false;
    std::string cachePath;
    std::map<std::string, ai::Analysis> cache;
    int searched = 0, unsaved = 0;
    ai::Analysis failed;   // what get() returns without an engine or a cached search

    // The whole cache, rewritten (mode 0644 whatever the umask: files::create).
    void save() {
        if (cachePath.empty() || unsaved == 0) return;
        if (FILE* f = files::create(cachePath.c_str())) {
            for (const auto& kv : cache) {
                const std::string block = writeSearch(kv.first, kv.second);
                std::fwrite(block.data(), 1, block.size(), f);
            }
            std::fclose(f);
        }
        unsaved = 0;
    }
};

SequenceSearches::SequenceSearches(int threads, const std::string& cachePath) : d(new Impl) {
    d->cachePath = cachePath;
    {
        std::ifstream in(cachePath, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string err;
        if (!readSearches(ss.str(), d->cache, err))
            std::fprintf(stderr, "%s: %s (the searches before it are kept)\n", cachePath.c_str(), err.c_str());
    }
    d->ok = d->engine.start() && d->engine.waitReady(60000);
    if (d->ok) {
        ai::EngineSettings s;
        s.hashMB = 64;
        s.threads = threads;
        d->engine.configure(s);
    }
}

SequenceSearches::~SequenceSearches() {
    d->save();
    if (d->ok) d->engine.shutdown();
}

bool SequenceSearches::ok() const { return d->ok; }

int SequenceSearches::searched() const { return d->searched; }

const ai::Analysis& SequenceSearches::get(const std::string& key) {
    const auto hit = d->cache.find(key);
    if (hit != d->cache.end()) return hit->second;
    if (!d->ok) return d->failed;
    ai::AnalysisRequest r = coach::Reviewer::verifyRequest(key);
    r.moveTimeMs = 0;   // depth only, from a cleared hash table: reproducible
    d->engine.clearHash();
    ai::Analysis a;
    const uint32_t id = d->engine.requestAnalysis(r);
    const auto t0 = std::chrono::steady_clock::now();
    bool done = true;
    while (!d->engine.analysisReady(id)) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::minutes(5)) {
            done = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!done || !d->engine.takeAnalysis(id, a)) a.ok = false;
    ++d->searched;
    const ai::Analysis& kept = d->cache.emplace(key, a).first->second;
    if (++d->unsaved >= 100) d->save();   // a run stopped midway keeps most of what it searched
    return kept;
}

}  // namespace audit
