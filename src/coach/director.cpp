// The coach's director (director.h): a queue of beats performed one at a time on the Stage.
//
// A beat's life: Prepare (its line rendered twice with one seed, the spoken text synthesised; a
// beat that moves the hand also waits until the previous hand work is over), then Running (the
// voice starts, the gestures are handed to the scene with their apex times, the marks light on
// their words, the table action starts), then, for an offer, Offer (the card, until closeOffer()).
// The next queued lines are synthesised while the current one plays, so that lines follow each
// other without a gap. A WaitMove leaves the queue when it is reached: it waits in its own slot,
// holding the beats queued behind it, while anything queued meanwhile (a hint, the explanation of
// an illegal attempt, the reaction to a wrong move) is performed on top of the wait.
#include "director.h"
#include "catalog.h"
#include "pacing.h"
#include "subtitles.h"
#include "../core/log.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <utility>

namespace coach {
namespace {

constexpr int kLookahead = 3;            // queued lines synthesised ahead of the running one
constexpr float kFadeIn = 0.25f;         // mark strength 0 -> 1
constexpr float kHold = 0.4f;            // a mark stays this long after its line
constexpr float kFadeOut = 0.4f;         // then fades out
constexpr float kDemoPause = 0.4f;       // stillness after a demonstration move (Beat::seconds 0)
constexpr float kRefusedVoice = 1.0f;    // startVoice refused this long: the line is only shown
constexpr float kWatchdog = 1.5f;        // a heard line never reported finished: over after this
constexpr float kLevelWindow = 0.05f;    // RMS window of the mouth's speech level
constexpr size_t kPrefetchMax = 64;      // lines synthesised ahead by prefetch() kept at most
constexpr int kMaxSteps = 32;            // beats started in one update at most

bool handGesture(GestureKind k) { return k != GestureKind::Nod && k != GestureKind::ShakeHead; }

bool tableBeat(BeatKind k) {
    return k == BeatKind::DemoMove || k == BeatKind::Rewind || k == BeatKind::SetPosition || k == BeatKind::PlayMove;
}

bool speaksLine(const Beat& b) {
    switch (b.kind) {
    case BeatKind::Say:
    case BeatKind::DemoMove:
    case BeatKind::Rewind:
    case BeatKind::OfferTakeback: return !b.line.key.empty();
    default: return false;
    }
}

bool hasHandGesture(const Beat& b) {
    for (const Gesture& g : b.gestures)
        if (handGesture(g.kind)) return true;
    return false;
}

bool movesHand(const Beat& b) { return tableBeat(b.kind) || hasHandGesture(b); }

// Beat serials are process-wide (main thread): the catalog remembers its variant picks by seed, so
// seeds must not start again with every game.
uint64_t nextSerial() {
    static uint64_t serial = 0;
    return ++serial;
}

uint32_t mixSeed(uint64_t counter, int ply) {
    uint64_t x = counter * 0x9E3779B97F4A7C15ull ^ (uint64_t(uint32_t(ply)) << 32);
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return uint32_t(x) | 1u;
}

// Identity of a line (key and arguments), for the lines prefetch() synthesised ahead.
std::string signature(const Line& l) {
    std::string s = l.key;
    for (const auto& p : l.args) {
        const Arg& a = p.second;
        s += '|';
        s += p.first;
        s += '=';
        s += std::to_string(int(a.kind)) + ',' + std::to_string(int(a.piece)) + ',' + std::to_string(int(a.color)) + ',' +
             (a.own ? "1" : "0") + ',' + std::to_string(int(a.square)) + ',' + a.san + ',' + a.uci + ',' +
             std::to_string(a.number) + ',' + std::to_string(a.mate) + ',' + a.text;
    }
    return s;
}

// What the coach looks at for Look::Target: the first gesture's square, else the first mark's.
chess::Square targetOf(const Beat& b) {
    for (const Gesture& g : b.gestures) {
        if (g.square != chess::NoSquare) return g.square;
        if (!g.path.empty()) return g.path.front();
    }
    for (const Mark& m : b.marks) {
        if (m.kind == Mark::Kind::Arrow && m.from != chess::NoSquare) return m.from;
        if (m.square != chess::NoSquare) return m.square;
    }
    if (b.uci.size() >= 4) return chess::parseSquare(b.uci.substr(0, 2));
    return chess::NoSquare;
}

// Seconds from a line's start at which the word 'name' is read, for a line shown without a
// voice: its share of the text's weight over the reading time (Gesture::at without the word).
float readingAnchor(const Catalog::Rendered& spoken, const std::string& name, float fallback, float duration) {
    const int off = name.empty() ? -1 : spoken.anchor(name);
    const float total = textWeight(spoken.text, 0, spoken.text.size());
    float frac = std::min(1.0f, std::max(0.0f, fallback));
    if (off >= 0 && total > 0.0f) frac = textWeight(spoken.text, 0, size_t(off)) / total;
    return std::max(0.0f, frac * duration - PacingOptions().earlyBias);   // pacing's bias
}

// The mouth: RMS of the voice over ~50 ms around 't' (seconds into the PCM), 0..1.
float levelAt(const std::vector<float>& pcm, int rate, float t) {
    if (pcm.empty() || rate <= 0 || t < 0.0f) return 0.0f;
    const long half = long(kLevelWindow * float(rate) * 0.5f);
    const long c = long(double(t) * rate);
    const long a = std::max(0L, c - half), b = std::min(long(pcm.size()), c + half);
    if (b <= a) return 0.0f;
    double e = 0.0;
    for (long i = a; i < b; ++i) e += double(pcm[size_t(i)]) * double(pcm[size_t(i)]);
    const float rms = float(std::sqrt(e / double(b - a)));
    if (rms < 0.003f) return 0.0f;                  // silence (about -50 dBFS)
    return std::min(1.0f, rms / 0.2f);              // speech is normalised to about -20 dBFS RMS
}

}  // namespace

struct Director::Impl {
    // A line's two renderings and its audio.
    struct Speech {
        bool rendered = false;
        bool empty = false;                // no such key: nothing to say
        std::string ui, lang;              // subtitle and voice languages it was made for
        float speed = 1.0f;
        Catalog::Rendered written, spoken;
        uint32_t request = 0;
        bool ready = false;                // pcm holds the audio
        bool failed = false;               // nothing will be heard: shown only
        std::vector<float> pcm;
    };
    struct Item {
        Beat beat;
        uint64_t script = 0;
        uint64_t serial = 0;
        uint32_t seed = 0;
        bool silent = false;               // an offer whose line was skipped: the card only
        bool fast = false;                 // a rewind of a skipped script
        Speech speech;
    };
    // A line synthesised ahead by prefetch(), taken by the first beat that says the same line.
    struct Cached {
        std::string sig, ui, lang;
        float speed = 1.0f;
        int variant = 1;
        Speech speech;
    };
    struct LiveMark {
        ShownMark shown;
        uint64_t serial = 0;               // the beat it belongs to
        float onAt = 0.0f;                 // line time at which it lights
        bool lit = false;
        bool untilRewind = false;
        bool released = false;             // its line is over: hold, then fade
        float holdLeft = 0.0f;
        bool fading = false;
    };
    enum class Phase { Prepare, Running, Offer };
    struct Run {
        bool active = false;
        Item item;
        Phase phase = Phase::Prepare;
        bool line = false;                 // a line is said
        bool voiced = false;               // ... and heard
        bool lineStarted = false, lineDone = false;
        float lineTime = 0.0f;             // seconds since the line started (not paused)
        float clock = 0.0f;                // line time: the voice clock, or lineTime without a voice
        float duration = 0.0f;             // audio length, or reading time
        float refused = 0.0f;
        float cutAt = -1.0f;               // -1 no cut asked, -2 no boundary left (finish), else a time
        SpeechTiming timing;
        bool gestures = false;             // hand gestures were given: endGestures() at the end
        bool table = false, tableDone = true;
        int tableFrames = 0;
        float pauseLeft = 0.0f;
        bool fast = false;                 // rewind
        bool card = false;                 // the takeback card is shown
    };

