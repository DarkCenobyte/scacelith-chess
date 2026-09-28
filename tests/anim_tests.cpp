// Animation tests (no GPU): the writing-hand helpers, the writing queue's event instants and pen
// tip, writing never delaying the playing hand, and the left-handed player as the exact mirror
// image of a right-handed one. The animator is not part of scacelith_core, so its sources are
// compiled into this file.
#include "test.h"
#include "../src/anim/animator.cpp"
#include "../src/anim/animator_writing.cpp"
#include "../src/character/skeleton.cpp"

namespace {

using anim::PenKey;
using anim::WriteTask;
using anim::WriteTaskType;

// A few strokes on White's scoresheet (row 1, as the anim viewer writes): hover, three down
// segments separated by lifts, a final lift. Keys every 1/60 s.
std::vector<PenKey> testPath(float xMirror = 1.0f) {
    const float top = layout::TABLE_TOP_Y + layout::SCORESHEET_THICKNESS;
    const m::vec3 base(-layout::SCORESHEET_X - layout::SCORESHEET_WIDTH * 0.5f + 0.017f, top,
                       layout::SCORESHEET_Z - layout::SCORESHEET_LENGTH * 0.5f + 0.032f + 0.007f);
    std::vector<PenKey> k;
    float t = 0.0f;
    auto add = [&](m::vec3 p, bool down) {
        k.push_back({t, m::vec3(p.x * xMirror, p.y, p.z), down});
        t += 1.0f / 60.0f;
    };
    add(base + m::vec3(0, 0.0025f, 0), false);
    for (int s = 0; s < 3; ++s) {
        m::vec3 o = base + m::vec3(0.008f * float(s), 0, 0);
        for (int i = 0; i <= 8; ++i) {
            float u = float(i) / 8.0f;
            add(o + m::vec3(0.004f * u, 0, -0.0045f * std::sin(3.1416f * u)), i < 8);
        }
        add(o + m::vec3(0.006f, 0.002f, 0), false);
    }
    add(base + m::vec3(0.03f, 0.003f, 0), false);
    return k;
}

// The pen lying beside White's pad (outer edge, parallel to Z, tip towards the board).
m::mat4 penFrame(float xMirror = 1.0f) {
    m::vec3 tip(xMirror * -(layout::SCORESHEET_X + layout::SCORESHEET_WIDTH * 0.5f + 0.030f), layout::TABLE_TOP_Y + layout::PEN_RADIUS,
                layout::SCORESHEET_Z - layout::PEN_LENGTH * 0.5f);
    return m::translate(tip) * m::toMat4(m::fromTo(m::vec3(0, 1, 0), m::vec3(0, 0, 1)), m::vec3(0));
}

// Outer corner of the bottom edge of White's page, turning over the top edge (towards the board).
m::vec3 pageCorner(float s, float xMirror = 1.0f) {
    const float L = layout::SCORESHEET_LENGTH, a = 3.1416f * s;
    const float top = layout::TABLE_TOP_Y + layout::SCORESHEET_THICKNESS;
    const float bindZ = layout::SCORESHEET_Z - L * 0.5f;
    return m::vec3(xMirror * -(layout::SCORESHEET_X + layout::SCORESHEET_WIDTH * 0.5f), top + L * std::sin(a) + 0.0006f, bindZ + L * std::cos(a));
}

void initWhite(anim::Animator& an) {
    an.init(character::robotSkeleton(), m::vec3(0, layout::PLAYER_PELVIS_Y, layout::PLAYER_PELVIS_Z), 1.0f);
    an.setRestHand(m::vec3(0.24f, layout::TABLE_TOP_Y, 0.34f));
}

anim::Task pressClock(float xMirror, float zSign) {
    anim::Task t;
    t.type = anim::TaskType::PressClock;
    t.position = m::vec3(xMirror * layout::CLOCK_OFFSET_X, layout::TABLE_TOP_Y + layout::CLOCK_HEIGHT + 0.005f, zSign * 0.045f);
    return t;
}

}  // namespace

