#include "events.h"
#include "lesson.h"
#include <algorithm>

namespace coach {
namespace {

Gesture gesture(GestureKind k) {
    Gesture g;
    g.kind = k;
    g.at = 0.3f;
    return g;
}

Beat sayBeat(const std::string& key, std::vector<Gesture> g = {}, Look look = Look::Player,
             Priority priority = Priority::Normal, int ply = -1) {
    Beat b;
    b.kind = BeatKind::Say;
    b.line.key = key;
    b.gestures = std::move(g);
    b.look = look;
    b.priority = priority;
    b.ply = ply;
    return b;
}

Script one(Beat b) { return Script{std::move(b)}; }

Line chapterTitle(const Lesson& lesson, int chapter) {
    Line l;
    if (chapter >= 0 && size_t(chapter) < lesson.chapters().size()) l = lesson.chapters()[size_t(chapter)].title;
    return l;
}

Script lessonLine(const char* key, const Lesson& lesson, int chapter) {
    Script s;
    Line title = chapterTitle(lesson, chapter);
    if (title.empty()) return s;
    Beat b = sayBeat(key, {gesture(GestureKind::Open)});
    b.line.with("chapter", Arg::ofText(title.key));
    s.push_back(b);
    return s;
}

}  // namespace

Script greetingScript(int level, chess::Color human, bool introduceLevel) {
    level = std::max(1, std::min(6, level));
    Script s;
    s.push_back(sayBeat("event.greet.l" + std::to_string(level), {gesture(GestureKind::Nod)}));
    if (introduceLevel) {
        Script intro = levelIntroScript(level);
        s.insert(s.end(), intro.begin(), intro.end());
    }
    s.push_back(sayBeat(human == chess::White ? "event.colour.white" : "event.colour.black", {gesture(GestureKind::Open)}));
    return s;
}

Script levelIntroScript(int level) {
    level = std::max(1, std::min(6, level));
    return one(sayBeat("event.level.l" + std::to_string(level)));
}

Script yourMoveScript(int ply) { return one(sayBeat("event.your_move", {gesture(GestureKind::Open)}, Look::Player, Priority::Normal, ply)); }

Script takeYourTimeScript(int ply) { return one(sayBeat("event.take_time", {}, Look::Player, Priority::Low, ply)); }

Script fillerScript(int ply) { return one(sayBeat("event.filler", {}, Look::Board, Priority::Low, ply)); }

Script takebackScript(bool taken, int ply) {
    return one(sayBeat(taken ? "event.takeback.taken" : "event.takeback.declined", {gesture(GestureKind::Nod)},
                       Look::Player, Priority::Normal, ply));
}

Script playOnScript(int ply) { return one(sayBeat("event.play_on", {gesture(GestureKind::Open)}, Look::Player, Priority::Normal, ply)); }

Script encouragementScript(Encouragement kind, int ply) {
    const char* key = kind == Encouragement::AfterMistake ? "event.encourage.mistake"
                      : kind == Encouragement::Behind     ? "event.encourage.behind"
                                                          : "event.encourage.well";
    GestureKind g = kind == Encouragement::PlayingWell ? GestureKind::Nod : GestureKind::Open;
    return one(sayBeat(key, {gesture(g)}, Look::Player, Priority::Normal, ply));
}

Script drawAnswerScript(bool accepted) {
    return one(sayBeat(accepted ? "event.draw.accepted" : "event.draw.declined",
                       {gesture(accepted ? GestureKind::Nod : GestureKind::ShakeHead)}));
}

Script gameEndScript(GameEnd end) {
    const char* key = end == GameEnd::Win ? "event.end.win"
                      : end == GameEnd::Loss ? "event.end.loss"
                      : end == GameEnd::Draw ? "event.end.draw"
                                             : "event.end.resigned";
    Script s;
    s.push_back(sayBeat(key, {gesture(GestureKind::Nod)}));
    Beat hands = sayBeat("event.end.handshake", {gesture(GestureKind::Open)});
    hands.skippable = false;   // the handshake follows it
    s.push_back(hands);
    return s;
}

Script lessonResumeScript(const Lesson& lesson, int chapter) { return lessonLine("event.lesson.resume", lesson, chapter); }

Script lessonNextScript(const Lesson& lesson, int chapter) { return lessonLine("event.lesson.next", lesson, chapter); }

std::vector<std::string> eventKeys() {
    std::vector<std::string> k;
    for (int l = 1; l <= 6; ++l) k.push_back("event.greet.l" + std::to_string(l));
    for (int l = 1; l <= 6; ++l) k.push_back("event.level.l" + std::to_string(l));
    for (const char* s : {"event.colour.white", "event.colour.black", "event.your_move", "event.take_time", "event.filler",
                          "event.takeback.taken", "event.takeback.declined", "event.play_on", "event.encourage.mistake",
                          "event.encourage.behind", "event.encourage.well", "event.draw.accepted", "event.draw.declined",
                          "event.end.win", "event.end.loss", "event.end.draw", "event.end.resigned", "event.end.handshake",
                          "event.lesson.resume", "event.lesson.next"})
        k.push_back(s);
    return k;
}

}  // namespace coach