    Stage* stage = nullptr;
    DirectorConfig config;
    std::deque<Item> queue;                // beats to perform, in order
    std::deque<Item> held;                 // beats behind the waiting WaitMove
    Item waitItem;
    bool waiting = false;
    Run run;
    std::vector<LiveMark> live;
    std::vector<ShownMark> shown;
    std::deque<Cached> cache;
    std::function<void(const Beat&)> observer;
    uint64_t scripts = 0, last = 0;
    int ply = 0;
    int demoDepth = 0;                     // demonstration moves on the table, not rewound yet
    bool paused = false;
    bool voicePaused = false;              // the stage was told pauseVoice(true), not released yet
    bool hint = false;                     // skip hint shown
    float level = 0.0f;

    std::string speechLang() const { return speechLanguage(config.uiLanguage); }

    // ---- Speech --------------------------------------------------------------------------------------
    void cancel(Speech& s) {
        if (stage && s.request && !s.ready && !s.failed) stage->cancelSpeech(s.request);
        s = Speech();
    }

    void poll(Speech& s) {
        if (!stage || !s.request || s.ready || s.failed) return;
        if (stage->takeSpeech(s.request, s.pcm)) {
            s.ready = true;
            if (s.pcm.empty()) s.failed = true;   // synthesis failed: shown only
        } else if (stage->speechFailed(s.request)) {
            s.failed = true;
            const std::string& text = s.written.text.empty() ? s.spoken.text : s.written.text;   // prefetched: spoken only
            LOGW("coach: speech of %s failed, shown only", text.c_str());
        }
    }