TEST(anim_page_turn_ease) {
    CHECK(anim::pageTurnEase(0.0f) == 0.0f);
    CHECK(anim::pageTurnEase(0.33f) == 0.0f);
    CHECK(std::fabs(anim::pageTurnEase(0.70f) - 0.60f) < 1e-5f);
    CHECK(anim::pageTurnEase(0.90f) == 1.0f);
    CHECK(anim::pageTurnEase(1.0f) == 1.0f);
    float prev = 0.0f, maxStep = 0.0f;
    bool monotone = true;
    for (int i = 0; i <= 2000; ++i) {
        float s = anim::pageTurnEase(float(i) / 2000.0f);
        if (s < prev - 1e-6f) monotone = false;
        maxStep = std::max(maxStep, s - prev);
        prev = s;
    }
    CHECK(monotone);
    CHECK(maxStep < 0.005f);   // continuous
}

TEST(anim_pen_path_curve) {
    const std::vector<PenKey> k = testPath();
    for (const PenKey& key : k) CHECK(m::length(anim::penPathPoint(k, key.t) - key.tip) < 1e-6f);
    CHECK(m::length(anim::penPathPoint(k, -1.0f) - k.front().tip) < 1e-6f);
    CHECK(m::length(anim::penPathPoint(k, 99.0f) - k.back().tip) < 1e-6f);
    bool flags = true, above = true, onPaper = true;
    for (size_t i = 0; i + 1 < k.size(); ++i)
        for (int j = 1; j < 10; ++j) {
            float t = k[i].t + (k[i + 1].t - k[i].t) * float(j) / 10.0f;
            if (anim::penPathDown(k, t) != k[i].down) flags = false;
            float y = anim::penPathPoint(k, t).y;
            if (y < std::min(k[i].tip.y, k[i + 1].tip.y) - 1e-6f) above = false;   // never dips into the paper
            if (k[i].down && k[i + 1].down && std::fabs(y - k[i].tip.y) > 1e-6f) onPaper = false;
        }
    CHECK(flags);
    CHECK(above);
    CHECK(onPaper);
}

TEST(anim_write_task_durations) {
    WriteTask w;
    w.type = WriteTaskType::Write;
    w.path = testPath();
    CHECK(std::fabs(anim::writeTaskDuration(w) - (anim::Timing::WriteApproach + w.path.back().t + anim::Timing::WriteRetract)) < 1e-6f);
    w.type = WriteTaskType::TurnPage;
    CHECK(anim::writeTaskDuration(w) == anim::Timing::PageTurn);
    w.duration = 2.0f;
    CHECK(anim::writeTaskDuration(w) == 2.0f);
    w.type = WriteTaskType::PickPen;
    CHECK(anim::writeTaskDuration(w) == anim::Timing::PickPen);
    w.type = WriteTaskType::PutPen;
    CHECK(anim::writeTaskDuration(w) == anim::Timing::PutPen);
}

