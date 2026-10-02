#include "model.h"
#include <cstring>

namespace tts {

const char* const Engine::kFiles[kFileCount] = {"duration_predictor.int8.onnx", "text_encoder.int8.onnx",
                                                "vector_estimator.int8.onnx",   "vocoder.int8.onnx",
                                                "unicode_indexer.bin",          "voice.bin"};

namespace {

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

}  // namespace

bool Engine::loadDirectory(const std::string& dir, const kern::Table& k, std::string* error) {
    Blob b[kFileCount];
    std::string base = dir;
    if (!base.empty() && base.back() != '/' && base.back() != '\\') base += '/';
    for (int i = 0; i < kFileCount; ++i) {
        if (!files_[i].open(base + kFiles[i], error)) return false;
        b[i].data = files_[i].data();
        b[i].size = files_[i].size();
    }
    return build(b, k, error);
}

bool Engine::build(const Blob blobs[kFileCount], const kern::Table& k, std::string* error) {
    loaded_ = false;
    for (int i = 0; i < kFileCount; ++i) blobs_[i] = blobs[i];
    const Blob& ix = blobs[kFileIndexer];
    if (ix.size != 65536 * 4 || reinterpret_cast<uintptr_t>(ix.data) % 4)
        return fail(error, "unicode_indexer.bin: unexpected size or alignment");
    indexer_ = reinterpret_cast<const int32_t*>(ix.data);

    // voice.bin: int64 header [n, 50, 256, n, 8, 16], then every ttl style, then every dp style.
    const Blob& vb = blobs[kFileVoices];
    if (vb.size < 48 || reinterpret_cast<uintptr_t>(vb.data) % 8) return fail(error, "voice.bin: too small");
    int64_t h[6];
    std::memcpy(h, vb.data, 48);
    if (h[0] <= 0 || h[0] != h[3] || h[1] != 50 || h[2] != 256 || h[4] != 8 || h[5] != 16)
        return fail(error, "voice.bin: unexpected header");
    size_t floats = size_t(h[0] * h[1] * h[2] + h[3] * h[4] * h[5]);
    if (vb.size != 48 + floats * 4) return fail(error, "voice.bin: unexpected size");
    voices_ = int(h[0]);
    ttlDims_[0] = h[1];
    ttlDims_[1] = h[2];
    dpDims_[0] = h[4];
    dpDims_[1] = h[5];
    ttl_ = reinterpret_cast<const float*>(vb.data + 48);
    dpStyle_ = ttl_ + h[0] * h[1] * h[2];

    if (!dp_.load(blobs[kFileDuration].data, blobs[kFileDuration].size, "duration predictor", {}, k, error))
        return false;
    if (!te_.load(blobs[kFileTextEncoder].data, blobs[kFileTextEncoder].size, "text encoder", {}, k, error))
        return false;
    if (!ve_.load(blobs[kFileVectorEstimator].data, blobs[kFileVectorEstimator].size, "vector estimator",
                  {"text_emb", "style_ttl", "text_mask", "latent_mask", "total_step"}, k, error))
        return false;
    if (!voc_.load(blobs[kFileVocoder].data, blobs[kFileVocoder].size, "vocoder", {}, k, error)) return false;
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
    loaded_ = true;
    return true;
}

std::string Engine::voiceName(int i) const {
    // generate_voices_bin.py packs the voice JSON files sorted by name: F1..F5, M1..M5.
    if (voices_ == 10 && i >= 0 && i < 10) return std::string(i < 5 ? "F" : "M") + char('1' + i % 5);
    return "V" + std::to_string(i + 1);
}

size_t Engine::modelBytes() const {
    size_t n = 0;
    for (int i = 0; i < kFileCount; ++i) n += blobs_[i].size;
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
    return Tensor::view(DType::F32, {1, ttlDims_[0], ttlDims_[1]}, ttl_ + int64_t(voice) * ttlDims_[0] * ttlDims_[1]);
}

Tensor Engine::styleDp(int voice) const {
    return Tensor::view(DType::F32, {1, dpDims_[0], dpDims_[1]}, dpStyle_ + int64_t(voice) * dpDims_[0] * dpDims_[1]);
}

bool Engine::duration(const std::vector<int64_t>& ids, int voice, const ExecContext& ctx, float* seconds,
                      std::string* error) const {
    Session s(dp_);
    int64_t T = int64_t(ids.size());
    if (!setInput(s, dp_, "text_ids", idsTensor(ids), error) || !setInput(s, dp_, "style_dp", styleDp(voice), error) ||
        !setInput(s, dp_, "text_mask", ones({1, 1, T}), error) || !s.run(ctx, error))
        return false;
    const Tensor& d = s.output(0);
    if (d.type != DType::F32 || d.count() < 1) return fail(error, "duration predictor: bad output");
    *seconds = d.scalarFloat();
    return true;
}

bool Engine::encode(const std::vector<int64_t>& ids, int voice, const ExecContext& ctx, Tensor* textEmb,
                    std::string* error) const {
    Session s(te_);
    int64_t T = int64_t(ids.size());
    if (!setInput(s, te_, "text_ids", idsTensor(ids), error) ||
        !setInput(s, te_, "style_ttl", styleTtl(voice), error) ||
        !setInput(s, te_, "text_mask", ones({1, 1, T}), error) || !s.run(ctx, error))
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

bool Engine::vocode(const Tensor& latent, const ExecContext& ctx, Tensor* wav, std::string* error) const {
    if (latent.rank() != 3 || latent.dims[1] != kLatentChannels) return fail(error, "latent shape");
    Session s(voc_);
    if (!setInput(s, voc_, "latent", latent, error) || !s.run(ctx, error)) return false;
    *wav = s.output(0);
    if (wav->type != DType::F32) return fail(error, "vocoder: bad output");
    return true;
}

}  // namespace tts