    // Renders the line (written and spoken, one seed) and asks for its audio.
    void prepare(Item& it) {
        Speech& s = it.speech;
        if (s.rendered && (s.ui != config.uiLanguage || s.speed != config.speed)) cancel(s);
        if (s.rendered || !speaksLine(it.beat) || it.silent) return;
        const Catalog& cat = Catalog::shared();
        const Line& line = it.beat.line;
        s.rendered = true;
        s.ui = config.uiLanguage;
        s.lang = speechLang();
        s.speed = config.speed;
        const std::string sig = signature(line);
        for (auto c = cache.begin(); c != cache.end(); ++c) {
            if (c->sig != sig || c->ui != s.ui || c->lang != s.lang || c->speed != s.speed) continue;
            // Said as prefetched: same variant in both renderings, same audio.
            Speech got = std::move(c->speech);
            const int variant = c->variant;
            cache.erase(c);
            s.written = cat.renderVariant(line, s.ui, false, variant);
            s.spoken = std::move(got.spoken);
            s.request = got.request;
            s.ready = got.ready;
            s.failed = got.failed;
            s.pcm = std::move(got.pcm);
            return;
        }
        s.written = cat.render(line, s.ui, false, it.seed);
        s.spoken = cat.render(line, s.lang, true, it.seed);
        if (s.written.text.empty() && s.spoken.text.empty()) {
            s.empty = true;
            return;
        }
        stage->prewarmGlyphs(s.written.text);
        if (s.spoken.text.empty() || !stage->voiceAvailable()) {
            s.failed = true;
            return;
        }
        s.request = stage->requestSpeech(s.spoken.text, s.lang, s.speed, 4 - int(it.beat.priority));
        if (!s.request) s.failed = true;
    }

    // Keeps the running line and the next queued ones synthesised; collects finished audio.
    void pump(bool ahead) {
        if (run.active) poll(run.item.speech);
        int n = 0;
        auto walk = [&](std::deque<Item>& q) {
            for (Item& it : q) {
                if (ahead && n < kLookahead && speaksLine(it.beat) && !it.silent) {
                    prepare(it);
                    ++n;
                }
                poll(it.speech);
            }
        };
        walk(queue);
        walk(held);
        for (Cached& c : cache) poll(c.speech);
    }

    void drop(Item& it) { cancel(it.speech); }

    // ---- Queue ---------------------------------------------------------------------------------------
    Item makeItem(const Beat& b, uint64_t script) {
        Item it;
        it.beat = b;
        it.script = script;
        it.serial = nextSerial();
        it.seed = mixSeed(it.serial, ply);
        if (observer) observer(b);
        return it;
    }

    void play(const Script& s) {
        if (s.empty()) return;
        const uint64_t id = ++scripts;
        last = id;
        size_t prev = 0;
        bool first = true;
        for (const Beat& b : s) {
            // Behind every queued beat of the same or a higher priority, and behind the previous
            // beat of its own script (beats keep their order inside a script).
            size_t pos = queue.size();
            while (pos > 0 && int(queue[pos - 1].beat.priority) > int(b.priority)) --pos;
            if (!first) pos = std::max(pos, prev);
            queue.insert(queue.begin() + long(pos), makeItem(b, id));
            prev = pos + 1;
            first = false;
        }
        preempt();
    }

    void playNext(const Script& s) {
        if (s.empty()) return;
        const uint64_t id = ++scripts;
        last = id;
        size_t pos = 0;
        for (const Beat& b : s) queue.insert(queue.begin() + long(pos++), makeItem(b, id));
    }

    bool dropStale(std::deque<Item>& q, bool all) {
        bool any = false;
        for (auto it = q.begin(); it != q.end();) {
            const Beat& b = it->beat;
            if (b.priority == Priority::Low && (all || (b.ply >= 0 && ply - b.ply > 2))) {
                drop(*it);
                it = q.erase(it);
                any = true;
            } else {
                ++it;
            }
        }
        return any;
    }