// The whole writing sequence on a right-handed White: event instants, the tip exactly on the path,
// the page turning, and a clock press of the playing hand at the same instant as without writing.
TEST(anim_writing_sequence) {
    using anim::EventType;
    anim::Animator an, ref;
    initWhite(an);
    initWhite(ref);
    const std::vector<PenKey> path = testPath();
    std::vector<WriteTask> w(4);
    w[0].type = WriteTaskType::PickPen;
    w[0].frame = penFrame();
    w[1].type = WriteTaskType::Write;
    w[1].path = path;
    w[2].type = WriteTaskType::TurnPage;
    w[2].pageCorner = [](float s) { return pageCorner(s); };
    w[3].type = WriteTaskType::PutPen;
    w[3].frame = penFrame() * m::translate(m::vec3(0, 0.004f, 0));
    an.enqueueWriting(w);
    CHECK(an.writingBusy());
    CHECK(std::fabs(an.writingRemainingTime() - (anim::Timing::PickPen + anim::writeTaskDuration(w[1]) + anim::Timing::PageTurn + anim::Timing::PutPen)) < 1e-5f);
    for (anim::Animator* a : {&an, &ref}) {
        anim::Task wait;
        wait.type = anim::TaskType::Wait;
        wait.duration = 0.9f;
        a->enqueue(wait);
        a->enqueue(pressClock(1.0f, 1.0f));
    }

    const float tWrite = anim::Timing::PickPen, tPath = tWrite + anim::Timing::WriteApproach;
    const float tTurn = tWrite + anim::writeTaskDuration(w[1]), tPut = tTurn + anim::Timing::PageTurn;
    std::vector<float> downs, ups;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        if (path[i].down && (i == 0 || !path[i - 1].down)) downs.push_back(tPath + path[i].t);
        if (!path[i].down && i > 0 && path[i - 1].down) ups.push_back(tPath + path[i].t);
    }
    float picked = -1, put = -1, done = -1, gripped = -1, turned = -1, empty = -1, clock = -1, clockRef = -1;
    size_t nDown = 0, nUp = 0;
    float timingErr = 0.0f, tipErr = 0.0f, lastS = -1.0f;
    bool sBack = false, heldOk = true;
    const float dt = 1.0f / 120.0f;
    std::vector<anim::Event> ev;
    for (int step = 0; step < int(6.0f / dt); ++step) {
        ev.clear();
        an.update(dt, ev);
        for (const anim::Event& e : ev) {
            switch (e.type) {
                case EventType::PenPicked: picked = e.time; break;
                case EventType::PenPut: put = e.time; break;
                case EventType::WritingDone: done = e.time; break;
                case EventType::PageGripped: gripped = e.time; break;
                case EventType::PageTurned: turned = e.time; break;
                case EventType::WritingQueueEmpty: empty = e.time; break;
                case EventType::ClockPressed: clock = e.time; break;
                case EventType::PenDown:
                    if (nDown < downs.size()) timingErr = std::max(timingErr, std::fabs(e.time - downs[nDown]));
                    ++nDown;
                    break;
                case EventType::PenUp:
                    if (nUp < ups.size()) timingErr = std::max(timingErr, std::fabs(e.time - ups[nUp]));
                    ++nUp;
                    break;
                default: break;
            }
        }
        ev.clear();
        ref.update(dt, ev);
        for (const anim::Event& e : ev)
            if (e.type == EventType::ClockPressed) clockRef = e.time;
        // The rendered tip is the path point at the reported path time.
        const float tp = an.writingPathTime();
        m::mat4 px;
        if (tp >= 0.0f) {
            CHECK(an.penTransform(px));
            tipErr = std::max(tipErr, m::length(px.translation() - anim::penPathPoint(path, tp)));
        }
        const float s = an.pageTurnProgress();
        if (s >= 0.0f) {
            if (s < lastS - 1e-6f) sBack = true;
            lastS = s;
        }
        const float t = an.time();
        if (an.holdsPen() != (picked >= 0.0f && picked <= t && (put < 0.0f || t < put))) heldOk = false;
    }
    CHECK(std::fabs(picked - 0.36f) < 1e-4f);
    CHECK(std::fabs(done - (tPath + path.back().t)) < 1e-4f);
    CHECK(std::fabs(gripped - (tTurn + 0.33f * anim::Timing::PageTurn)) < 1e-4f);
    CHECK(std::fabs(turned - (tTurn + 0.90f * anim::Timing::PageTurn)) < 1e-4f);
    CHECK(std::fabs(put - (tPut + 0.34f)) < 1e-4f);
    CHECK(std::fabs(empty - (tPut + anim::Timing::PutPen)) < 1e-4f);
    CHECK_EQ(nDown, downs.size());
    CHECK_EQ(nUp, ups.size());
    CHECK(timingErr < 1e-4f);
    CHECK(tipErr < 2e-4f);
    CHECK(!sBack);
    CHECK(heldOk);
    CHECK(!an.writingBusy());
    CHECK(!an.holdsPen());
    // Writing never delays the playing hand.
    CHECK(clock > 0.0f);
    CHECK(std::fabs(clock - clockRef) < 1e-6f);
}

