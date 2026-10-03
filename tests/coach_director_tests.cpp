// The coach's director (src/coach/director.*) on a fake Stage (tests/coach_fakes.h): one seed for
// the written and the spoken rendering, synthesis ahead and lines chained without a gap, gesture
// apexes and marks on their words, marks held after the line and kept until the rewind, Urgent
// lines cutting at a pause, stale and acted-upon Low lines dropped, Space (skip, the offer's card,
// the rewind only hurried), pause, the voice-less mode, a voice the stage refuses, the takeback
// offer, WaitMove with lines said on top of it, and clear() putting the demonstration back.
#include "test.h"
#include "coach_fakes.h"

#include "coach/catalog.h"
#include "coach/director.h"

#include <cmath>
#include <functional>
#include <set>
#include <string>
#include <vector>

using namespace coach;
using chess::parseSquare;

namespace {

struct Rig {
    fake::Stage stage;
    Director dir;
    int ply = 0;
    float dt = 1.0f / 60.0f;

    explicit Rig(bool voice = true, DirectorConfig cfg = DirectorConfig()) {
        stage.voice = voice;
        dir.reset(&stage, cfg);
    }
    void step() {
        stage.advance(dt);
        dir.update(dt, ply);
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
    bool started(const std::string& key) const {
        for (const fake::Stage::Ev& e : stage.ev)
            if (e.kind == "voice.start" && isKey(e.text, key)) return true;
        return false;
    }
    int voiceStarts() const { return stage.count("voice.start"); }

    // The spoken text is one of the key's renderings (any variant) for this rig's language.
    static bool isKey(const std::string& text, const std::string& key, const Line* line = nullptr) {
        const Catalog& cat = Catalog::shared();
        Line l = line ? *line : Line{key, {}};
        for (int v = 1; v <= std::max(1, cat.variants(key)); ++v)
            if (cat.renderVariant(l, "en", true, v).text == text) return true;
        return false;
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

Line hangingLine() {
    Line l{"ex.hanging.b1", {}};
    l.with("your", Arg::ofPiece(chess::Knight, chess::White, true, parseSquare("d5")))
        .with("sq", Arg::ofSquare(parseSquare("d5")))
        .with("my", Arg::ofPiece(chess::Pawn, chess::Black, false, parseSquare("e6")));
    return l;
}

Line squareLine() {
    Line l{"lesson.welcome.square", {}};
    l.with("sq", Arg::ofSquare(parseSquare("e4")));
    return l;
}

Gesture point(GestureKind k, const char* sq, const char* anchor) {
    Gesture g;
    g.kind = k;
    g.square = parseSquare(sq);
    g.anchor = anchor;
    return g;
}

Mark squareMark(const char* sq, const char* anchor, bool untilRewind = false) {
    Mark m;
    m.kind = Mark::Kind::Square;
    m.square = parseSquare(sq);
    m.anchor = anchor;
    m.untilRewind = untilRewind;
    return m;
}

Beat demo(const char* uci, const Line& narration = Line()) {
    Beat b;
    b.kind = BeatKind::DemoMove;
    b.uci = uci;
    b.line = narration;
    b.look = Look::Board;
    return b;
}

Beat pause(float s) {
    Beat b;
    b.kind = BeatKind::Pause;
    b.seconds = s;
    return b;
}

Beat rewind(int n, const char* key = "") {
    Beat b;
    b.kind = BeatKind::Rewind;
    b.count = n;
    b.line.key = key;
    b.skippable = false;
    return b;
}

Beat offer() {
    Beat b;
    b.kind = BeatKind::OfferTakeback;
    b.line.key = "ex.offer.b1";
    Gesture g;
    g.kind = GestureKind::Open;
    b.gestures.push_back(g);
    return b;
}

Beat waitMove(int expect) {
    Beat b;
    b.kind = BeatKind::WaitMove;
    b.expect = expect;
    b.skippable = false;
    return b;
}

float strengthOf(const Director& d) { return d.marks().empty() ? 0.0f : d.marks().front().strength; }

}  // namespace

TEST(coach_director_variant_parity) {
    // One seed per beat: the subtitle and the voice say the same variant (written "d5", spoken
    // "dee five"), in the UI language and the voice's language.
    const Catalog& cat = Catalog::shared();
    const Line l = hangingLine();
    for (const char* ui : {"en", "fr", "zh-Hans"}) {
        DirectorConfig cfg;
        cfg.uiLanguage = ui;
        cfg.subtitles = 1;   // On
        Rig rig(true, cfg);
        for (int i = 0; i < 6; ++i) {
            rig.dir.play({say(l)});
            CHECK(rig.settle());
        }
        std::vector<fake::Stage::Ev> subs;
        for (const fake::Stage::Ev& e : rig.stage.all("subtitle"))
            if (!e.text.empty()) subs.push_back(e);
        CHECK_EQ(subs.size(), size_t(6));
        CHECK_EQ(rig.stage.reqs.size(), size_t(6));
        std::set<int> seen;
        for (size_t i = 0; i < subs.size() && i < rig.stage.reqs.size(); ++i) {
            int variant = 0;
            for (int v = 1; v <= cat.variants(l.key); ++v)
                if (cat.renderVariant(l, ui, false, v).text == subs[i].text) variant = v;
            CHECK(variant > 0);
            if (variant == 0) continue;
            seen.insert(variant);
            CHECK_EQ(rig.stage.reqs[i].text, cat.renderVariant(l, speechLanguage(ui), true, variant).text);
            CHECK_EQ(rig.stage.reqs[i].lang, speechLanguage(ui));
            if (variant == 1) CHECK(subs[i].text != rig.stage.reqs[i].text);   // "d5" written, "dee five" spoken
        }
        CHECK(seen.size() >= 2);   // the coach does not repeat itself word for word
        CHECK(rig.stage.count("prewarm") >= 6);
    }
}

TEST(coach_director_synthesis_ahead_and_gapless) {
    Rig rig;
    rig.dir.play({say("event.your_move"), say("event.take_time"), say("event.play_on"), say("event.filler"),
                  say("event.takeback.taken")});
    rig.step();
    CHECK(rig.stage.reqs.size() >= 4);   // the running line and the next three
    CHECK(rig.settle());
    const auto starts = rig.stage.all("voice.start"), ends = rig.stage.all("voice.end");
    CHECK_EQ(starts.size(), size_t(5));
    CHECK_EQ(ends.size(), size_t(5));
    for (size_t i = 1; i < starts.size() && i < ends.size(); ++i) {
        const double gap = starts[i].t - ends[i - 1].t;
        CHECK(gap >= -1e-9 && gap <= rig.dt + 1e-6);   // chained in the frame the previous one ended
        const fake::Stage::Req* r = rig.stage.request(starts[i].text);
        CHECK(r && r->at < starts[i - 1].t + 1e-9);    // synthesised while the previous one played
    }
    // The mouth moved with the voice, and is closed at the end.
    float top = 0.0f;
    for (float v : rig.stage.levels) top = std::max(top, v);
    CHECK(top > 0.2f);
    CHECK(rig.stage.levels.back() == 0.0f);
}

TEST(coach_director_gesture_apex_and_marks_on_the_word) {
    Rig rig;
    const Line l = squareLine();
    Beat b = say(l);
    b.look = Look::Target;
    b.gestures.push_back(point(GestureKind::PointSquare, "e4", "sq"));
    b.marks.push_back(squareMark("e4", "sq"));
    rig.dir.play({b});
    CHECK(rig.until([&] { return rig.voiceStarts() > 0; }, 5.0f));
    const auto starts = rig.stage.all("voice.start");
    const fake::Stage::Req* r = rig.stage.request(starts.front().text);
    CHECK(r != nullptr);
    if (!r) return;
    const Catalog::Rendered spoken = Catalog::shared().renderVariant(l, "en", true, 1);
    CHECK_EQ(spoken.text, r->text);
    const int off = spoken.anchor("sq");
    CHECK(off > 0);
    const float truth = r->synth.onset[size_t(off)] - 0.1f;   // the word's onset, pointed 0.1 s early

    const auto gestures = rig.stage.all("gesture");
    CHECK_EQ(gestures.size(), size_t(1));
    if (!gestures.empty()) {
        CHECK(std::fabs(gestures[0].a - truth) < 0.15f);
        CHECK(std::fabs(gestures[0].t - starts[0].t) < 1e-9);   // handed over as the voice starts
    }
    const auto looks = rig.stage.all("look");
    CHECK(!looks.empty() && looks.back().n == int(Look::Target) && int(looks.back().a) == parseSquare("e4"));

    // The mark lights on the word, fades in over 0.25 s...
    CHECK(rig.dir.marks().empty());
    CHECK(rig.until([&] { return !rig.dir.marks().empty(); }, 10.0f));
    CHECK(std::fabs(float(rig.stage.vclock) - truth) < 0.15f);
    CHECK(rig.dir.marks().front().square == parseSquare("e4"));
    CHECK(rig.dir.marks().front().strength < 0.2f);
    rig.run(0.3f);
    CHECK(std::fabs(strengthOf(rig.dir) - 1.0f) < 1e-4f);
    // ...stays 0.4 s after the line, then fades out over 0.4 s.
    CHECK(rig.until([&] { return rig.stage.count("voice.end") > 0; }, 10.0f));
    rig.run(0.33f);
    CHECK(std::fabs(strengthOf(rig.dir) - 1.0f) < 1e-4f);
    rig.run(0.25f);
    CHECK(strengthOf(rig.dir) > 0.0f && strengthOf(rig.dir) < 1.0f);
    rig.run(0.3f);
    CHECK(rig.dir.marks().empty());
    CHECK_EQ(rig.stage.count("endGestures"), 1);
}

TEST(coach_director_marks_until_rewind) {
    Rig rig;
    Beat b = say(squareLine());
    b.marks.push_back(squareMark("d5", "", true));
    rig.dir.play({b, demo("e6d5"), pause(0.5f), rewind(1)});
    CHECK(rig.until([&] { return rig.stage.count("demoMove") > 0; }, 20.0f));
    CHECK(std::fabs(strengthOf(rig.dir) - 1.0f) < 1e-4f);   // lit through the demonstration
    CHECK(rig.until([&] { return rig.stage.count("rewindDemo") > 0; }, 20.0f));
    CHECK(strengthOf(rig.dir) > 0.9f);
    rig.run(0.45f);
    CHECK(rig.dir.marks().empty());   // gone with the rewind
    const auto rw = rig.stage.all("rewindDemo");
    CHECK(rw.size() == 1 && rw[0].n == 1 && !rw[0].flag);
    const auto dm = rig.stage.all("demoMove");
    CHECK(dm.size() == 1 && std::fabs(dm[0].a - 0.4f) < 1e-6f);   // the default stillness after it
    CHECK(rig.settle());
}

TEST(coach_director_urgent_cuts_at_a_pause) {
    Rig rig;
    rig.dir.play({say("lesson.welcome.hello"), say("event.your_move")});
    CHECK(rig.until([&] { return rig.voiceStarts() > 0; }, 5.0f));
    const fake::Stage::Ev start = rig.stage.all("voice.start").front();
    rig.run(0.15f);
    rig.dir.play({say("ann.check.human", Priority::Urgent)});
    CHECK(rig.until([&] { return rig.stage.count("voice.stop") > 0; }, 10.0f));
    const fake::Stage::Req* r = rig.stage.request(start.text);
    CHECK(r && !r->synth.gaps.empty());
    if (r && !r->synth.gaps.empty()) {
        const double at = rig.stage.all("voice.stop").front().t - start.t;
        const fake::Synth::Gap g = r->synth.gaps.front();   // "Hello!"
        CHECK(at >= g.start - rig.dt && at <= g.end + rig.dt);
    }
    CHECK(rig.settle());
    const auto starts = rig.stage.all("voice.start");
    CHECK_EQ(starts.size(), size_t(3));   // the cut line is not said again
    if (starts.size() == 3) {
        CHECK(Rig::isKey(starts[1].text, "ann.check.human"));
        CHECK(Rig::isKey(starts[2].text, "event.your_move"));   // its script goes on after
    }
}

TEST(coach_director_low_lines_dropped) {
    // Stale: more than two plies old when their turn comes.
    Rig rig;
    rig.ply = 5;
    rig.dir.play({say("lesson.welcome.hello", Priority::Normal, 5)});
    rig.dir.play({say("event.take_time", Priority::Low, 5)});
    rig.dir.play({say("event.your_move", Priority::Normal, 5)});   // goes before the Low line
    CHECK(rig.until([&] { return rig.voiceStarts() > 0; }, 5.0f));
    rig.ply = 8;
    CHECK(rig.settle());
    CHECK(rig.started("event.your_move"));
    CHECK(!rig.started("event.take_time"));
    CHECK_EQ(rig.voiceStarts(), 2);
    for (const fake::Stage::Req& r : rig.stage.reqs)
        if (Rig::isKey(r.text, "event.take_time")) CHECK(r.cancelled || r.taken);   // cancelled, or its audio dropped

    // Once the player acts.
    Rig rig2;
    rig2.dir.play({say("lesson.welcome.hello")});
    rig2.dir.play({say("event.filler", Priority::Low, 0)});
    CHECK(rig2.until([&] { return rig2.voiceStarts() > 0; }, 5.0f));
    rig2.dir.playerActed();
    CHECK(rig2.settle());
    CHECK(!rig2.started("event.filler"));
    CHECK_EQ(rig2.voiceStarts(), 1);
}

TEST(coach_director_skip) {
    Line narration{"demo.my.take", {}};
    narration.with("your", Arg::ofPiece(chess::Knight, chess::White, true, parseSquare("d5")))
        .with("my", Arg::ofPiece(chess::Pawn, chess::Black, false, parseSquare("e6")));
    auto script = [&] {
        Beat cause = say(hangingLine());
        cause.gestures.push_back(point(GestureKind::PointPiece, "d5", "your"));
        cause.marks.push_back(squareMark("d5", "", true));
        return Script{cause, demo("e6d5", narration), pause(0.5f), rewind(1, "ex.rewind.b1"), say("event.your_move")};
    };
    // Space on the explanation: the demonstration and what follows go; nothing to rewind.
    {
        Rig rig;
        rig.dir.play(script());
        CHECK(rig.until([&] { return rig.voiceStarts() > 0; }, 5.0f));
        rig.step();
        CHECK(rig.dir.skippable());
        rig.dir.skip();
        CHECK_EQ(rig.stage.count("voice.stop"), 1);
        CHECK(rig.settle());
        CHECK_EQ(rig.stage.count("demoMove"), 0);
        CHECK_EQ(rig.stage.count("rewindDemo"), 0);
        CHECK_EQ(rig.voiceStarts(), 1);
        rig.run(0.45f);
        CHECK(rig.dir.marks().empty());   // the kept mark faded when the (empty) rewind came
    }
    // skipCurrent() (the rules lesson): the explanation only; the rest of the script goes on.
    {
        Rig rig;
        rig.dir.play(script());
        CHECK(rig.until([&] { return rig.voiceStarts() > 0; }, 5.0f));
        rig.step();
        rig.dir.skipCurrent();
        CHECK_EQ(rig.stage.count("voice.stop"), 1);
        CHECK(rig.settle());
        CHECK_EQ(rig.stage.count("demoMove"), 1);
        CHECK_EQ(rig.stage.count("rewindDemo"), 1);
        CHECK(rig.started("event.your_move"));
    }
    // skipCurrent() on a demonstration not begun yet (the hand still busy): its narration only; the
    // move is still played and taken back, for the lines after it speak of it.
    {
        Rig rig;
        rig.dir.play(script());
        CHECK(rig.until([&] { return rig.stage.count("voice.end") > 0; }, 10.0f));
        CHECK(rig.stage.bodyBusy());
        CHECK_EQ(rig.stage.count("demoMove"), 0);
        CHECK(rig.dir.skippable());
        rig.dir.skipCurrent();
        CHECK(rig.settle());
        CHECK_EQ(rig.stage.count("demoMove"), 1);
        CHECK_EQ(rig.stage.count("rewindDemo"), 1);
        CHECK_EQ(rig.voiceStarts(), 3);   // the explanation, the rewind's line, event.your_move
        CHECK(rig.started("ex.rewind.b1"));
        CHECK(rig.started("event.your_move"));
    }
    // skipCurrent() during the demonstration's narration: the narration stops, the move stays and
    // the rewind runs at its pace.
    {
        Rig rig;
        rig.dir.play(script());
        CHECK(rig.until([&] { return rig.stage.count("demoMove") > 0; }, 20.0f));
        rig.step();
        CHECK(rig.dir.speaking());
        rig.dir.skipCurrent();
        CHECK_EQ(rig.stage.count("voice.stop"), 1);
        CHECK(!rig.dir.speaking());
        CHECK(rig.settle());
        const auto rw = rig.stage.all("rewindDemo");
        CHECK(rw.size() == 1 && rw[0].n == 1 && !rw[0].flag);
        CHECK(rig.started("event.your_move"));
    }
    // Space during the demonstration: its narration stops, the rewind still runs, briskly.
    {
        Rig rig;
        rig.dir.play(script());
        CHECK(rig.until([&] { return rig.stage.count("demoMove") > 0; }, 20.0f));
        rig.step();
        rig.dir.skip();
        CHECK(rig.settle());
        const auto rw = rig.stage.all("rewindDemo");
        CHECK(rw.size() == 1 && rw[0].n == 1 && rw[0].flag);
        CHECK(!rig.started("event.your_move"));
        CHECK(rig.started("ex.rewind.b1"));   // the rewind is never skipped
    }
    // Space during the rewind: it only goes faster, and the script goes on.
    {
        Rig rig;
        rig.dir.play(script());
        CHECK(rig.until([&] { return rig.stage.count("rewindDemo") > 0; }, 30.0f));
        rig.step();
        CHECK(rig.dir.skippable());
        rig.dir.skip();
        CHECK_EQ(rig.stage.count("hurryTable"), 1);
        CHECK(!rig.dir.skippable());
        rig.dir.skip();
        CHECK_EQ(rig.stage.count("hurryTable"), 1);
        CHECK(rig.settle());
        CHECK(rig.started("event.your_move"));
    }
    // A beat that is not skippable.
    {
        Rig rig;
        Beat b = say("event.end.handshake");
        b.skippable = false;
        rig.dir.play({b});
        CHECK(rig.until([&] { return rig.voiceStarts() > 0; }, 5.0f));
        CHECK(!rig.dir.skippable());
        rig.dir.skip();
        CHECK_EQ(rig.stage.count("voice.stop"), 0);
    }
}

TEST(coach_director_pause_holds_everything) {
    Rig rig;
    Beat b = say("lesson.welcome.hello");
    b.marks.push_back(squareMark("e4", ""));
    rig.dir.play({b, say("event.your_move")});
    CHECK(rig.until([&] { return rig.voiceStarts() > 0; }, 5.0f));
    rig.run(0.5f);
    rig.dir.setPaused(true);
    const auto pauses = rig.stage.all("voice.pause");
    CHECK(pauses.size() == 1 && pauses[0].flag);
    rig.step();
    const double clock = rig.stage.vclock;
    const float age = rig.dir.marks().empty() ? -1.0f : rig.dir.marks().front().age;
    CHECK(age > 0.0f);
    rig.run(3.0f);
    CHECK(rig.stage.vclock == clock);
    CHECK(!rig.dir.marks().empty() && rig.dir.marks().front().age == age);
    CHECK_EQ(rig.voiceStarts(), 1);
    CHECK(rig.stage.levels.back() == 0.0f);
    rig.dir.setPaused(false);
    CHECK(!rig.stage.all("voice.pause").back().flag);
    CHECK(rig.settle());
    CHECK_EQ(rig.voiceStarts(), 2);
}

TEST(coach_director_pause_released_after_clear) {
    // Paused while a line is heard, then cleared (the pause menu's Take back or Resign): the pause
    // is released on resuming, so the next line is heard to its end.
    Rig rig;
    rig.dir.play({say("lesson.welcome.hello")});
    CHECK(rig.until([&] { return rig.voiceStarts() > 0; }, 5.0f));
    rig.run(0.3f);
    rig.dir.setPaused(true);
    rig.dir.clear();
    rig.step();
    rig.dir.setPaused(false);
    CHECK(!rig.stage.all("voice.pause").back().flag);
    rig.dir.play({say("event.your_move")});
    CHECK(rig.settle());
    CHECK_EQ(rig.voiceStarts(), 2);
    CHECK_EQ(rig.stage.count("voice.end"), 1);

    // Back to the main menu while paused: the next game's reset() releases it.
    Rig rig2;
    rig2.dir.play({say("lesson.welcome.hello")});
    CHECK(rig2.until([&] { return rig2.voiceStarts() > 0; }, 5.0f));
    rig2.run(0.3f);
    rig2.dir.setPaused(true);
    rig2.dir.clear();
    rig2.dir.reset(&rig2.stage, DirectorConfig());
    rig2.dir.play({say("event.your_move")});
    CHECK(rig2.settle());
    CHECK_EQ(rig2.voiceStarts(), 2);
    CHECK_EQ(rig2.stage.count("voice.end"), 1);
}

TEST(coach_director_without_voice) {
    // No voice: nothing is synthesised, every line is shown for its reading time and the
    // gestures are timed on it.
    Rig rig(false);
    Beat b = say(squareLine());
    b.gestures.push_back(point(GestureKind::PointSquare, "e4", "sq"));
    rig.dir.play({b, say("event.your_move")});
    CHECK(rig.settle());
    CHECK(rig.stage.reqs.empty());
    const auto subs = rig.stage.all("subtitle");
    CHECK(subs.size() >= 2);
    if (subs.size() >= 2) {
        const float reading = rig.stage.readingTime(subs[0].text);
        CHECK(std::fabs(subs[0].a - reading) < 1e-6f);
        CHECK(std::fabs(float(subs[1].t - subs[0].t) - reading) <= rig.dt * 1.5f);
        const auto g = rig.stage.all("gesture");
        CHECK(g.size() == 1 && g[0].a > reading * 0.5f && g[0].a < reading);   // "e4" ends the line
    }
    CHECK(rig.dir.marks().empty());

    for (const fake::Stage::Ev& e : subs) CHECK(e.flag || e.text.empty());   // unheard: shown whatever the option

    // A synthesis that fails: that line is shown (even with subtitles off, or in Automatic mode
    // when the voice speaks the UI's language), marked unheard so that the scene draws it too;
    // the others are heard.
    for (int mode : {2, 0}) {   // Off, Automatic
        DirectorConfig cfg;
        cfg.subtitles = mode;
        Rig rig2(true, cfg);
        rig2.stage.failIf.push_back("ee four");
        rig2.dir.play({say(squareLine()), say("event.your_move")});
        CHECK(rig2.settle());
        CHECK_EQ(rig2.voiceStarts(), 1);
        const auto subs2 = rig2.stage.all("subtitle");
        CHECK(subs2.size() == 1 && subs2[0].text.find("e4") != std::string::npos);
        CHECK(!subs2.empty() && subs2[0].flag);
    }
    // The same line synthesised ahead (prefetch) and failed: taken from the cache as it is, shown.
    Rig rig4;
    rig4.stage.failIf.push_back("ee four");
    rig4.dir.prefetch({squareLine()});
    rig4.run(0.2f);
    rig4.dir.play({say(squareLine())});
    CHECK(rig4.settle());
    CHECK_EQ(rig4.voiceStarts(), 0);
    CHECK_EQ(rig4.stage.reqs.size(), size_t(1));
    const auto subs4 = rig4.stage.all("subtitle");
    CHECK(!subs4.empty() && subs4[0].text.find("e4") != std::string::npos && subs4[0].flag);
    // Subtitles On: a heard line is shown, not marked unheard.
    DirectorConfig on;
    on.subtitles = 1;
    Rig rig3(true, on);
    rig3.dir.play({say("event.your_move")});
    CHECK(rig3.settle());
    const auto subs3 = rig3.stage.all("subtitle");
    CHECK(!subs3.empty() && !subs3[0].text.empty() && !subs3[0].flag);
}

// The voice refused by the audio engine: the line starts on a later frame; refused for a second, it
// is shown for its reading time without the voice, and the script goes on.
TEST(coach_director_refused_voice) {
    Rig rig;
    rig.stage.refuseStarts = 1;
    rig.dir.play({say("event.your_move")});
    CHECK(rig.settle());
    const auto refused = rig.stage.all("voice.refused"), starts = rig.stage.all("voice.start");
    REQUIRE(refused.size() == 1 && starts.size() == 1);
    CHECK(starts[0].t - refused[0].t <= rig.dt + 1e-6);   // retried on the next frame
    CHECK(rig.started("event.your_move"));
    CHECK(rig.stage.all("subtitle").empty());               // heard: no subtitle (Automatic, English)

    DirectorConfig cfg;
    cfg.subtitles = 2;   // Off: the line shown without its voice is shown all the same
    Rig rig2(true, cfg);
    rig2.stage.refuseStarts = 1000;
    rig2.dir.play({say("event.your_move"), say("event.take_time")});
    CHECK(rig2.until([&] { return !rig2.stage.all("subtitle").empty(); }, 5.0f));
    const auto refused2 = rig2.stage.all("voice.refused");
    REQUIRE(refused2.size() >= 2);
    CHECK(refused2.back().t - refused2.front().t >= 0.9);   // kRefusedVoice: about a second of retries
    CHECK_EQ(rig2.voiceStarts(), 0);
    rig2.stage.refuseStarts = 0;   // the device is back for the next line
    CHECK(rig2.settle());
    const auto subs = rig2.stage.all("subtitle");
    REQUIRE(subs.size() == 1);
    CHECK(std::fabs(subs[0].a - rig2.stage.readingTime(subs[0].text)) < 1e-6f);
    CHECK(!rig2.started("event.your_move"));
    CHECK(rig2.started("event.take_time"));
    CHECK_EQ(rig2.voiceStarts(), 1);
}

TEST(coach_director_takeback_offer) {
    Rig rig;
    rig.dir.play({say("event.your_move"), offer(), say("event.play_on")});
    CHECK(rig.until([&] { return rig.dir.offerOpen(); }, 20.0f));
    CHECK(rig.started("ex.offer.b1"));
    const auto shown = rig.stage.all("offer");
    CHECK(shown.size() == 1 && shown[0].flag);
    CHECK(!rig.dir.skippable());
    CHECK(!rig.dir.speaking());
    rig.run(5.0f);
    CHECK(rig.dir.offerOpen());
    CHECK(!rig.started("event.play_on"));
    rig.dir.closeOffer();
    CHECK(!rig.dir.offerOpen());
    CHECK(!rig.stage.all("offer").back().flag);
    CHECK(rig.settle());
    CHECK(rig.started("event.play_on"));

    // Space on the explanation keeps the offer (card only); Space on its line shows the card.
    Rig rig2;
    rig2.dir.play({say("lesson.welcome.hello"), offer()});
    CHECK(rig2.until([&] { return rig2.voiceStarts() > 0; }, 5.0f));
    rig2.dir.skip();
    CHECK(rig2.until([&] { return rig2.dir.offerOpen(); }, 5.0f));
    CHECK_EQ(rig2.voiceStarts(), 1);
    Rig rig3;
    rig3.dir.play({offer()});
    CHECK(rig3.until([&] { return rig3.voiceStarts() > 0; }, 5.0f));
    rig3.step();
    rig3.dir.skip();
    rig3.step();
    CHECK(rig3.dir.offerOpen());
    CHECK_EQ(rig3.stage.count("voice.stop"), 1);
}

TEST(coach_director_wait_move) {
    Rig rig;
    rig.dir.play({say("event.your_move"), waitMove(3), say("event.play_on")});
    int expect = -1;
    CHECK(rig.until([&] { return rig.dir.waitingMove(&expect); }, 10.0f));
    CHECK_EQ(expect, 3);
    CHECK(!rig.dir.idle());
    CHECK(!rig.dir.busy());
    rig.run(3.0f);
    CHECK(!rig.started("event.play_on"));
    // Lines played meanwhile are said on top of the wait.
    rig.dir.play({say("event.take_time", Priority::Low)});
    CHECK(rig.until([&] { return rig.started("event.take_time"); }, 5.0f));
    CHECK(rig.dir.busy());
    CHECK(rig.dir.waitingMove());
    CHECK(rig.until([&] { return !rig.dir.busy(); }, 10.0f));
    // The move: its reaction first, then the beats behind the wait.
    rig.dir.playNext({say("event.takeback.taken")});
    rig.dir.endWait();
    CHECK(!rig.dir.waitingMove());
    CHECK(rig.settle());
    const auto starts = rig.stage.all("voice.start");
    CHECK(starts.size() == 4 && Rig::isKey(starts[2].text, "event.takeback.taken") &&
          Rig::isKey(starts[3].text, "event.play_on"));

    // A move before the wait is reached: the lines before it are dropped.
    Rig rig2;
    rig2.dir.play({say("lesson.welcome.hello"), say("event.your_move"), waitMove(7), say("event.play_on")});
    CHECK(rig2.until([&] { return rig2.voiceStarts() > 0; }, 5.0f));
    CHECK(rig2.dir.jumpToWait());
    CHECK(rig2.dir.waitingMove(&expect) && expect == 7);
    CHECK_EQ(rig2.stage.count("voice.stop"), 1);
    rig2.run(3.0f);
    CHECK(!rig2.started("event.your_move"));
    // Not across a table action.
    Rig rig3;
    Beat set;
    set.kind = BeatKind::SetPosition;
    set.fen = "4k3/8/8/8/8/8/8/4K3 w - - 0 1";
    set.skippable = false;
    rig3.dir.play({say("event.your_move"), set, waitMove(1)});
    CHECK(rig3.until([&] { return rig3.voiceStarts() > 0; }, 5.0f));
    CHECK(!rig3.dir.jumpToWait());
}

TEST(coach_director_clear_puts_the_demonstration_back) {
    Rig rig;
    const uint64_t before = rig.dir.lastScript();
    rig.dir.play({demo("e2e4"), demo("e7e5"), pause(10.0f), rewind(2)});
    const uint64_t id = rig.dir.lastScript();
    CHECK(id != before);
    CHECK(rig.dir.pending(id));
    CHECK(rig.until([&] { return rig.stage.count("demoMove") == 2 && !rig.stage.tableBusy(); }, 20.0f));
    rig.run(0.5f);
    rig.dir.clear();
    CHECK(!rig.dir.pending(id));
    CHECK(rig.dir.idle());
    const auto rw = rig.stage.all("rewindDemo");
    CHECK(rw.size() == 1 && rw[0].n == 2 && rw[0].flag);
}