    // An Urgent beat waits at the head of the queue: a Normal/Low beat not started yet goes back
    // behind it, a running Normal/Low line is cut at its next pause at a punctuation mark, a pause
    // ends.
    void preempt() {
        if (!run.active || queue.empty() || queue.front().beat.priority != Priority::Urgent) return;
        const Beat& b = run.item.beat;
        if (b.priority == Priority::Urgent) return;
        if (run.phase == Phase::Prepare) {
            size_t pos = 0;
            while (pos < queue.size() && queue[pos].beat.priority == Priority::Urgent) ++pos;
            queue.insert(queue.begin() + long(pos), std::move(run.item));
            run = Run();
            return;
        }
        if (run.phase != Phase::Running) return;
        if (b.kind == BeatKind::Pause) {
            run.pauseLeft = 0.0f;
            return;
        }
        if (b.kind != BeatKind::Say || !run.line || run.lineDone || run.cutAt != -1.0f) return;
        run.cutAt = nextBoundary();
    }

    // Line time of the next pause at a punctuation mark (comma, colon, dash or sentence end) after
    // the running line's clock (-2: none).
    float nextBoundary() const {
        const float t = run.clock + 0.02f;
        if (run.voiced) {
            for (const SpeechTiming::Pause& p : run.timing.pauses) {
                if (p.boundary < 0 || p.end <= t) continue;
                return std::max(p.start, p.end - std::min(0.05f, 0.5f * (p.end - p.start)));
            }
            return -2.0f;
        }
        const std::string& text = run.item.speech.spoken.text;
        const float total = textWeight(text, 0, text.size());
        if (total <= 0.0f) return -2.0f;
        for (size_t off : pauseBoundaries(text)) {
            const float at = textWeight(text, 0, off) / total * run.duration;
            if (at > t) return at;
        }
        return -2.0f;
    }

    // ---- Marks ---------------------------------------------------------------------------------------
    void addMarks(const Item& it, const Catalog::Rendered* spoken) {
        for (const Mark& m : it.beat.marks) {
            LiveMark lm;
            lm.shown.kind = m.kind;
            lm.shown.square = m.square;
            lm.shown.from = m.from;
            lm.shown.via = m.via;
            lm.shown.to = m.to;
            lm.serial = it.serial;
            lm.untilRewind = m.untilRewind;
            lm.onAt = 0.0f;
            if (!m.anchor.empty() && spoken) {
                lm.onAt = run.voiced ? anchorTime(run.timing, *spoken, m.anchor, 0.0f)
                                     : readingAnchor(*spoken, m.anchor, 0.0f, run.duration);
            }
            live.push_back(lm);
        }
    }

    void light(LiveMark& m, float clock) {
        m.lit = true;
        m.shown.age = std::max(0.0f, clock - m.onAt);
        m.shown.strength = std::min(1.0f, m.shown.age / kFadeIn);
    }

    void lightMarks(uint64_t serial, float clock) {
        for (LiveMark& m : live)
            if (m.serial == serial && !m.lit && clock >= m.onAt) light(m, clock);
    }

    // The beat's line is over: its marks stay kHold (untilRewind ones: until the next rewind).
    void releaseMarks(uint64_t serial, bool now) {
        for (LiveMark& m : live) {
            if (m.serial != serial) continue;
            if (!m.lit) light(m, m.onAt);
            if (m.untilRewind && !now) continue;
            release(m, now);
        }
    }

    void release(LiveMark& m, bool now) {
        if (m.released && !now) return;
        m.released = true;
        m.holdLeft = now ? 0.0f : kHold;
        if (now) m.fading = true;
    }

    void releaseUntilRewind() {
        for (LiveMark& m : live) {
            if (!m.untilRewind) continue;
            if (!m.lit) light(m, m.onAt);
            release(m, false);
            m.holdLeft = 0.0f;
        }
    }

    void fadeAll() {
        for (LiveMark& m : live) {
            if (!m.lit) light(m, m.onAt);
            release(m, true);
        }
    }

    void updateMarks(float dt) {
        for (LiveMark& m : live) {
            if (!m.lit) continue;
            m.shown.age += dt;
            if (m.released && !m.fading) {
                m.holdLeft -= dt;
                if (m.holdLeft <= 0.0f) m.fading = true;
            }
            if (m.fading) m.shown.strength -= dt / kFadeOut;
            else m.shown.strength = std::min(1.0f, m.shown.strength + dt / kFadeIn);
        }
        live.erase(std::remove_if(live.begin(), live.end(),
                                  [](const LiveMark& m) { return m.lit && m.fading && m.shown.strength <= 0.0f; }),
                   live.end());
    }

