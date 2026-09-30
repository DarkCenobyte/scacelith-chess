// The four Supertonic 3 graphs and their assets (character indexer, voice styles), and the
// stages of one synthesis at batch 1. Internal to src/tts (tts.h is the public API); the unit
// tests drive the stages directly to compare them with onnxruntime.
#pragma once
#include "graph.h"
#include "mapped_file.h"
#include <atomic>
#include <string>
#include <vector>

namespace tts {

constexpr int kSampleRate = 44100;
constexpr int kLatentChannels = 144;    // 24 x chunk_compress_factor 6
constexpr int kFrameSamples = 3072;     // 512 (vocoder hop) x 6 per latent frame

// Model files, in the order of Engine::kFiles.
enum ModelFile { kFileDuration, kFileTextEncoder, kFileVectorEstimator, kFileVocoder, kFileIndexer, kFileVoices,
                 kFileCount };

struct Blob {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

class Engine {
public:
    static const char* const kFiles[kFileCount];

    // From a folder holding the release files (mapped, used in place).
    bool loadDirectory(const std::string& dir, const kern::Table& k, std::string* error);
    // From memory that outlives the engine (the embedded copy).
    bool loadBlobs(const Blob blobs[kFileCount], const kern::Table& k, std::string* error);
    bool loaded() const { return loaded_; }

    int voiceCount() const { return voices_; }
    std::string voiceName(int i) const;
    const int32_t* indexer() const { return indexer_; }
    size_t modelBytes() const;   // bytes of the four graphs (mapped or embedded)

    // Stages. 'ids' are model ids (text::indices), 'voice' in [0, voiceCount()).
    bool duration(const std::vector<int64_t>& ids, int voice, const ExecContext& ctx, float* seconds,
                  std::string* error) const;
    bool encode(const std::vector<int64_t>& ids, int voice, const ExecContext& ctx, Tensor* textEmb,
                std::string* error) const;
    // 'steps' Euler steps of the vector estimator from 'noise' [1, 144, L]. 'each' (optional)
    // receives the latent after every step.
    bool denoise(const Tensor& textEmb, int voice, const Tensor& noise, int steps, const ExecContext& ctx,
                 Tensor* latent, const std::atomic<bool>* cancel, std::string* error,
                 std::vector<Tensor>* each = nullptr) const;
    bool vocode(const Tensor& latent, const ExecContext& ctx, Tensor* wav, std::string* error) const;

    const Graph& graph(ModelFile f) const;
    Tensor styleTtl(int voice) const;   // [1, 50, 256]
    Tensor styleDp(int voice) const;    // [1, 8, 16]

private:
    bool build(const Blob blobs[kFileCount], const kern::Table& k, std::string* error);

    MappedFile files_[kFileCount];
    Blob blobs_[kFileCount];
    Graph dp_, te_, ve_, voc_;
    const int32_t* indexer_ = nullptr;
    const float* ttl_ = nullptr;
    const float* dpStyle_ = nullptr;
    int64_t ttlDims_[2] = {0, 0}, dpDims_[2] = {0, 0};
    int voices_ = 0;
    bool loaded_ = false;
};

}  // namespace tts