// Black left-handed (clock at +X) against Black right-handed in the mirrored world: the same motion
// bone for bone, the same events at the same instants, the pen mirrored.
TEST(anim_left_handed_mirror) {
    using namespace character;
    const character::Skeleton& sk = robotSkeleton();
    anim::Animator L, R;
    const m::vec3 pelvis(0, layout::PLAYER_PELVIS_Y, -layout::PLAYER_PELVIS_Z);
    L.init(sk, pelvis, -1.0f, Side::Left);
    R.init(sk, pelvis, -1.0f, Side::Right);
    CHECK(L.playHand() == Side::Left);
    CHECK(L.writingHand() == Side::Right);
    CHECK(R.writingHand() == Side::Left);
    L.setRestHand(m::vec3(0.24f, layout::TABLE_TOP_Y, -0.34f));
    R.setRestHand(m::vec3(-0.24f, layout::TABLE_TOP_Y, -0.34f));
    const m::mat4 S = m::scale(m::vec3(-1, 1, 1));
    for (int k = 0; k < 2; ++k) {
        anim::Animator& a = k == 0 ? L : R;
        // L: clock at +X, Black's pad at -X (its right). R: all of it mirrored about X = 0. Black's
        // pad and pen are White's of the mirrored layout turned by 180 degrees.
        const float mx = k == 0 ? 1.0f : -1.0f;
        std::vector<WriteTask> w(3);
        w[0].type = WriteTaskType::PickPen;
        w[0].frame = m::rotateY(3.1415927f) * penFrame(-mx);
        w[1].type = WriteTaskType::Write;
        w[1].path = testPath(-mx);
        for (PenKey& key : w[1].path) key.tip = m::vec3(-key.tip.x, key.tip.y, -key.tip.z);
        w[2].type = WriteTaskType::PutPen;
        w[2].frame = w[0].frame;
        a.enqueueWriting(w);
        anim::Task wait;
        wait.type = anim::TaskType::Wait;
        wait.duration = 0.5f;
        a.enqueue(wait);
        a.enqueue(pressClock(mx, -1.0f));
        anim::Task back;
        back.type = anim::TaskType::Retract;
        a.enqueue(back);
    }
    auto mirrorBone = [](int b) {
        if (b >= ClavicleL && b <= PinkyL3) return b + (ClavicleR - ClavicleL);
        if (b >= ClavicleR && b <= PinkyR3) return b - (ClavicleR - ClavicleL);
        if (b >= ThighL && b <= FootL) return b + (ThighR - ThighL);
        if (b >= ThighR && b <= FootR) return b - (ThighR - ThighL);
        if (b == EyeL) return int(EyeR);
        if (b == EyeR) return int(EyeL);
        if (b == LidUpperL) return int(LidUpperR);
        if (b == LidUpperR) return int(LidUpperL);
        if (b == LidLowerL) return int(LidLowerR);
        if (b == LidLowerR) return int(LidLowerL);
        return b;
    };
    float worst = 0.0f, penDiff = 0.0f;
    int evBad = 0, nEvents = 0;
    std::vector<anim::Event> evL, evR;
    const float dt = 1.0f / 120.0f;
    for (int step = 0; step < int(3.5f / dt); ++step) {
        evL.clear();
        evR.clear();
        L.update(dt, evL);
        R.update(dt, evR);
        nEvents += int(evL.size());
        if (evL.size() != evR.size()) ++evBad;
        for (size_t i = 0; i < std::min(evL.size(), evR.size()); ++i) {
            m::vec3 pr = evR[i].position;
            if (evL[i].type != evR[i].type || std::fabs(evL[i].time - evR[i].time) > 1e-6f || m::length(evL[i].position - m::vec3(-pr.x, pr.y, pr.z)) > 1e-4f) ++evBad;
        }
        for (int b = 0; b < BoneCount; ++b) {
            const m::mat4 want = S * R.globals()[mirrorBone(b)] * S;
            worst = std::max(worst, m::length(L.globals()[b].translation() - want.translation()));
        }
        m::mat4 pl, pr;
        const bool hl = L.penTransform(pl), hr = R.penTransform(pr);
        if (hl != hr) ++evBad;
        if (hl && hr) penDiff = std::max(penDiff, m::length(pl.translation() - (S * pr).translation()));
    }
    CHECK(nEvents >= 6);
    CHECK_EQ(evBad, 0);
    CHECK(worst < 1e-4f);
    CHECK(penDiff < 1e-4f);
}
