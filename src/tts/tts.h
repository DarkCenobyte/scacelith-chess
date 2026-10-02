// The coach's voice: in-process text-to-speech with the Supertonic 3 INT8 model (sherpa-onnx
// release of Supertone's OpenRAIL-M weights), run by our own small ONNX interpreter (src/tts/graph.*,
// no onnxruntime). The model files are never shipped with the game: they live in the folder of the
// model store (src/tts/model_store.h: <application data>/coach/, or --coach-dir), which the game
// fills by downloading them at the player's request. Without them load() fails and the coach runs
// with subtitles only.
//
// Pipeline per chunk of text (sentence-aligned, at most 300 characters, 120 for Japanese): the
// official text normalisation, duration predictor, text encoder, 'steps' Euler steps of the flow
// matching vector estimator from seeded Gaussian noise, vocoder; chunks are joined with 0.3 s of
// silence. Output: mono float PCM at 44.1 kHz.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tts {

struct Options {
    int threads = 2;            // threads of one synthesis (the caller plus helpers)
    int steps = 5;              // flow-matching steps (quality/speed: 3 is usable, 5 the default)
    float speed = 1.0f;         // speaking rate (the duration is divided by it)
    int voice = -1;             // -1 = the default teacher voice (M3); see Synthesizer::voiceName
    uint32_t seed = 0;          // noise seed; 0 = derived from the text, language and voice, so the
                                // same line always sounds the same
};

// Speech languages for a UI language code: en fr de es ru uk ar ja (false for zh-Hans, zh-Hant and
// anything else the coach does not speak). The one list: coach::speechSupported asks it.
bool languageSupported(const std::string& uiCode);

// Voice used when Options::voice is -1 (M3, the male "teacher" voice chosen by listening; M2, deeper, is
// the alternative).
int defaultVoice();

// Caps the instruction set of the compute kernels ("auto", "avx512", "avxvnni", "avx2", "sse2",
// "scalar"), for troubleshooting (a settings entry or a command-line switch). Applies from the
// next synthesis (and to the constant folding of later loads). Returns false (and logs) for an
// unknown name.
bool setArchCap(const char* arch);
// Name of the kernel set the next synthesis will use ("avx2", ...).
const char* activeArch();

// Folder Synthesizer::load() reads the model files from: tts::modelFolder() of the model store
// (<application data>/coach/), or 'dir' (--coach-dir, "" = the default again). Applies to later
// loads. Same as tts::setModelFolder() / tts::modelFolder() (model_store.h).
void setModelDirectory(const std::string& dir);
std::string modelDirectory();
// Whether load() can find the model files without loading them: every file of the manifest in
// modelDirectory() with its size (tts::modelStatus() == Ready). False means the coach speaks
// through subtitles only until the files are downloaded (model_store.h).
bool modelFilesPresent();

class Engine;
class ThreadPool;

// Blocking synthesis on the calling thread (plus Options::threads - 1 helpers); loads the models
// once. Not thread-safe: one synthesis at a time.
class Synthesizer {
public:
    Synthesizer();
    ~Synthesizer();
    Synthesizer(const Synthesizer&) = delete;
    Synthesizer& operator=(const Synthesizer&) = delete;

    // From the model store's folder (tts::modelFolder()). On failure with every file there
    // (tts::modelStatus() Ready), run tts::verifyModel(): damaged files mean a new download.
    bool load(std::string* error = nullptr);
    // From a given folder (tests, tools).
    bool loadFrom(const std::string& dir, std::string* error = nullptr);
    bool loaded() const;

    // Mono float PCM at sampleRate() (44100), normalised to about -20 dBFS RMS, peak <= -1 dBFS,
    // 10 ms fades at both ends. 'lang' is a model language tag (en fr de es ru uk ar ja ...);
    // unknown tags fall back to English. Empty on failure or when 'cancel' becomes true.
    std::vector<float> synthesize(const std::string& text, const std::string& lang, const Options& o,
                                  const std::atomic<bool>* cancel = nullptr);
    int sampleRate() const;
    int voiceCount() const;
    std::string voiceName(int i) const;

    // Timings of the last synthesize() call, in seconds (profiling, tests).
    struct Stats {
        double duration = 0, textEncoder = 0, vectorEstimator = 0, vocoder = 0, total = 0;
        double audioSeconds = 0;
        int chunks = 0;
        int droppedCharacters = 0;   // characters the model's indexer does not know
    };
    const Stats& lastStats() const { return stats_; }
    const Engine* engine() const { return engine_.get(); }

private:
    ThreadPool* pool(int threads);

    std::unique_ptr<Engine> engine_;
    std::unique_ptr<ThreadPool> pool_;
    int poolThreads_ = 0;   // the count pool_ was made for (it runs fewer if helpers failed to start)
    Stats stats_;
};

// A background thread around a Synthesizer, at below-normal priority. Requests are served by
// priority (higher first), first come first served within a priority.
class Worker {
public:
    Worker() = default;
    ~Worker();   // stop()
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // Starts the thread, which loads the models and then synthesises a short warm-up sentence
    // (pages the weights in) before ready() turns true. Requests made meanwhile are queued.
    // Returns false (and logs) when the thread cannot be created.
    bool start(const Options& o);
    // Cancels everything and joins the thread (call before audio::shutdown()).
    void stop();
    bool ready() const { return ready_.load(); }
    bool failed() const { return failed_.load(); }   // load failed: speech is unavailable

    // Queues a text; returns its id, or 0 when the worker is stopped or failed. 'seed' 0 derives
    // the noise seed from the text (Options::seed). 'speed' > 0 is this text's speaking rate in
    // place of the one given to start() (the rules lesson speaks slower; the models stay loaded).
    uint32_t request(const std::string& text, const std::string& lang, int priority = 0, uint32_t seed = 0,
                     float speed = 0.0f);
    // True once the request is finished (also when synthesis failed: take() then gives no samples).
    bool done(uint32_t id) const;
    // Moves the samples out and forgets the request. False if not done or unknown.
    bool take(uint32_t id, std::vector<float>& pcm);
    // Drops a request: queued, in progress (interrupted) or finished and not taken. 0 = all.
    void cancel(uint32_t id);
    // Requests queued or in progress.
    size_t pending() const;

private:
    struct Job {
        uint32_t id;
        int priority;
        uint64_t order;
        uint32_t seed;
        float speed;   // 0 = opts_.speed
        std::string text, lang;
    };
    void run();

    Options opts_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    std::map<uint32_t, std::vector<float>> results_;
    uint32_t nextId_ = 1;
    uint64_t order_ = 0;
    uint32_t running_ = 0;               // id in progress, 0 = none
    std::atomic<bool> cancelRunning_{false};
    std::atomic<bool> ready_{false}, failed_{false};
    bool quit_ = false;
    bool started_ = false;
};

}  // namespace tts
