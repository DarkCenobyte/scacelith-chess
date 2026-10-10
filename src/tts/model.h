// The four Supertonic 3 graphs and their assets (character indexer, voice styles), and the
// stages of one synthesis at batch 1. Internal to src/tts (tts.h is the public API); the unit
// tests drive the stages directly to compare them with onnxruntime.
//
// Two layouts of the model folder load (model_store.h):
//   - the official release (Supertone/supertonic-3): duration_predictor.onnx, text_encoder.onnx,
//     vector_estimator.onnx, vocoder.onnx (fp32), tts.json (checked against the constants below),
//     unicode_indexer.json and the voice style M3.json;
//   - the old INT8 conversion by sherpa-onnx (*.int8.onnx, unicode_indexer.bin, voice.bin with the
//     ten voices), which a player who declined the update still speaks with.
// The graphs are mapped and used in place; the indexer and the styles are read into memory.
#pragma once
#include "graph.h"
#include "mapped_file.h"
#include "model_store.h"
#include <atomic>
#include <string>
#include <vector>

namespace tts {

constexpr int kSampleRate = 44100;
constexpr int kLatentChannels = 144;    // 24 x chunk_compress_factor 6
constexpr int kFrameSamples = 3072;     // 512 (vocoder hop) x 6 per latent frame

// The graphs, in the order of Engine::Layout::graphs.
enum ModelFile { kFileDuration, kFileTextEncoder, kFileVectorEstimator, kFileVocoder, kGraphCount };

struct Blob {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

// The loudness normalisation and fades of a synthesized line (tts.cpp).
void finishPcm(std::vector<float>& pcm);

class Engine {
public:
    // The file names of a layout ("" = none).
    struct Layout {
        const char* graphs[kGraphCount];
        const char* indexer;     // unicode_indexer.json / unicode_indexer.bin
        const char* voices;      // M3.json / voice.bin
        const char* config;      // tts.json (official layout only)
    };
    static const Layout kOfficial, kLegacy;
    static const Layout& layout(ModelKind k) { return k == ModelKind::Legacy ? kLegacy : kOfficial; }

    // From a folder holding one of the layouts. ModelKind::None: the official files when they are
    // all there, else the old ones when they are all there, else the official ones (the error
    // names the first one missing).
    bool loadDirectory(const std::string& dir, const kern::Table& k, std::string* error, ModelKind kind = ModelKind::None);
    bool loaded() const { return loaded_; }
    ModelKind kind() const { return kind_; }

    int voiceCount() const { return int(voiceNames_.size()); }
    std::string voiceName(int i) const;
    int voiceIndex(const std::string& name) const;   // -1 when absent
    const int32_t* indexer() const { return indexer_.data(); }
    size_t modelBytes() const;   // bytes of the model files

    // Stages. 'ids' are model ids (text::indices), 'voice' in [0, voiceCount()). A stage stops
    // with the error "cancelled" once 'cancel' (optional) is set.
    bool duration(const std::vector<int64_t>& ids, int voice, const ExecContext& ctx, float* seconds,
                  std::string* error, const std::atomic<bool>* cancel = nullptr) const;
    bool encode(const std::vector<int64_t>& ids, int voice, const ExecContext& ctx, Tensor* textEmb,
                std::string* error, const std::atomic<bool>* cancel = nullptr) const;
    // 'steps' Euler steps of the vector estimator from 'noise' [1, 144, L]. 'each' (optional)
    // receives the latent after every step.
    bool denoise(const Tensor& textEmb, int voice, const Tensor& noise, int steps, const ExecContext& ctx,
                 Tensor* latent, const std::atomic<bool>* cancel, std::string* error,
                 std::vector<Tensor>* each = nullptr) const;
    bool vocode(const Tensor& latent, const ExecContext& ctx, Tensor* wav, std::string* error,
                const std::atomic<bool>* cancel = nullptr) const;

    const Graph& graph(ModelFile f) const;
    Tensor styleTtl(int voice) const;   // [1, 50, 256]
    Tensor styleDp(int voice) const;    // [1, 8, 16]

    // The asset parsers, exposed for the tests. Each fails with a message naming the file.
    static bool parseConfig(const std::string& json, std::string* error);
    static bool parseIndexerJson(const std::string& json, std::vector<int32_t>& out, std::string* error);
    static bool parseIndexerBin(const uint8_t* data, size_t size, std::vector<int32_t>& out, std::string* error);
    // One voice style ({"style_ttl": {"dims": [1, 50, 256], "data": ...}, "style_dp": {"dims": [1, 8, 16], ...}}),
    // appended to 'ttl' and 'dp'.
    static bool parseVoiceJson(const std::string& json, std::vector<float>& ttl, std::vector<float>& dp, std::string* error);
    // voice.bin: int64 header [n, 50, 256, n, 8, 16], then every ttl style, then every dp style.
    static bool parseVoiceBin(const uint8_t* data, size_t size, std::vector<float>& ttl, std::vector<float>& dp, int* count,
                              std::string* error);

private:
    bool buildGraphs(const kern::Table& k, std::string* error);

    MappedFile files_[kGraphCount];
    Graph dp_, te_, ve_, voc_;
    std::vector<int32_t> indexer_;       // 65,536 entries: code point -> model id, -1 unknown
    std::vector<float> ttl_, dpStyle_;   // every voice's [50 x 256] and [8 x 16] styles
    std::vector<std::string> voiceNames_;
    size_t assetBytes_ = 0;
    ModelKind kind_ = ModelKind::None;
    bool loaded_ = false;
};

}  // namespace tts