    void rebuildShown() {
        shown.clear();
        for (const LiveMark& m : live)
            if (m.lit && m.shown.strength > 0.0f) shown.push_back(m.shown);
    }

    // ---- Beats ---------------------------------------------------------------------------------------
    bool startNext() {
        if (queue.empty()) return false;
        Item it = std::move(queue.front());
        queue.pop_front();
        if (it.beat.kind == BeatKind::WaitMove) {
            if (waiting) {   // a second wait inside the first one: after it
                held.push_front(std::move(it));
                return true;
            }
            waiting = true;
            waitItem = std::move(it);
            for (Item& q : queue) held.push_back(std::move(q));
            queue.clear();
            stage->look(waitItem.beat.look, targetOf(waitItem.beat));
            return true;
        }
        run = Run();
        run.active = true;
        run.item = std::move(it);
        run.phase = Phase::Prepare;
        run.line = speaksLine(run.item.beat) && !run.item.silent;
        run.fast = run.item.fast;
        return true;
    }

    // Starts the prepared beat now. False while the voice is refused (retried next update).
    bool begin(float dt) {
        Beat& b = run.item.beat;
        Speech& s = run.item.speech;
        if (b.kind == BeatKind::Rewind) {
            const int count = std::min(b.count, demoDepth);
            if (count <= 0) {   // nothing of the demonstration is on the table (skipped)
                run.line = false;
                run.table = false;
                releaseUntilRewind();
                return true;
            }
        }
        if (run.line) {
            run.voiced = s.ready && !s.failed && !s.pcm.empty();
            if (run.voiced) {
                run.timing = estimateTiming(s.pcm, stage->speechSampleRate(), s.spoken);
                std::vector<float> pcm = s.pcm;   // s.pcm stays for the mouth's level
                if (!stage->startVoice(std::move(pcm))) {
                    run.refused += dt;
                    if (run.refused < kRefusedVoice) return false;
                    LOGW("coach: the voice was refused, %s shown only", s.written.text.c_str());
                    run.voiced = false;
                }
            }
            if (run.voiced) {
                run.duration = run.timing.duration;
            } else {
                run.duration = stage->readingTime(s.written.text);
                run.timing = SpeechTiming();
            }
            run.lineStarted = true;
            const bool voiceOk = stage->voiceAvailable() && run.voiced;
            if (subtitlesShown(config.subtitles, config.uiLanguage, speechLang(), voiceOk))
                stage->showSubtitle(s.written.text, run.duration);
        }
        stage->look(b.look, targetOf(b));

        // Gestures: each apex on its word (a fraction of the line without one).
        for (const Gesture& g : b.gestures) {
            float apex = 0.0f;
            if (run.line) {
                apex = run.voiced ? anchorTime(run.timing, s.spoken, g.anchor, g.at)
                                  : readingAnchor(s.spoken, g.anchor, g.at, run.duration);
            }
            stage->gesture(g, apex);
            if (handGesture(g.kind)) run.gestures = true;
        }
        addMarks(run.item, run.line ? &s.spoken : nullptr);
        lightMarks(run.item.serial, 0.0f);

        switch (b.kind) {
        case BeatKind::DemoMove:
            stage->demoMove(b.uci, b.seconds > 0.0f ? b.seconds : kDemoPause);
            ++demoDepth;
            run.table = true;
            break;
        case BeatKind::Rewind: {
            const int count = std::min(b.count, demoDepth);
            releaseUntilRewind();
            stage->rewindDemo(count, run.fast);
            demoDepth -= count;
            run.table = true;
            break;
        }
        case BeatKind::SetPosition:
            fadeAll();
            stage->setPosition(b.fen);
            demoDepth = 0;
            run.table = true;
            break;
        case BeatKind::PlayMove:
            stage->playLessonMove(b.uci);
            run.table = true;
            break;
        case BeatKind::Pause: run.pauseLeft = b.seconds; break;
        default: break;
        }
        run.tableDone = !run.table;
        run.tableFrames = 0;
        return true;
    }

    // The running line is over (heard, cut or skipped).
    void lineOver(bool cut) {
        run.lineDone = true;
        releaseMarks(run.item.serial, cut);
        if (run.gestures) {
            stage->endGestures();
            run.gestures = false;
        }
        if (cut) stage->showSubtitle("", 0.0f);
    }

