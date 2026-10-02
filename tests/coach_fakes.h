// Fakes of the coach's world for the director and session tests (coach/stage.h): a Stage that
// records every call with its time, synthesises "speech" as tone bursts (one per character) with
// 150 ms silences at the punctuation where a voice may pause, plays it on a voice clock advanced
// by the test's frames, and keeps its table busy for a few frames per action; and an Analyst that
// answers scripted analyses keyed by position and request shape after a few polls. Like the
// scene's stage, a takeback undoes the game only when its table action starts (a frame after the
// call at the earliest, once the action running is over). A pause asked by the director holds
// until it releases it, and applies to the utterances started meanwhile too: a pause the director
// never releases mutes the coach here (stricter than CoachStage, whose startVoice also clears it),
// so a missing release shows in the tests.
#pragma once
#include "ai/analysis.h"
#include "chess/chess.h"
#include "coach/pacing.h"
#include "coach/stage.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fake {

// Synthetic speech of a text: tone bursts, silences at the pause boundaries.
struct Synth {
    std::vector<float> pcm;
    std::vector<float> onset;   // per byte of the text: when its character starts (s)
    struct Gap { float start = 0.0f, end = 0.0f; };
    std::vector<Gap> gaps;      // the silences at punctuation, in order
    float duration = 0.0f;
};

inline Synth synthesize(const std::string& text, int rate, float charTime = 0.055f, float gapTime = 0.15f,
                        float lead = 0.05f) {
    Synth s;
    const std::vector<size_t> bounds = coach::pauseBoundaries(text);
    s.onset.assign(text.size() + 1, 0.0f);
    double t = lead;
    auto emit = [&](double seconds, bool tone) {
        const size_t n = size_t(std::lround(seconds * rate));
        const size_t base = s.pcm.size();
        for (size_t i = 0; i < n; ++i)
            s.pcm.push_back(tone ? 0.12f * float(std::sin(2.0 * 3.14159265358979 * 200.0 * double(base + i) / rate)) : 0.0f);
    };
    emit(lead, false);
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        for (size_t k = i; k < i + len && k < text.size(); ++k) s.onset[k] = float(t);
        const bool weighted = c >= 0x80 || std::isalnum(c) || c == ' ';
        if (weighted) {
            emit(charTime, true);
            t += charTime;
        }
        if (std::find(bounds.begin(), bounds.end(), i) != bounds.end()) {
            Synth::Gap g;
            g.start = float(t);
            emit(gapTime, false);
            t += gapTime;
            g.end = float(t);
            s.gaps.push_back(g);
        }
        i += len;
    }
    s.onset[text.size()] = float(t);
    emit(lead, false);
    s.duration = float(s.pcm.size()) / float(rate);
    return s;
}

class Stage : public coach::Stage {
public:
    // ---- Settings -----------------------------------------------------------------------------------
    bool voice = true;
    int rate = 16000;
    int synthUpdates = 3;                   // frames before a synthesis is ready
    std::vector<std::string> failIf;        // a request whose text holds one of these fails
    int refuseStarts = 0;                   // startVoice refusals left
    int tableUpdates = 6;                   // frames a table action stays busy
    int takeBackWait = 1;                   // frames a takeback waits for the free table before its undo
    float gestureHold = 0.5f;               // the hand is busy until a gesture's apex + this
    float retract = 0.3f;                   // ... and this long after endGestures()
    chess::Game* game = nullptr;            // takeBack / setPosition / playLessonMove act on it

    // ---- Record -------------------------------------------------------------------------------------
    struct Ev {
        double t = 0.0;
        std::string kind, text;
        float a = 0.0f;
        int n = 0;
        bool flag = false;
        coach::Gesture gesture;
    };
    std::vector<Ev> ev;
    struct Req {
        uint32_t id = 0;
        std::string text, lang;
        float speed = 1.0f;
        int priority = 0;
        double at = 0.0;
        int left = 0;
        bool cancelled = false, taken = false, failed = false;
        Synth synth;
    };
    std::vector<Req> reqs;
    std::vector<float> levels;
    double now = 0.0;
    // The voice.
    bool vstarted = false, vplaying = false, vpaused = false;
    bool dpaused = false;                   // pauseVoice(true) not released yet
    double vclock = 0.0, vdur = 0.0;
    std::string vtext;
    int table = 0;
    int undoPlies = 0, undoWait = 0;        // a takeback asked for, not started yet
    double bodyUntil = 0.0;
    int lessonMoves = 0;                    // playLessonMove calls not yet reported by the test

