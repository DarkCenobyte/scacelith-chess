#include "model.h"
#include "net/json.h"
#include "net/net_sys.h"
#include <cmath>
#include <cstring>

namespace tts {

const Engine::Layout Engine::kOfficial = {
    {"duration_predictor.onnx", "text_encoder.onnx", "vector_estimator.onnx", "vocoder.onnx"},
    "unicode_indexer.json", "M3.json", "tts.json"};
const Engine::Layout Engine::kLegacy = {
    {"duration_predictor.int8.onnx", "text_encoder.int8.onnx", "vector_estimator.int8.onnx", "vocoder.int8.onnx"},
    "unicode_indexer.bin", "voice.bin", ""};

namespace {

constexpr int kIndexerSize = 65536;   // the Basic Multilingual Plane
constexpr int64_t kTtlRows = 50, kTtlCols = 256, kDpRows = 8, kDpCols = 16;
const char* const kLegacyVoices[10] = {"F1", "F2", "F3", "F4", "F5", "M1", "M2", "M3", "M4", "M5"};

bool fail(std::string* e, const std::string& msg) {
    if (e) *e = msg;
    return false;
}

Tensor ones(const Dims& d) {
    Tensor t = Tensor::alloc(DType::F32, d);
    for (int64_t i = 0; i < t.count(); ++i) t.mut<float>()[i] = 1.0f;
    return t;
}

Tensor idsTensor(const std::vector<int64_t>& ids) {
    Tensor t = Tensor::alloc(DType::I64, {1, int64_t(ids.size())});
    if (!ids.empty()) std::memcpy(t.mut<int64_t>(), ids.data(), ids.size() * 8);
    return t;
}

Tensor scalar1(float v) {
    Tensor t = Tensor::alloc(DType::F32, {1});
    *t.mut<float>() = v;
    return t;
}

bool setInput(Session& s, const Graph& g, const char* name, Tensor t, std::string* error) {
    int i = g.inputIndex(name);
    if (i < 0) return fail(error, g.label() + ": no input " + name);
    s.setInput(i, std::move(t));
    return true;
}

// An integer member of a JSON object ("ae" -> "sample_rate"), or 'def' when absent or not a number.
int64_t member(const net::json::Value& v, const char* a, const char* b, int64_t def = -1) {
    const net::json::Value& x = v[a][b];
    return x.isNumber() ? x.asInt() : def;
}

// The style tensor 'name' of a voice style document: {"dims": [1, rows, cols], "data": nested
// arrays of rows x cols numbers, any nesting}, appended to 'out'.
bool readStyle(const net::json::Value& doc, const char* name, int64_t rows, int64_t cols, std::vector<float>& out,
               std::string* error) {
    const net::json::Value& st = doc[name];
    const net::json::Value& dims = st["dims"];
    if (!dims.isArray() || dims.size() != 3 || dims[0].asInt() != 1 || dims[1].asInt() != rows || dims[2].asInt() != cols)
        return fail(error, std::string("voice style: ") + name + " is not [1, " + std::to_string(rows) + ", " +
                               std::to_string(cols) + "]");
    if (st.has("type") && st["type"].asString() != "float32")
        return fail(error, std::string("voice style: ") + name + " is not float32");
    size_t start = out.size(), want = size_t(rows * cols);
    // Depth-first over the nested arrays; any shape that flattens to rows x cols numbers (as
    // numpy's flatten() in the official helper).
    std::vector<const net::json::Value*> stack{&st["data"]};
    if (!stack.back()->isArray()) return fail(error, std::string("voice style: ") + name + " has no data");
    while (!stack.empty()) {
        const net::json::Value* v = stack.back();
        stack.pop_back();
        if (v->isArray()) {
            for (size_t i = v->size(); i-- > 0;) stack.push_back(&(*v)[i]);
        } else if (v->isNumber() && std::isfinite(v->asNumber()) && out.size() - start < want) {
            out.push_back(float(v->asNumber()));
        } else {
            return fail(error, std::string("voice style: ") + name + " has a value that is not a finite number, or too many");
        }
    }
    if (out.size() - start != want) return fail(error, std::string("voice style: ") + name + " has too few values");
    return true;
}

}  // namespace

// ---- Assets -----------------------------------------------------------------------------------------

bool Engine::parseConfig(const std::string& text, std::string* error) {
    net::json::Value v;
    std::string err;
    if (!net::json::parse(text, v, &err)) return fail(error, "tts.json: " + err);
    // The constants compiled into this file (and tts.cpp) must be the model's.
    if (member(v, "ae", "sample_rate") != kSampleRate || member(v, "ae", "base_chunk_size") != kFrameSamples / 6 ||
        member(v, "ttl", "latent_dim") != kLatentChannels / 6 || member(v, "ttl", "chunk_compress_factor") != 6)
        return fail(error, "tts.json: not the configuration this runtime was written for");
    return true;
}

bool Engine::parseIndexerJson(const std::string& text, std::vector<int32_t>& out, std::string* error) {
    net::json::Value v;
    net::json::Limits lim;
    lim.maxElements = kIndexerSize + 16;
    std::string err;
    if (!net::json::parse(text, v, &err, lim)) return fail(error, "unicode_indexer.json: " + err);
    if (!v.isArray() || v.size() != size_t(kIndexerSize))
        return fail(error, "unicode_indexer.json: not an array of " + std::to_string(kIndexerSize) + " numbers");
    out.assign(size_t(kIndexerSize), -1);
    for (size_t i = 0; i < out.size(); ++i) {
        const net::json::Value& e = v[i];
        double d = e.asNumber(-2.0);
        if (!e.isNumber() || d != std::floor(d) || d < -1.0 || d >= double(1 << 24))
            return fail(error, "unicode_indexer.json: entry " + std::to_string(i) + " is not an id");
        out[i] = int32_t(d);
    }
    return true;
}

bool Engine::parseIndexerBin(const uint8_t* data, size_t size, std::vector<int32_t>& out, std::string* error) {
    if (size != size_t(kIndexerSize) * 4) return fail(error, "unicode_indexer.bin: unexpected size");
    out.resize(size_t(kIndexerSize));
    std::memcpy(out.data(), data, size);
    return true;
}

bool Engine::parseVoiceJson(const std::string& text, std::vector<float>& ttl, std::vector<float>& dp, std::string* error) {
    net::json::Value v;
    net::json::Limits lim;
    lim.maxElements = 20000;   // 12,928 numbers and their arrays
    std::string err;
    if (!net::json::parse(text, v, &err, lim)) return fail(error, "voice style: " + err);
    size_t t0 = ttl.size(), d0 = dp.size();
    if (!readStyle(v, "style_ttl", kTtlRows, kTtlCols, ttl, error) || !readStyle(v, "style_dp", kDpRows, kDpCols, dp, error)) {
        ttl.resize(t0);
        dp.resize(d0);
        return false;
    }
    return true;
}

bool Engine::parseVoiceBin(const uint8_t* data, size_t size, std::vector<float>& ttl, std::vector<float>& dp, int* count,
                           std::string* error) {
    if (size < 48) return fail(error, "voice.bin: too small");
    int64_t h[6];
    std::memcpy(h, data, 48);
    if (h[0] <= 0 || h[0] != h[3] || h[1] != kTtlRows || h[2] != kTtlCols || h[4] != kDpRows || h[5] != kDpCols)
        return fail(error, "voice.bin: unexpected header");
    // The count must be the one the size gives (multiplying the header's could overflow).
    const uint64_t perVoice = 4 * (kTtlRows * kTtlCols + kDpRows * kDpCols);
    if ((size - 48) % perVoice != 0 || uint64_t(h[0]) != (size - 48) / perVoice) return fail(error, "voice.bin: unexpected size");
    size_t nTtl = size_t(h[0] * kTtlRows * kTtlCols), nDp = size_t(h[0] * kDpRows * kDpCols);
    ttl.resize(nTtl);
    dp.resize(nDp);
    std::memcpy(ttl.data(), data + 48, nTtl * 4);
    std::memcpy(dp.data(), data + 48 + nTtl * 4, nDp * 4);
    *count = int(h[0]);
    return true;
}

// ---- Loading ----------------------------------------------------------------------------------------

bool Engine::loadDirectory(const std::string& dir, const kern::Table& k, std::string* error, ModelKind kind) {
    loaded_ = false;
    std::string base = dir;
    if (!base.empty() && base.back() != '/' && base.back() != '\\') base += '/';
    auto complete = [&base](const Layout& l) {
        for (const char* g : l.graphs)
            if (!net::sys::fileExists(base + g)) return false;
        return net::sys::fileExists(base + l.indexer) && net::sys::fileExists(base + l.voices);
    };
    if (kind == ModelKind::None) kind = !complete(kOfficial) && complete(kLegacy) ? ModelKind::Legacy : ModelKind::Official;
    const Layout& l = layout(kind);
    kind_ = kind;
    indexer_.clear();
    ttl_.clear();
    dpStyle_.clear();
    voiceNames_.clear();
    assetBytes_ = 0;
    auto read = [&](const char* name, std::string& out) {
        if (net::sys::readFile(base + name, out, size_t(4) << 20)) {
            assetBytes_ += out.size();
            return true;
        }
        return fail(error, std::string(name) + ": cannot read " + base + name);
    };
    std::string text;
    if (kind == ModelKind::Official) {
        if (!read(l.config, text) || !parseConfig(text, error)) return false;
        if (!read(l.indexer, text) || !parseIndexerJson(text, indexer_, error)) return false;
        if (!read(l.voices, text) || !parseVoiceJson(text, ttl_, dpStyle_, error)) return false;
        std::string voice = l.voices;
        voiceNames_.push_back(voice.substr(0, voice.rfind('.')));   // "M3"
    } else {
        int count = 0;
        if (!read(l.indexer, text) ||
            !parseIndexerBin(reinterpret_cast<const uint8_t*>(text.data()), text.size(), indexer_, error))
            return false;
        if (!read(l.voices, text) ||
            !parseVoiceBin(reinterpret_cast<const uint8_t*>(text.data()), text.size(), ttl_, dpStyle_, &count, error))
            return false;
        // sherpa-onnx's generate_voices_bin.py packs the voice JSON files sorted by name: F1..F5, M1..M5.
        for (int i = 0; i < count; ++i) voiceNames_.push_back(count == 10 ? kLegacyVoices[i] : "V" + std::to_string(i + 1));
    }
    for (int i = 0; i < kGraphCount; ++i)
        if (!files_[i].open(base + l.graphs[i], error)) return false;
    if (!buildGraphs(k, error)) return false;
    loaded_ = true;
    return true;
}

bool Engine::buildGraphs(const kern::Table& k, std::string* error) {
    const Blob b[kGraphCount] = {{files_[0].data(), files_[0].size()},
                                 {files_[1].data(), files_[1].size()},
                                 {files_[2].data(), files_[2].size()},
                                 {files_[3].data(), files_[3].size()}};
    // The parser's errors do not name the graph: the log and the tests need to know which one.
    auto load = [&](Graph& g, const Blob& blob, const char* label, const std::vector<std::string>& invariant) {
        if (g.load(blob.data, blob.size, label, invariant, k, error)) return true;
        if (error && error->compare(0, std::strlen(label), label) != 0) *error = std::string(label) + ": " + *error;
        return false;
    };
    if (!load(dp_, b[kFileDuration], "duration predictor", {})) return false;
    if (!load(te_, b[kFileTextEncoder], "text encoder", {})) return false;
    if (!load(ve_, b[kFileVectorEstimator], "vector estimator",
              {"text_emb", "style_ttl", "text_mask", "latent_mask", "total_step"}))
        return false;
    if (!load(voc_, b[kFileVocoder], "vocoder", {})) return false;
    struct Io {
        const Graph* g;
        const char* names[7];
    };
    const Io io[] = {{&dp_, {"text_ids", "style_dp", "text_mask"}},
                     {&te_, {"text_ids", "style_ttl", "text_mask"}},
                     {&ve_, {"noisy_latent", "text_emb", "style_ttl", "latent_mask", "text_mask", "current_step",
                             "total_step"}},
                     {&voc_, {"latent"}}};
    for (const Io& e : io) {
        for (const char* n : e.names)
            if (n && e.g->inputIndex(n) < 0) return fail(error, e.g->label() + ": no input " + n);
        if (e.g->outputCount() != 1) return fail(error, e.g->label() + ": expected one output");
    }
    return true;
}

std::string Engine::voiceName(int i) const {
    return i >= 0 && i < voiceCount() ? voiceNames_[size_t(i)] : std::string();
}

int Engine::voiceIndex(const std::string& name) const {
    for (size_t i = 0; i < voiceNames_.size(); ++i)
        if (voiceNames_[i] == name) return int(i);
    return -1;
}

size_t Engine::modelBytes() const {
    size_t n = assetBytes_;
    for (const MappedFile& f : files_) n += f.size();
    return n;
}

const Graph& Engine::graph(ModelFile f) const {
    switch (f) {
    case kFileDuration: return dp_;
    case kFileTextEncoder: return te_;
    case kFileVectorEstimator: return ve_;
    default: return voc_;
    }
}

Tensor Engine::styleTtl(int voice) const {
    return Tensor::view(DType::F32, {1, kTtlRows, kTtlCols}, ttl_.data() + int64_t(voice) * kTtlRows * kTtlCols);
}

Tensor Engine::styleDp(int voice) const {
    return Tensor::view(DType::F32, {1, kDpRows, kDpCols}, dpStyle_.data() + int64_t(voice) * kDpRows * kDpCols);
}

bool Engine::duration(const std::vector<int64_t>& ids, int voice, const ExecContext& ctx, float* seconds,
                      std::string* error, const std::atomic<bool>* cancel) const {
    Session s(dp_);
    int64_t T = int64_t(ids.size());
    if (!setInput(s, dp_, "text_ids", idsTensor(ids), error) || !setInput(s, dp_, "style_dp", styleDp(voice), error) ||
        !setInput(s, dp_, "text_mask", ones({1, 1, T}), error) || !s.run(ctx, error, cancel))
        return false;
    const Tensor& d = s.output(0);
    if (d.type != DType::F32 || d.count() < 1) return fail(error, "duration predictor: bad output");
    *seconds = d.scalarFloat();
    return true;
}

bool Engine::encode(const std::vector<int64_t>& ids, int voice, const ExecContext& ctx, Tensor* textEmb,
                    std::string* error, const std::atomic<bool>* cancel) const {
    Session s(te_);
    int64_t T = int64_t(ids.size());
    if (!setInput(s, te_, "text_ids", idsTensor(ids), error) ||
        !setInput(s, te_, "style_ttl", styleTtl(voice), error) ||
        !setInput(s, te_, "text_mask", ones({1, 1, T}), error) || !s.run(ctx, error, cancel))
        return false;
    *textEmb = s.output(0);
    if (textEmb->rank() != 3 || textEmb->dims[2] != T) return fail(error, "text encoder: bad output shape");
    return true;
}

bool Engine::denoise(const Tensor& textEmb, int voice, const Tensor& noise, int steps, const ExecContext& ctx,
                     Tensor* latent, const std::atomic<bool>* cancel, std::string* error,
                     std::vector<Tensor>* each) const {
    if (noise.rank() != 3 || noise.dims[1] != kLatentChannels) return fail(error, "noise shape");
    if (textEmb.rank() != 3 || textEmb.type != DType::F32) return fail(error, "text embedding shape");
    int64_t T = textEmb.dims[2], L = noise.dims[2];
    Session s(ve_);
    if (!setInput(s, ve_, "text_emb", textEmb, error) || !setInput(s, ve_, "style_ttl", styleTtl(voice), error) ||
        !setInput(s, ve_, "text_mask", ones({1, 1, T}), error) ||
        !setInput(s, ve_, "latent_mask", ones({1, 1, L}), error) ||
        !setInput(s, ve_, "total_step", scalar1(float(steps)), error))
        return false;
    Tensor x = noise;
    for (int step = 0; step < steps; ++step) {
        if (!setInput(s, ve_, "noisy_latent", x, error) || !setInput(s, ve_, "current_step", scalar1(float(step)), error))
            return false;
        if (!s.run(ctx, error, cancel)) return false;
        x = s.output(0);
        if (each) each->push_back(x);
    }
    *latent = x;
    return true;
}

bool Engine::vocode(const Tensor& latent, const ExecContext& ctx, Tensor* wav, std::string* error,
                    const std::atomic<bool>* cancel) const {
    if (latent.rank() != 3 || latent.dims[1] != kLatentChannels) return fail(error, "latent shape");
    Session s(voc_);
    if (!setInput(s, voc_, "latent", latent, error) || !s.run(ctx, error, cancel)) return false;
    *wav = s.output(0);
    if (wav->type != DType::F32) return fail(error, "vocoder: bad output");
    return true;
}

}  // namespace tts