    // Advances the running beat by dt. True once it is over.
    bool advance(float dt) {
        const Beat& b = run.item.beat;
        if (run.phase == Phase::Prepare) {
            if (run.line) {
                prepare(run.item);
                const Speech& s = run.item.speech;
                if (s.empty) run.line = false;
                else if (!s.ready && !s.failed) return false;   // still synthesising
            }
            if (movesHand(b) && (stage->bodyBusy() || stage->tableBusy())) return false;
            if (!begin(dt)) return false;
            run.phase = Phase::Running;
            dt = 0.0f;
        }
        if (run.phase == Phase::Offer) return false;

        if (run.line && !run.lineDone) {
            run.lineTime += dt;
            bool finished = false;
            if (run.voiced) {
                const double c = stage->voiceClock(&finished);
                if (c >= 0.0) run.clock = float(c);
            } else {
                run.clock = run.lineTime;
                finished = run.clock >= run.duration;
            }
            lightMarks(run.item.serial, run.clock);
            if (run.cutAt >= 0.0f && run.clock >= run.cutAt) {
                if (run.voiced) stage->stopVoice();
                lineOver(true);
            } else if (finished) {
                lineOver(false);
            } else if (run.voiced && run.lineTime > run.duration + kWatchdog) {
                stage->stopVoice();
                lineOver(false);
            }
        }
        if (run.table && !run.tableDone && run.tableFrames++ >= 1 && !stage->tableBusy()) run.tableDone = true;
        if (b.kind == BeatKind::Pause) run.pauseLeft -= dt;

        const bool lineOk = !run.line || run.lineDone;
        switch (b.kind) {
        case BeatKind::Pause: return run.pauseLeft <= 0.0f;
        case BeatKind::OfferTakeback:
            if (!lineOk) return false;
            run.phase = Phase::Offer;
            run.card = true;
            stage->showTakebackOffer(true);
            return false;
        default: return lineOk && run.tableDone;
        }
    }

    // The running beat is over: its marks without a line go, the hand retracts.
    void finish() {
        if (!run.line) releaseMarks(run.item.serial, false);
        if (run.gestures) stage->endGestures();
        if (run.card) stage->showTakebackOffer(false);
        run = Run();
    }

    // Stops the running beat now (skip, clear, jump): voice, subtitle, hand, marks.
    void stopRun(bool keepUntilRewind) {
        if (!run.active) return;
        if (run.phase == Phase::Prepare) {
            drop(run.item);
        } else {
            if (run.line && !run.lineDone) {
                if (run.voiced) stage->stopVoice();
                stage->showSubtitle("", 0.0f);
                run.lineDone = true;
            }
            for (LiveMark& m : live) {
                if (m.serial != run.item.serial) continue;
                if (!m.lit) light(m, m.onAt);
                if (!(keepUntilRewind && m.untilRewind)) release(m, true);
            }
            if (run.gestures) stage->endGestures();
            if (run.card) stage->showTakebackOffer(false);
        }
        run = Run();
    }

    bool rewindQueued(uint64_t script) const {
        for (const Item& it : queue)
            if (it.script == script && it.beat.kind == BeatKind::Rewind) return true;
        return false;
    }

    void update(float dt, int nowPly) {
        ply = nowPly;
        pump(!paused);
        if (paused) {
            if (level != 0.0f) stage->speechLevel(0.0f);
            level = 0.0f;
            return;
        }
        dropStale(queue, false);
        dropStale(held, false);
        preempt();
        updateMarks(dt);     // marks lit below take their age from the line clock
        float step = dt;
        for (int i = 0; i < kMaxSteps; ++i) {
            if (!run.active && !startNext()) break;
            if (!run.active) continue;   // a WaitMove began waiting
            if (!advance(step)) break;
            finish();
            step = 0.0f;
        }
        pump(true);   // the beats just started or moved up
        if (!run.active && queue.empty() && held.empty() && !waiting) releaseUntilRewind();
        rebuildShown();

        float lv = 0.0f;
        if (run.active && run.voiced && run.lineStarted && !run.lineDone)
            lv = levelAt(run.item.speech.pcm, stage->speechSampleRate(), run.clock);
        stage->speechLevel(lv);
        level = lv;
        const bool h = skippable();
        if (h != hint) {
            hint = h;
            stage->showSkipHint(h);
        }
    }

    bool skippable() const {
        if (!run.active) return false;
        const Beat& b = run.item.beat;
        if (b.kind == BeatKind::Rewind) return !run.fast;
        if (run.phase == Phase::Offer) return false;
        return b.skippable;
    }