    void advance(float dt) {
        now += dt;
        for (Req& r : reqs)
            if (r.left > 0) --r.left;
        if (vplaying && !vpaused) {
            vclock += dt;
            if (vclock >= vdur) {
                vclock = vdur;
                vplaying = false;
                log("voice.end", vtext);
            }
        }
        if (table > 0) --table;
        if (undoPlies > 0 && table == 0) {
            if (undoWait > 0) {
                --undoWait;
            } else {
                if (game) game->undo(undoPlies);
                undoPlies = 0;
                table = tableUpdates;
            }
        }
    }

    Ev& log(const std::string& kind, const std::string& text = std::string()) {
        Ev e;
        e.t = now;
        e.kind = kind;
        e.text = text;
        ev.push_back(e);
        return ev.back();
    }
    std::vector<Ev> all(const std::string& kind) const {
        std::vector<Ev> out;
        for (const Ev& e : ev)
            if (e.kind == kind) out.push_back(e);
        return out;
    }
    int count(const std::string& kind) const { return int(all(kind).size()); }
    const Req* request(const std::string& text) const {
        for (const Req& r : reqs)
            if (r.text == text) return &r;
        return nullptr;
    }

    // ---- coach::Stage -------------------------------------------------------------------------------
    bool voiceAvailable() const override { return voice; }
    uint32_t requestSpeech(const std::string& text, const std::string& lang, float speed, int priority) override {
        if (!voice) return 0;
        Req r;
        r.id = uint32_t(reqs.size() + 1);
        r.text = text;
        r.lang = lang;
        r.speed = speed;
        r.priority = priority;
        r.at = now;
        r.left = synthUpdates;
        for (const std::string& f : failIf)
            if (text.find(f) != std::string::npos) r.failed = true;
        r.synth = synthesize(text, rate);
        reqs.push_back(r);
        log("request", text).n = int(r.id);
        return r.id;
    }
    bool takeSpeech(uint32_t id, std::vector<float>& pcm) override {
        if (id == 0 || id > reqs.size()) return false;
        Req& r = reqs[id - 1];
        if (r.cancelled || r.taken || r.failed || r.left > 0) return false;
        r.taken = true;
        pcm = r.synth.pcm;
        return true;
    }
    bool speechFailed(uint32_t id) const override {
        return id == 0 || id > reqs.size() || reqs[id - 1].cancelled || (reqs[id - 1].failed && reqs[id - 1].left == 0);
    }
    void cancelSpeech(uint32_t id) override {
        log("cancel").n = int(id);
        for (Req& r : reqs)
            if (id == 0 || r.id == id) r.cancelled = true;
    }
    int speechSampleRate() const override { return rate; }
    bool startVoice(std::vector<float>&& pcm) override {
        if (refuseStarts > 0) {
            --refuseStarts;
            log("voice.refused");
            return false;
        }
        std::string text;
        for (const Req& r : reqs)
            if (r.taken && r.synth.pcm == pcm) text = r.text;
        vstarted = vplaying = true;
        vpaused = dpaused;
        vclock = 0.0;
        vdur = double(pcm.size()) / rate;
        vtext = text;
        log("voice.start", text).a = float(vdur);
        return true;
    }
    void stopVoice() override {
        if (vplaying) log("voice.stop", vtext).a = float(vclock);
        vplaying = false;
    }
    void pauseVoice(bool paused) override {
        vpaused = dpaused = paused;
        log("voice.pause").flag = paused;
    }
    double voiceClock(bool* finished) const override {
        if (finished) *finished = vstarted && !vplaying;
        return vstarted ? vclock : -1.0;
    }
    void showSubtitle(const std::string& written, float hold, bool unheard) override {
        Ev& e = log("subtitle", written);
        e.a = hold;
        e.flag = unheard;
    }
    float readingTime(const std::string& written) const override { return 0.4f + float(written.size()) / 15.0f; }
    void look(coach::Look look, chess::Square target) override {
        Ev& e = log("look");
        e.n = int(look);
        e.a = float(target);
    }
    void gesture(const coach::Gesture& g, float apexIn) override {
        Ev& e = log("gesture");
        e.gesture = g;
        e.a = apexIn;
        if (g.kind != coach::GestureKind::Nod && g.kind != coach::GestureKind::ShakeHead)
            bodyUntil = std::max(bodyUntil, now + apexIn + gestureHold);
    }
    void endGestures() override {
        log("endGestures");
        bodyUntil = now + retract;
    }
    void speechLevel(float level) override { levels.push_back(level); }
    void demoMove(const std::string& uci, float pause) override {
        log("demoMove", uci).a = pause;
        table = tableUpdates;
    }
    void rewindDemo(int plies, bool fast) override {
        Ev& e = log("rewindDemo");
        e.n = plies;
        e.flag = fast;
        table = tableUpdates;
    }
    void hurryTable() override { log("hurryTable"); }
    void takeBack(int plies) override {
        log("takeBack").n = plies;
        undoPlies += plies;
        undoWait = takeBackWait;
    }
    void setPosition(const std::string& fen) override {
        log("setPosition", fen);
        if (game) game->resetFromFEN(fen);
        table = tableUpdates;
    }
    void playLessonMove(const std::string& uci) override {
        log("playLessonMove", uci);
        if (game) {
            const chess::Move m = game->position().parseUCI(uci);
            if (m.valid()) game->play(m);
            ++lessonMoves;
        }
        table = tableUpdates;
    }
    bool tableBusy() const override { return table > 0 || undoPlies > 0; }
    bool bodyBusy() const override { return now < bodyUntil || tableBusy(); }
    void showTakebackOffer(bool shown) override { log("offer").flag = shown; }
    void showSkipHint(bool shown) override { log("skipHint").flag = shown; }
    void prewarmGlyphs(const std::string& written) override { log("prewarm", written); }
};

