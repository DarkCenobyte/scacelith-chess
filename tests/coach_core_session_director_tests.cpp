// The coach's director (src/coach/director.*), the paths the other director tests leave out: a line
// whose key the catalog does not have (nothing said, the script goes on); an Urgent line ending a
// pause; without a voice, an Urgent line cutting the running line at its next punctuation by the
// reading clock (and not cutting a line with no pause left), a mark lit on its word by the reading
// clock; a voice that never reports its end (the watchdog); a set-up fading the marks kept for a
// rewind; a change of language or speed (the lines made ahead made again) or of the subtitles only
// (nothing made again); more lines synthesised ahead than the cache keeps (the oldest goes); a Low
// line still being prepared dropped when the player acts; and a stage that cannot hurry its table.
// On the fake Stage of tests/coach_fakes.h, and a plain Stage of this file.
#include "test.h"
#include "coach_fakes.h"

#include "coach/catalog.h"
#include "coach/director.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

using namespace coach;
using chess::parseSquare;

namespace {

struct Rig {
    fake::Stage stage;
    Director dir;
    float dt = 1.0f / 60.0f;

    explicit Rig(bool voice = true) {
        stage.voice = voice;
        dir.reset(&stage, DirectorConfig());
    }
    void step() {
        stage.advance(dt);
        dir.update(dt, 0);
    }
    bool until(const std::function<bool()>& cond, float seconds) {
        for (float t = 0.0f; t < seconds; t += dt) {
            if (cond()) return true;
            step();
        }
        return cond();
    }
    void run(float seconds) {
        for (float t = 0.0f; t < seconds; t += dt) step();
    }
    bool settle(float seconds = 60.0f) {
        return until([&] { return dir.idle() && !stage.bodyBusy(); }, seconds);
    }
    int voiceStarts() const { return stage.count("voice.start"); }
    bool started(const std::string& key) const {
        for (const fake::Stage::Ev& e : stage.ev)
            if (e.kind == "voice.start" && isKey(e.text, key)) return true;
        return false;
    }
    // The text is one of the key's renderings (any variant), spoken or written, in English.
    static bool isKey(const std::string& text, const std::string& key, const Line* line = nullptr) {
        const Catalog& cat = Catalog::shared();
        const Line l = line ? *line : Line{key, {}};
        for (int v = 1; v <= (std::max)(1, cat.variants(key)); ++v)
            for (bool spoken : {true, false})
                if (cat.renderVariant(l, "en", spoken, v).text == text) return true;
        return false;
    }
    // The subtitles shown, the empty ones (a line taken off) aside.
    std::vector<fake::Stage::Ev> subtitles() const {
        std::vector<fake::Stage::Ev> out;
        for (const fake::Stage::Ev& e : stage.all("subtitle"))
            if (!e.text.empty()) out.push_back(e);
        return out;
    }
};

Beat say(const Line& l, Priority p = Priority::Normal, int ply = -1) {
    Beat b;
    b.kind = BeatKind::Say;
    b.line = l;
    b.priority = p;
    b.ply = ply;
    return b;
}
Beat say(const char* key, Priority p = Priority::Normal, int ply = -1) { return say(Line{key, {}}, p, ply); }

Line squareLine(chess::Square sq = parseSquare("e4")) {
    Line l{"lesson.welcome.square", {}};
    l.with("sq", Arg::ofSquare(sq));
    return l;
}

Mark squareMark(const char* sq, const char* anchor, bool untilRewind = false) {
    Mark m;
    m.kind = Mark::Kind::Square;
    m.square = parseSquare(sq);
    m.anchor = anchor;
    m.untilRewind = untilRewind;
    return m;
}

Beat pause(float s) {
    Beat b;
    b.kind = BeatKind::Pause;
    b.seconds = s;
    return b;
}

Beat tableBeat(BeatKind kind, const char* uci = "", int count = 1) {
    Beat b;
    b.kind = kind;
    b.uci = uci;
    b.count = count;
    b.look = Look::Board;
    b.skippable = false;
    return b;
}

// A stage with no voice, a table busy for a few frames per action, and no way to hurry it (the
// default Stage::hurryTable).
class PlainStage : public Stage {
public:
    int table = 0, demos = 0, rewinds = 0;
    std::vector<std::string> shown;