    void skip() {
        if (!stage || !run.active) return;
        const Beat& b = run.item.beat;
        const uint64_t script = run.item.script;
        if (b.kind == BeatKind::Rewind) {
            if (!run.fast) {
                run.fast = true;
                if (run.phase == Phase::Running) stage->hurryTable();
            }
            return;
        }
        if (!b.skippable || run.phase == Phase::Offer) return;
        // The rest of the script: skippable beats go; an offer stays (its card only); a rewind
        // stays and goes briskly.
        for (auto it = queue.begin(); it != queue.end();) {
            if (it->script != script) {
                ++it;
                continue;
            }
            if (it->beat.kind == BeatKind::OfferTakeback) {
                drop(*it);
                it->silent = true;
                ++it;
            } else if (it->beat.kind == BeatKind::Rewind) {
                it->fast = true;
                ++it;
            } else if (it->beat.skippable) {
                drop(*it);
                it = queue.erase(it);
            } else {
                ++it;
            }
        }
        if (b.kind == BeatKind::OfferTakeback) {
            // The offer's line: the card shows at once.
            if (run.phase == Phase::Running && run.line && !run.lineDone) {
                if (run.voiced) stage->stopVoice();
                stage->showSubtitle("", 0.0f);
                run.lineDone = true;
                if (run.gestures) {
                    stage->endGestures();
                    run.gestures = false;
                }
            } else if (run.phase == Phase::Prepare) {
                drop(run.item);
                run.item.silent = true;
                run.line = false;
            }
            return;
        }
        stopRun(rewindQueued(script));
    }

    void clear() {
        if (!stage) return;
        stopRun(false);
        stage->showSubtitle("", 0.0f);
        for (Item& it : queue) drop(it);
        for (Item& it : held) drop(it);
        queue.clear();
        held.clear();
        waiting = false;
        waitItem = Item();
        live.clear();
        shown.clear();
        if (demoDepth > 0) {   // a demonstration cut short: its pieces go back
            stage->rewindDemo(demoDepth, true);
            demoDepth = 0;
        }
        if (hint) {
            hint = false;
            stage->showSkipHint(false);
        }
    }

    bool pending(uint64_t script) const {
        if (script == 0) return false;
        if (run.active && run.item.script == script) return true;
        if (waiting && waitItem.script == script) return true;
        for (const Item& it : queue)
            if (it.script == script) return true;
        for (const Item& it : held)
            if (it.script == script) return true;
        return false;
    }