// Scripted analyses: results[fen + "|" + shape] with shape A0 (MultiPV 3), A1 (searchmoves), A2
// (MultiPV 2), A3 (depth 6), E (background evaluation). Anything else gets a level line (cp 0) on
// the first legal move (or the searched move). Ready after 'delay' polls.
class Analyst : public coach::Analyst {
public:
    bool engine = true;
    int delay = 3;
    std::map<std::string, ai::Analysis> results;
    struct Job {
        uint32_t id = 0;
        ai::AnalysisRequest req;
        std::string fen, shape;
        int left = 0;
        bool stopped = false;
    };
    std::vector<Job> jobs;
    std::vector<Job> asked;   // every request, in order

    static std::string shapeOf(const ai::AnalysisRequest& r) {
        if (!r.searchMoves.empty()) return "A1";
        if (r.priority < 0) return "E";
        if (r.depth == 6) return "A3";
        if (r.multiPV == 2) return "A2";
        return "A0";
    }
    static std::string fenOf(const ai::AnalysisRequest& r) {
        chess::Game g;
        if (!r.startFen.empty()) g.resetFromFEN(r.startFen);
        for (const std::string& u : r.moves) {
            const chess::Move m = g.position().parseUCI(u);
            if (m.valid()) g.play(m);
        }
        return g.position().fen();
    }
    int count(const std::string& shape, const std::string& fen = std::string()) const {
        int n = 0;
        for (const Job& j : asked)
            if (j.shape == shape && (fen.empty() || j.fen == fen)) ++n;
        return n;
    }

    uint32_t analyse(const ai::AnalysisRequest& r) override {
        if (!engine) return 0;
        Job j;
        j.id = uint32_t(asked.size() + 1);
        j.req = r;
        j.fen = fenOf(r);
        j.shape = shapeOf(r);
        j.left = delay;
        jobs.push_back(j);
        asked.push_back(j);
        return j.id;
    }
    bool takeAnalysis(uint32_t id, ai::Analysis& out) override {
        for (auto it = jobs.begin(); it != jobs.end(); ++it) {
            if (it->id != id) continue;
            if (--it->left > 0) return false;
            chess::Position p;
            p.setFEN(it->fen);
            auto found = results.find(it->fen + "|" + it->shape);
            if (found != results.end()) {
                out = found->second;
            } else {
                out = ai::Analysis();
                out.ok = true;
                out.depth = 14;
                ai::PvLine l;
                l.depth = 14;
                std::string first = it->req.searchMoves.empty() ? std::string() : it->req.searchMoves.front();
                if (first.empty()) {
                    const std::vector<chess::Move> moves = p.legalMoves();
                    if (!moves.empty()) first = p.toUCI(moves.front());
                }
                if (first.empty()) {
                    out.noLegalMove = true;
                    l.score.matedNow = p.inCheck();
                } else {
                    l.pv.push_back(first);
                }
                out.lines.push_back(l);
                out.bestMove = first;
            }
            out.id = id;
            out.fen = it->fen;
            out.whiteToMove = p.sideToMove() == chess::White;
            jobs.erase(it);
            return true;
        }
        return false;
    }
    void stopAnalysis(uint32_t id) override {
        for (Job& j : jobs)
            if (j.id == id) {
                j.stopped = true;
                j.left = std::min(j.left, 1);
            }
    }
    void cancelAnalysis(uint32_t id) override {
        jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [&](const Job& j) { return id == 0 || j.id == id; }),
                   jobs.end());
    }
    bool idle() const override { return jobs.empty(); }
};

}  // namespace fake