    void advance() {
        if (table > 0) --table;
    }
    bool voiceAvailable() const override { return false; }
    uint32_t requestSpeech(const std::string&, const std::string&, float, int) override { return 0; }
    bool takeSpeech(uint32_t, std::vector<float>&) override { return false; }
    bool speechFailed(uint32_t) const override { return true; }
    void cancelSpeech(uint32_t) override {}
    int speechSampleRate() const override { return 16000; }
    bool startVoice(std::vector<float>&&) override { return false; }
    void stopVoice() override {}
    void pauseVoice(bool) override {}
    double voiceClock(bool* finished) const override {
        if (finished) *finished = true;
        return -1.0;
    }
    void showSubtitle(const std::string& written, float, bool) override { shown.push_back(written); }
    float readingTime(const std::string&) const override { return 0.5f; }
    void look(Look, chess::Square) override {}
    void gesture(const Gesture&, float) override {}
    void endGestures() override {}
    void speechLevel(float) override {}
    void demoMove(const std::string&, float) override {
        ++demos;
        table = 30;
    }
    void rewindDemo(int, bool) override {
        ++rewinds;
        table = 30;
    }
    void takeBack(int) override {}
    void setPosition(const std::string&) override { table = 5; }
    void playLessonMove(const std::string&) override { table = 5; }
    bool tableBusy() const override { return table > 0; }
    bool bodyBusy() const override { return tableBusy(); }
    void showTakebackOffer(bool) override {}
    void showSkipHint(bool) override {}
    void prewarmGlyphs(const std::string&) override {}
};

}  // namespace

TEST(coach_core_session_director_unknown_key_says_nothing) {
    Rig rig;
    rig.dir.play({say("zz.no.such.key"), say("event.your_move")});
    CHECK(rig.settle());
    CHECK_EQ(rig.voiceStarts(), 1);
    CHECK(rig.started("event.your_move"));
    CHECK_EQ(rig.stage.reqs.size(), size_t(1));      // nothing synthesised for it
    CHECK_EQ(rig.stage.count("prewarm"), 1);         // nor any glyph to draw
}

TEST(coach_core_session_director_urgent_ends_a_pause) {
    Rig rig;
    rig.dir.play({pause(5.0f), say("event.your_move")});
    rig.run(0.5f);
    CHECK(rig.dir.busy());
    CHECK_EQ(rig.voiceStarts(), 0);
    const double asked = rig.stage.now;
    rig.dir.play({say("ann.check.human", Priority::Urgent)});
    CHECK(rig.settle());
    const auto starts = rig.stage.all("voice.start");
    CHECK_EQ(starts.size(), size_t(2));
    if (starts.size() == 2) {
        CHECK(Rig::isKey(starts[0].text, "ann.check.human"));
        CHECK(starts[0].t - asked < 0.5);              // not after the rest of the pause
        CHECK(Rig::isKey(starts[1].text, "event.your_move"));
        CHECK(starts[1].t - asked < 4.0);              // the pause is over: not resumed
    }
}

TEST(coach_core_session_director_voiceless_urgent_cut) {
    // "Hello! I'm your coach. ...": cut after "Hello!" by the reading clock.
    Rig rig(false);
    rig.dir.play({say("lesson.welcome.hello"), say("event.your_move")});
    CHECK(rig.until([&] { return !rig.subtitles().empty(); }, 5.0f));
    const fake::Stage::Ev hello = rig.subtitles().front();
    CHECK(Rig::isKey(hello.text, "lesson.welcome.hello"));
    rig.run(0.1f);
    rig.dir.play({say("ann.check.human", Priority::Urgent)});
    CHECK(rig.settle());
    std::vector<fake::Stage::Ev> subs = rig.subtitles();
    CHECK_EQ(subs.size(), size_t(3));   // the cut line is not shown again
    if (subs.size() == 3) {
        CHECK(Rig::isKey(subs[1].text, "ann.check.human"));
        CHECK(Rig::isKey(subs[2].text, "event.your_move"));
        const double cut = subs[1].t - hello.t;
        CHECK(cut > 0.1 && cut < 0.25 * double(hello.a));   // at "Hello!", long before its end
    }

    // "Your move.": no pause left in it, so it is read to its end before the Urgent line.
    Rig rig2(false);
    rig2.dir.play({say("event.your_move")});
    CHECK(rig2.until([&] { return !rig2.subtitles().empty(); }, 5.0f));
    const fake::Stage::Ev mine = rig2.subtitles().front();
    rig2.run(0.1f);
    rig2.dir.play({say("ann.check.human", Priority::Urgent)});
    CHECK(rig2.settle());
    subs = rig2.subtitles();
    CHECK_EQ(subs.size(), size_t(2));
    if (subs.size() == 2) CHECK(subs[1].t - mine.t >= double(mine.a) - 0.02);
}