    bool jumpToWait() {
        if (waiting) return true;
        auto isQuietSay = [](const Beat& b) { return b.skippable && (b.kind == BeatKind::Say || b.kind == BeatKind::Pause); };
        if (run.active && !isQuietSay(run.item.beat)) return false;
        size_t w = 0;
        while (w < queue.size() && queue[w].beat.kind != BeatKind::WaitMove) {
            if (!isQuietSay(queue[w].beat)) return false;
            ++w;
        }
        if (w == queue.size()) return false;
        stopRun(false);
        for (size_t i = 0; i < w; ++i) drop(queue[i]);
        queue.erase(queue.begin(), queue.begin() + long(w));
        startNext();
        return waiting;
    }
};

Director::Director() : d_(new Impl) {}
Director::~Director() { delete d_; }

void Director::reset(Stage* stage, const DirectorConfig& config) {
    // The old stage may be gone: it is only told to stop when it is the new one too.
    if (stage && stage == d_->stage) {
        clear();
        for (Impl::Cached& c : d_->cache) d_->cancel(c.speech);
        if (d_->voicePaused) stage->pauseVoice(false);   // left paused (back to the menu from the pause menu)
    }
    auto observer = std::move(d_->observer);
    delete d_;
    d_ = new Impl;
    d_->stage = stage;
    d_->config = config;
    d_->observer = std::move(observer);
}

void Director::setConfig(const DirectorConfig& config) {
    const bool speech = config.uiLanguage != d_->config.uiLanguage || config.speed != d_->config.speed;
    d_->config = config;
    if (!speech) return;
    // Prepared lines of the old language or speed are made again (the running one stays).
    for (Impl::Cached& c : d_->cache) d_->cancel(c.speech);
    d_->cache.clear();
    for (Impl::Item& it : d_->queue) d_->cancel(it.speech);
    for (Impl::Item& it : d_->held) d_->cancel(it.speech);
}

const DirectorConfig& Director::config() const { return d_->config; }

void Director::play(const Script& script) {
    if (d_->stage) d_->play(script);
}

void Director::playNext(const Script& script) {
    if (d_->stage) d_->playNext(script);
}

void Director::prefetch(const std::vector<Line>& lines) {
    Impl& d = *d_;
    if (!d.stage || !d.stage->voiceAvailable()) return;
    const Catalog& cat = Catalog::shared();
    const std::string lang = d.speechLang();
    for (const Line& l : lines) {
        if (l.empty() || !cat.has(l.key)) continue;
        const std::string sig = signature(l);
        bool have = false;
        for (const Impl::Cached& c : d.cache)
            if (c.sig == sig && c.lang == lang && c.ui == d.config.uiLanguage && c.speed == d.config.speed) have = true;
        if (have) continue;
        Impl::Cached c;
        c.sig = sig;
        c.ui = d.config.uiLanguage;
        c.lang = lang;
        c.speed = d.config.speed;
        c.variant = cat.pickVariant(l.key, mixSeed(nextSerial(), -1));
        c.speech.rendered = true;
        c.speech.ui = c.ui;
        c.speech.lang = lang;
        c.speech.speed = c.speed;
        c.speech.spoken = cat.renderVariant(l, lang, true, c.variant);
        if (c.speech.spoken.text.empty()) continue;
        d.stage->prewarmGlyphs(cat.renderVariant(l, c.ui, false, c.variant).text);
        c.speech.request = d.stage->requestSpeech(c.speech.spoken.text, lang, c.speed, 0);
        if (!c.speech.request) continue;
        d.cache.push_back(std::move(c));
        while (d.cache.size() > kPrefetchMax) {
            d.cancel(d.cache.front().speech);
            d.cache.pop_front();
        }
    }
}

void Director::update(float dt, int ply) {
    if (d_->stage) d_->update(dt, ply);
}

void Director::setPaused(bool paused) {
    Impl& d = *d_;
    if (d.paused == paused) return;
    d.paused = paused;
    if (!d.stage) return;
    if (paused && d.run.active && d.run.voiced && d.run.lineStarted && !d.run.lineDone) {
        d.stage->pauseVoice(true);
        d.voicePaused = true;
    } else if (!paused && d.voicePaused) {
        d.stage->pauseVoice(false);   // also when the paused line was stopped meanwhile (clear)
        d.voicePaused = false;
    }
}

void Director::skip() { d_->skip(); }

void Director::playerActed() {
    Impl& d = *d_;
    d.dropStale(d.queue, true);
    d.dropStale(d.held, true);
    if (d.run.active && d.run.phase == Impl::Phase::Prepare && d.run.item.beat.priority == Priority::Low) {
        d.drop(d.run.item);
        d.run = Impl::Run();
    }
}

void Director::clear() { d_->clear(); }

bool Director::idle() const {
    const Impl& d = *d_;
    return !d.run.active && d.queue.empty() && d.held.empty() && !d.waiting;
}

bool Director::speaking() const {
    const Impl& d = *d_;
    return d.run.active && d.run.line && d.run.lineStarted && !d.run.lineDone;
}

bool Director::skippable() const { return d_->skippable(); }

bool Director::waitingMove(int* expect) const {
    if (!d_->waiting) return false;
    if (expect) *expect = d_->waitItem.beat.expect;
    return true;
}

void Director::endWait() {
    Impl& d = *d_;
    if (!d.waiting) return;
    d.waiting = false;
    d.waitItem = Impl::Item();
    for (Impl::Item& it : d.held) d.queue.push_back(std::move(it));
    d.held.clear();
}

bool Director::offerOpen() const { return d_->run.active && d_->run.phase == Impl::Phase::Offer; }

bool Director::closeOffer() {
    Impl& d = *d_;
    if (!d.stage) return false;
    if (d.run.active && d.run.item.beat.kind == BeatKind::OfferTakeback) {
        d.stopRun(false);
        return true;
    }
    for (auto it = d.queue.begin(); it != d.queue.end(); ++it) {
        if (it->beat.kind != BeatKind::OfferTakeback) continue;
        d.drop(*it);
        d.queue.erase(it);
        return true;
    }
    return false;
}

const std::vector<ShownMark>& Director::marks() const { return d_->shown; }

uint64_t Director::lastScript() const { return d_->last; }

bool Director::pending(uint64_t script) const { return d_->pending(script); }

bool Director::busy() const {
    const Impl& d = *d_;
    return d.run.active || !d.queue.empty();
}

bool Director::jumpToWait() { return d_->stage && d_->jumpToWait(); }

void Director::dropQueued(uint64_t script) {
    if (script == 0) return;
    Impl& d = *d_;
    for (std::deque<Impl::Item>* q : {&d.queue, &d.held}) {
        for (auto it = q->begin(); it != q->end();) {
            if (it->script == script && !tableBeat(it->beat.kind)) {
                d.drop(*it);
                it = q->erase(it);
            } else {
                ++it;
            }
        }
    }
}

void Director::setObserver(std::function<void(const Beat&)> observer) { d_->observer = std::move(observer); }

}  // namespace coach