TEST(coach_core_session_director_voiceless_mark_on_its_word) {
    // "... This square is e4.": the mark lights when the reading reaches "e4", not before.
    Rig rig(false);
    Beat b = say(squareLine());
    b.marks.push_back(squareMark("e4", "sq"));
    rig.dir.play({b});
    CHECK(rig.until([&] { return !rig.subtitles().empty(); }, 5.0f));
    const fake::Stage::Ev shown = rig.subtitles().front();
    rig.step();
    CHECK(rig.dir.marks().empty());
    CHECK(rig.until([&] { return !rig.dir.marks().empty(); }, 10.0f));
    CHECK(rig.stage.now - shown.t > 0.5 * double(shown.a));
    CHECK_EQ(rig.dir.marks().front().square, parseSquare("e4"));
    CHECK(rig.settle());
    rig.run(1.0f);
    CHECK(rig.dir.marks().empty());   // held, then faded
}

TEST(coach_core_session_director_voice_watchdog) {
    // The audio device never plays the line to its end (its clock stops): the line is over 1.5 s
    // after its length, the voice stopped, and the script goes on.
    Rig rig;
    rig.dir.play({say("event.your_move"), say("lesson.welcome.hello")});
    CHECK(rig.until([&] { return rig.voiceStarts() > 0; }, 5.0f));
    const fake::Stage::Ev start = rig.stage.all("voice.start").front();
    rig.stage.vpaused = true;
    CHECK(rig.until([&] { return rig.stage.count("voice.stop") > 0; }, 10.0f));
    const double at = rig.stage.all("voice.stop").front().t - start.t;
    CHECK(at > double(start.a) + 1.4 && at < double(start.a) + 1.7);
    CHECK(rig.settle());
    CHECK(rig.started("lesson.welcome.hello"));
    CHECK_EQ(rig.stage.count("voice.stop"), 1);
}

TEST(coach_core_session_director_set_up_fades_marks) {
    // A mark kept until the next rewind goes when a new position is set up instead.
    Rig rig;
    Beat b = say(squareLine());
    b.marks.push_back(squareMark("e4", "", true));
    Beat set = tableBeat(BeatKind::SetPosition);
    set.fen = "4k3/8/8/8/8/8/8/4K3 w - - 0 1";
    rig.dir.play({b, pause(1.0f), set});
    CHECK(rig.until([&] { return rig.stage.count("setPosition") > 0; }, 20.0f));
    CHECK_EQ(rig.dir.marks().size(), size_t(1));
    const float before = rig.dir.marks().empty() ? 0.0f : rig.dir.marks().front().strength;
    CHECK(before > 0.9f);
    rig.step();
    CHECK(!rig.dir.marks().empty() && rig.dir.marks().front().strength < before);   // fading at once
    rig.run(0.5f);
    CHECK(rig.dir.marks().empty());
    CHECK(rig.settle());
}

TEST(coach_core_session_director_config_changes) {
    Rig rig;
    rig.dir.prefetch({squareLine()});
    rig.dir.play({say("lesson.welcome.hello"), say("event.your_move")});
    CHECK(rig.until([&] { return rig.voiceStarts() > 0; }, 5.0f));
    rig.run(0.2f);   // the next line synthesised meanwhile
    CHECK_EQ(rig.stage.reqs.size(), size_t(3));
    // The subtitles only: nothing made again.
    DirectorConfig cfg = rig.dir.config();
    cfg.subtitles = 1;
    rig.dir.setConfig(cfg);
    CHECK_EQ(rig.dir.config().subtitles, 1);
    CHECK(rig.settle());
    CHECK_EQ(rig.stage.reqs.size(), size_t(3));
    CHECK_EQ(rig.voiceStarts(), 2);

    // Another language while a line is said and the next one is ready: that one is made again in
    // the new language; the line made ahead too, when it is said.
    rig.dir.play({say("lesson.welcome.hello"), say("event.your_move")});
    CHECK(rig.until([&] { return rig.voiceStarts() == 3; }, 5.0f));
    rig.run(0.2f);
    const size_t made = rig.stage.reqs.size();
    cfg.uiLanguage = "fr";
    rig.dir.setConfig(cfg);
    CHECK_EQ(rig.dir.config().uiLanguage, std::string("fr"));
    CHECK_EQ(rig.stage.count("voice.stop"), 0);   // the running line goes on
    CHECK(rig.settle());
    CHECK_EQ(rig.stage.reqs.size(), made + 1);
    CHECK_EQ(rig.stage.reqs.back().lang, std::string("fr"));
    CHECK_EQ(rig.stage.all("voice.start").back().text, rig.stage.reqs.back().text);
    rig.dir.play({say(squareLine())});
    CHECK(rig.settle());
    CHECK_EQ(rig.stage.reqs.size(), made + 2);
    CHECK_EQ(rig.stage.reqs.back().lang, std::string("fr"));

    // Another speed: the same.
    rig.dir.prefetch({squareLine(parseSquare("d5"))});
    CHECK_EQ(rig.stage.reqs.size(), made + 3);
    cfg.speed = 0.8f;
    rig.dir.setConfig(cfg);
    rig.dir.play({say(squareLine(parseSquare("d5")))});
    CHECK(rig.settle());
    CHECK_EQ(rig.stage.reqs.size(), made + 4);
    CHECK(rig.stage.reqs.back().speed < 0.81f && rig.stage.reqs.back().speed > 0.79f);
}

TEST(coach_core_session_director_prefetch_bounded) {
    // 65 lines made ahead: the cache keeps the last 64, the first one is cancelled and made again
    // when it is said; the last one is said from the cache.
    Rig rig;
    std::vector<Line> lines;
    for (int sq = 0; sq < 64; ++sq) lines.push_back(squareLine(chess::Square(sq)));
    lines.push_back(Line{"event.your_move", {}});
    rig.dir.prefetch(lines);
    CHECK_EQ(rig.stage.reqs.size(), size_t(65));
    if (rig.stage.reqs.size() != 65) return;
    CHECK(rig.stage.reqs[0].cancelled);
    int kept = 0;
    for (size_t i = 1; i < rig.stage.reqs.size(); ++i) kept += rig.stage.reqs[i].cancelled ? 0 : 1;
    CHECK_EQ(kept, 64);
    rig.dir.play({say(lines.front()), say(lines.back())});
    CHECK(rig.settle());
    CHECK_EQ(rig.stage.reqs.size(), size_t(66));
    CHECK_EQ(rig.voiceStarts(), 2);
}

TEST(coach_core_session_director_low_line_dropped_while_prepared) {
    // A Low line whose synthesis is not ready yet is dropped when the player acts; a Normal one
    // in the same state is kept.
    for (Priority p : {Priority::Low, Priority::Normal}) {
        Rig rig;
        rig.stage.synthUpdates = 30;
        rig.dir.play({say("event.filler", p, 0)});
        rig.run(0.1f);
        CHECK(rig.dir.busy());
        CHECK(!rig.dir.speaking());
        rig.dir.playerActed();
        CHECK_EQ(rig.dir.idle(), p == Priority::Low);
        CHECK(rig.settle());
        CHECK_EQ(rig.voiceStarts(), p == Priority::Low ? 0 : 1);
        CHECK_EQ(rig.stage.reqs.size(), size_t(1));
        if (!rig.stage.reqs.empty()) CHECK_EQ(rig.stage.reqs[0].cancelled, p == Priority::Low);
    }
}

TEST(coach_core_session_director_plain_stage_rewind_not_hurried) {
    // Space during a rewind on a stage that cannot hurry its table: the rewind is marked fast
    // (nothing more to skip), and it ends at the table's own pace.
    PlainStage stage;
    Director dir;
    dir.reset(&stage, DirectorConfig());
    dir.play({tableBeat(BeatKind::DemoMove, "e2e4"), tableBeat(BeatKind::Rewind, "", 1), say("event.your_move")});
    const float dt = 1.0f / 60.0f;
    auto step = [&] {
        stage.advance();
        dir.update(dt, 0);
    };
    for (int i = 0; i < 600 && stage.rewinds == 0; ++i) step();
    CHECK_EQ(stage.demos, 1);
    CHECK_EQ(stage.rewinds, 1);
    step();
    CHECK(dir.skippable());
    dir.skip();
    CHECK(!dir.skippable());
    CHECK(stage.tableBusy());   // not hurried
    int frames = 0;
    for (; frames < 600 && !dir.idle(); ++frames) step();
    CHECK(dir.idle());
    CHECK(frames > 25);   // the rest of the table's 30 frames, then the line's reading time
    CHECK(!stage.shown.empty() && Rig::isKey(stage.shown.front(), "event.your_move"));
}
