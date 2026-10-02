// Animation tests (no GPU): the writing-hand helpers, the writing queue's event instants and pen
// tip, writing never delaying the playing hand, the left-handed player as the exact mirror image
// of a right-handed one, the first-person player writing on his own scoresheet as the game
// wires it (pen, ink, PenDown sounds heard from his head), and the coach's gestures (pointing,
// tracing a move, speaking gestures, their timing, and a demonstration taken back). The animator
// is not part of scacelith_core, so its sources are compiled into this file.
#include "test.h"
#include "../src/anim/animator.cpp"
#include "../src/anim/animator_gesture.cpp"
#include "../src/anim/animator_writing.cpp"
#include "../src/character/skeleton.cpp"
#include "audio/mixer.h"
#include "audio/synth.h"
#include "game/scoresheet_layout.h"

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

// A Write with an empty path (nothing to write): the pen tip stays near the writing rest, and
// WritingDone still fires on time, there.
TEST(anim_write_empty_path_stays_at_the_rest) {
    anim::Animator an;
    initWhite(an);
    const m::vec3 rest(-layout::SCORESHEET_X, layout::TABLE_TOP_Y + layout::SCORESHEET_THICKNESS, layout::SCORESHEET_Z);
    an.setWritingRest(rest);
    std::vector<WriteTask> w(2);
    w[0].type = WriteTaskType::PickPen;
    w[0].frame = penFrame();
    w[1].type = WriteTaskType::Write;
    an.enqueueWriting(w);
    const float tWrite = anim::Timing::PickPen, tEnd = tWrite + anim::writeTaskDuration(w[1]);
    const float dt = 1.0f / 120.0f;
    std::vector<anim::Event> ev;
    float done = -1.0f, far = 0.0f;
    m::vec3 donePos(0.0f);
    while (an.time() < tEnd + 0.1f) {
        ev.clear();
        an.update(dt, ev);
        for (const anim::Event& e : ev)
            if (e.type == anim::EventType::WritingDone) {
                done = e.time;
                donePos = e.position;
            }
        m::mat4 px;
        if (an.time() > tWrite && an.time() < tEnd && an.penTransform(px)) far = std::max(far, m::length(px.translation() - rest));
    }
    std::fprintf(stderr, "  empty Write: tip up to %.1f mm from the rest, WritingDone %.1f mm from it\n", far * 1000.0f,
                 m::length(donePos - rest) * 1000.0f);
    CHECK(std::fabs(done - (tWrite + anim::Timing::WriteApproach)) < 1e-4f);
    CHECK(m::length(donePos - rest) < 0.006f);
    CHECK(far < 0.03f);
    CHECK(!an.writingBusy());
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

// A seat off the X = 0 plane: the left-handed player is still the mirror image of a right-handed
// one seated at the mirrored spot, and sets the piece down where it was asked to.
TEST(anim_left_handed_seat_off_centre) {
    using namespace character;
    const character::Skeleton& sk = robotSkeleton();
    anim::Animator L, R;
    L.init(sk, m::vec3(0.05f, layout::PLAYER_PELVIS_Y, -layout::PLAYER_PELVIS_Z), -1.0f, Side::Left);
    R.init(sk, m::vec3(-0.05f, layout::PLAYER_PELVIS_Y, -layout::PLAYER_PELVIS_Z), -1.0f, Side::Right);
    L.setRestHand(m::vec3(0.29f, layout::TABLE_TOP_Y, -0.34f));
    R.setRestHand(m::vec3(-0.29f, layout::TABLE_TOP_Y, -0.34f));
    // L: a pawn from f7 to f5 (on its playing hand's side); R: the mirror image.
    const m::vec3 from = layout::squareCenter(5, 6), to = layout::squareCenter(5, 4);
    auto mirror = [](m::vec3 p) { return m::vec3(-p.x, p.y, p.z); };
    for (int k = 0; k < 2; ++k) {
        anim::Animator& a = k == 0 ? L : R;
        const m::vec3 f = k == 0 ? from : mirror(from), t = k == 0 ? to : mirror(to);
        a.pieceTransform = [f](int id) { return id == 0 ? m::translate(f) : m::mat4(); };
        a.pieceGripInfo = [](int) { return m::vec3(layout::PIECE_HEIGHT[1], layout::PIECE_GRIP_HEIGHT[1], layout::PIECE_GRIP_RADIUS[1]); };
        std::vector<anim::Task> ts(5);
        ts[0].type = anim::TaskType::Reach;
        ts[0].pieceId = 0;
        ts[1].type = anim::TaskType::Lift;
        ts[2].type = anim::TaskType::Carry;
        ts[2].position = t;
        ts[3].type = anim::TaskType::Place;
        ts[3].position = t;
        ts[4].type = anim::TaskType::Retract;
        a.enqueue(ts);
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
    const m::mat4 S = m::scale(m::vec3(-1, 1, 1));
    float worst = 0.0f, placeErr = -1.0f;
    int evBad = 0, released = 0;
    m::mat4 held;
    std::vector<anim::Event> evL, evR;
    const float dt = 1.0f / 120.0f;
    for (int step = 0; step < int(1.6f / dt); ++step) {
        evL.clear();
        evR.clear();
        L.update(dt, evL);
        R.update(dt, evR);
        if (evL.size() != evR.size()) ++evBad;
        for (size_t i = 0; i < std::min(evL.size(), evR.size()); ++i) {
            if (evL[i].type != evR[i].type || m::length(evL[i].position - mirror(evR[i].position)) > 1e-4f) ++evBad;
            if (evL[i].type == anim::EventType::PieceReleased) {
                ++released;
                placeErr = m::length(held.translation() - to);   // where the hand held it the frame before
            }
        }
        L.heldPieceTransform(0, held);
        for (int b = 0; b < BoneCount; ++b) {
            const m::mat4 want = S * R.globals()[mirrorBone(b)] * S;
            worst = std::max(worst, m::length(L.globals()[b].translation() - want.translation()));
        }
    }
    CHECK_EQ(released, 1);
    CHECK_EQ(evBad, 0);
    CHECK(worst < 1e-4f);
    CHECK(placeErr >= 0.0f && placeErr < 0.004f);
}

// A left-handed player's handshake needs its writing hand: it cuts a page turn short, even while
// the hand follows the page corner (the page is reported turned when the handshake starts).
TEST(anim_left_handed_handshake_cuts_page_turn) {
    const character::Skeleton& sk = character::robotSkeleton();
    anim::Animator B, W;
    W.init(sk, m::vec3(0, layout::PLAYER_PELVIS_Y, layout::PLAYER_PELVIS_Z), 1.0f);
    B.init(sk, m::vec3(0, layout::PLAYER_PELVIS_Y, -layout::PLAYER_PELVIS_Z), -1.0f, character::Side::Left);
    WriteTask turn;
    turn.type = WriteTaskType::TurnPage;   // the default page geometry
    B.enqueueWriting(turn);
    std::vector<anim::Event> ev;
    const float dt = 1.0f / 120.0f;
    float start = -1.0f, turned = -1.0f, clasp = -1.0f;
    while (B.time() < 4.0f) {
        if (start < 0.0f && B.time() >= 0.6f) {   // the hand is on the corner, lifting the page
            CHECK(B.pageTurnProgress() > 0.0f);
            anim::Task h;
            h.type = anim::TaskType::Handshake;
            h.partner = &W;
            B.enqueue(h);
            h.partner = &B;
            W.enqueue(h);
            start = B.time();
        }
        ev.clear();
        W.update(dt, ev);
        ev.clear();
        B.update(dt, ev);
        for (const anim::Event& e : ev) {
            if (e.type == anim::EventType::PageTurned) turned = e.time;
            if (e.type == anim::EventType::HandshakeClasp) clasp = e.time;
        }
    }
    CHECK(start > 0.0f);
    CHECK(std::fabs(turned - start) < 1e-4f);
    CHECK(std::fabs(clasp - (start + anim::Timing::HandshakeClaspAt)) < 1e-4f);
}

// A handshake starting in the middle of a frame, just after the writing hand laid its pen down:
// PenPut fires once, at its own instant and where the PutPen put it (the handshake has no pen to
// lay down).
TEST(anim_left_handed_handshake_after_pen_put_mid_frame) {
    const character::Skeleton& sk = character::robotSkeleton();
    anim::Animator B, W;
    W.init(sk, m::vec3(0, layout::PLAYER_PELVIS_Y, layout::PLAYER_PELVIS_Z), 1.0f);
    B.init(sk, m::vec3(0, layout::PLAYER_PELVIS_Y, -layout::PLAYER_PELVIS_Z), -1.0f, character::Side::Left);
    const m::mat4 taken = m::rotateY(3.1415927f) * penFrame(-1.0f);   // beside Black's pad
    const m::mat4 laid = m::translate(m::vec3(0, 0, -0.03f)) * taken;
    WriteTask pick, put;
    pick.type = WriteTaskType::PickPen;
    pick.frame = taken;
    put.type = WriteTaskType::PutPen;
    put.frame = laid;
    B.enqueueWriting(pick);
    B.enqueueWriting(put);
    const float putAt = anim::Timing::PickPen + 0.34f;   // PenPut, 0.34 s into the PutPen
    anim::Task h;
    h.type = anim::TaskType::Handshake;
    h.notBefore = putAt + 0.005f;                        // within the same 1/60 s frame
    h.partner = &W;
    B.enqueue(h);
    h.partner = &B;
    W.enqueue(h);
    std::vector<anim::Event> ev;
    const float dt = 1.0f / 60.0f;
    int puts = 0;
    float putTime = -1.0f;
    m::vec3 putPos(0.0f);
    while (B.time() < 3.5f) {
        ev.clear();
        W.update(dt, ev);
        ev.clear();
        B.update(dt, ev);
        for (const anim::Event& e : ev)
            if (e.type == anim::EventType::PenPut) {
                ++puts;
                putTime = e.time;
                putPos = e.position;
            }
    }
    CHECK_EQ(puts, 1);
    CHECK(std::fabs(putTime - putAt) < 1e-4f);
    CHECK(m::length(putPos - laid.translation()) < 1e-5f);
}

// Tasks cut short (cancelTasks: the online opponent's move comes while its robot still plays their
// live gestures): the piece in hand is let go without its release, and the next task starts at once
// from where the hand is, with its usual duration.
TEST(anim_cancel_tasks_lets_go_and_goes_on_from_the_hand) {
    using anim::EventType;
    using anim::TaskType;
    anim::Animator an;
    initWhite(an);
    const m::vec3 d2 = layout::squareCenter(11), e2 = layout::squareCenter(12), e4 = layout::squareCenter(28);
    // Two pawns, ids 0 (e2) and 1 (d2); no other piece.
    an.pieceTransform = [&](int id) { return id == 0 ? m::translate(e2) : id == 1 ? m::translate(d2) : m::mat4(); };
    an.pieceGripInfo = [](int) { return m::vec3(layout::PIECE_HEIGHT[1], layout::PIECE_GRIP_HEIGHT[1], layout::PIECE_GRIP_RADIUS[1]); };
    auto task = [](TaskType type, int id, m::vec3 pos = m::vec3(0)) {
        anim::Task t;
        t.type = type;
        t.pieceId = id;
        t.position = pos;
        return t;
    };
    an.enqueue({task(TaskType::Reach, 0), task(TaskType::Lift, 0), task(TaskType::Carry, 0, e4), task(TaskType::Place, 0, e4),
                task(TaskType::Retract, -1)});
    const float dt = 1.0f / 120.0f;
    std::vector<anim::Event> ev;
    while (an.time() < anim::Timing::Reach + anim::Timing::Lift + 0.1f) an.update(dt, ev);
    CHECK(an.holding(0));  // halfway to e4
    const m::vec3 hand = an.globals()[character::HandR].translation();
    const float t0 = an.time();
    an.cancelTasks();
    CHECK(!an.holding(0));
    CHECK(!an.busy());
    CHECK(an.remainingTime() == 0.0f);
    an.enqueue({task(TaskType::Reach, 1), task(TaskType::Lift, 1), task(TaskType::Place, 1, d2), task(TaskType::Retract, -1)});
    float gripped = -1.0f, released = -1.0f, empty = -1.0f, jump = -1.0f;
    int cutEvents = 0;
    for (int step = 0; step < int(2.0f / dt); ++step) {
        ev.clear();
        an.update(dt, ev);
        if (jump < 0.0f) jump = m::length(an.globals()[character::HandR].translation() - hand);
        for (const anim::Event& e : ev) {
            if (e.pieceId == 0) ++cutEvents;
            if (e.type == EventType::PieceGripped && e.pieceId == 1) gripped = e.time - t0;
            if (e.type == EventType::PieceReleased && e.pieceId == 1) released = e.time - t0;
            if (e.type == EventType::QueueEmpty) empty = e.time - t0;
        }
    }
    CHECK_EQ(cutEvents, 0);  // no release of the pawn let go
    CHECK(jump >= 0.0f && jump < 0.01f);
    CHECK(std::fabs(gripped - anim::Timing::Reach) < 1e-4f);
    CHECK(std::fabs(released - (anim::Timing::Reach + anim::Timing::Lift + anim::Timing::Place)) < 1e-4f);
    CHECK(std::fabs(empty - (anim::Timing::Reach + anim::Timing::Lift + anim::Timing::Place + anim::Timing::Retract)) < 1e-4f);
    CHECK(!an.holding(1));
    CHECK(!an.busy());
}

// A hand holding a piece does not gesture, Beat included: the task only holds the piece still, its
// beats still fire on time.
TEST(anim_beat_while_holding_only_holds) {
    anim::Animator an;
    initWhite(an);
    const m::vec3 e2 = layout::squareCenter(12);
    an.pieceTransform = [&](int id) { return id == 0 ? m::translate(e2) : m::mat4(); };
    an.pieceGripInfo = [](int) { return m::vec3(layout::PIECE_HEIGHT[1], layout::PIECE_GRIP_HEIGHT[1], layout::PIECE_GRIP_RADIUS[1]); };
    anim::Task reach, beat;
    reach.type = anim::TaskType::Reach;
    reach.pieceId = 0;
    beat.type = anim::TaskType::Gesture;
    beat.shape = anim::HandShape::Beat;
    beat.duration = 0.9f;   // two strokes
    an.enqueue({reach, beat});
    const float dt = 1.0f / 120.0f;
    std::vector<anim::Event> ev;
    std::vector<float> beats;
    m::mat4 held0;
    float moved = 0.0f;
    bool held = true;
    for (int step = 0; step < int((anim::Timing::Reach + 1.0f) / dt); ++step) {
        ev.clear();
        an.update(dt, ev);
        for (const anim::Event& e : ev)
            if (e.type == anim::EventType::GestureBeat) beats.push_back(e.time);
        if (an.time() < anim::Timing::Reach) continue;
        m::mat4 p;
        held = held && an.heldPieceTransform(0, p);
        if (an.time() - anim::Timing::Reach < dt) held0 = p;
        else moved = std::max(moved, m::length(p.translation() - held0.translation()));
    }
    CHECK(held);
    CHECK(moved < 0.002f);
    CHECK_EQ(int(beats.size()), 2);
    for (size_t k = 0; k < beats.size(); ++k) CHECK(std::fabs(beats[k] - (anim::Timing::Reach + (float(k) + 0.6f) * 0.45f)) < 1e-4f);
}

// A long session (the clock near 3 hours, where a float sum of 1/120 s frames runs 5% fast): the
// clock keeps its rate, and a task still lasts its duration in frames.
TEST(anim_clock_keeps_its_rate_in_a_long_session) {
    anim::Animator an;
    initWhite(an);
    std::vector<anim::Event> ev;
    an.update(10000.0f, ev);
    const float t0 = an.time();
    anim::Task wait;
    wait.type = anim::TaskType::Wait;
    wait.duration = 5.0f;
    an.enqueue(wait);
    int frames = 0, doneAt = -1;
    while (frames < 1200) {
        ev.clear();
        an.update(1.0f / 120.0f, ev);
        ++frames;
        for (const anim::Event& e : ev)
            if (e.type == anim::EventType::QueueEmpty && doneAt < 0) doneAt = frames;
    }
    CHECK(t0 == 10000.0f);
    CHECK(doneAt >= 599 && doneAt <= 601);
    CHECK(std::fabs(an.time() - (t0 + 10.0f)) < 1e-3f);
}

// An animator is not copyable (a copy would drive the same character state); the game resets one
// by moving a fresh one in.
static_assert(!std::is_copy_constructible_v<anim::Animator> && !std::is_copy_assignable_v<anim::Animator>);
static_assert(std::is_move_constructible_v<anim::Animator> && std::is_move_assignable_v<anim::Animator>);

// ---- The first-person player's own scoresheet ----------------------------------------------------
namespace {

namespace sh = game::sheet;

// A synthetic handwriting run (as in tests/scoresheet_tests.cpp): 0.5 em per glyph.
sh::Run moveRun(const std::string& text) {
    sh::Run r;
    float pen = 0.0f;
    for (size_t i = 0; i < text.size(); ++i) {
        sh::RunGlyph g;
        g.cp = uint32_t(uint8_t(text[i]));
        g.penX = pen;
        const bool tall = (text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= '0' && text[i] <= '9');
        g.x0 = 0.05f;
        g.x1 = 0.45f;
        g.y0 = tall ? -0.68f : -0.45f;
        g.source = int(i);
        r.glyphs.push_back(g);
        pen += 0.5f;
    }
    r.advance = pen;
    return r;
}

// One move on a pad as game::Scoresheet::beginMove builds it: the path (page mm) and its world keys.
struct SheetEntry {
    sh::PenPath path;
    std::vector<PenKey> keys;
};
SheetEntry sheetEntry(const sh::PadFrame& f, int ply, const std::string& san, uint32_t seed) {
    SheetEntry e;
    e.path = sh::buildPenPath(sh::placeHandwriting(moveRun(san), sh::moveBox(ply), seed), seed);
    for (const sh::PathKey& k : e.path.keys) {
        PenKey pk;
        pk.t = k.t;
        pk.tip = f.padToWorld(sh::pageToPad(k.x, k.y, sh::PAD_TOP + 0.04f + k.lift));
        pk.down = k.down;
        e.keys.push_back(pk);
    }
    return e;
}

double frameEnergy(const std::vector<float>& s, size_t i) { return 0.5 * (double(s[2 * i]) * s[2 * i] + double(s[2 * i + 1]) * s[2 * i + 1]); }
float toDb(double energy) { return float(10.0 * std::log10(std::max(energy, 1e-20))); }

// Mean level (dBFS, unweighted) of a stereo buffer over frame ranges.
float levelDb(const std::vector<float>& s, const std::vector<std::pair<size_t, size_t>>& ranges) {
    double e = 0.0;
    size_t n = 0;
    for (const auto& r : ranges)
        for (size_t i = r.first; i < r.second && i < s.size() / 2; ++i, ++n) e += frameEnergy(s, i);
    return toDb(e / double(std::max<size_t>(n, 1)));
}

// Level (dBFS) of the loudest 50 ms.
float loudest50msDb(const std::vector<float>& s) {
    const size_t W = 2400, n = s.size() / 2;
    double e = 0.0, best = 0.0;
    for (size_t i = 0; i < n; ++i) {
        e += frameEnergy(s, i);
        if (i >= W) e -= frameEnergy(s, i - W);
        if (i + 1 >= W) best = std::max(best, e / double(W));
    }
    return toDb(best);
}

}  // namespace

// A human game as GameScene sets it up, for both colours of the human: the clock at his right, his
// head driven by the first-person camera, both players recording 1. e4 Nf6 on their own pad while
// his playing hand presses the clock, updated with the fixed 60 Hz step of --shot / --warp. On his
// pad the pen is picked up and follows the path, the ink comes where the tip has passed, every
// stroke puts the pen down on time, and its PenDown sound (what Scorekeeper::onEvent hands to
// audio::playPenStroke) lasts the whole stroke. From his head (the listener at his eyes, default
// volumes) his own pen is heard clearly: louder than his opponent's, below a piece being placed.
TEST(anim_own_scoresheet_writing_heard_first_person) {
    using anim::EventType;
    const float dt = 1.0f / 60.0f;
    const float kHumanGazePitch = -0.62f;   // GameScene: looking down at the board from the chair
    const int spf = 800;                    // 48 kHz frames per update
    for (int human = 0; human < 2; ++human) {
        const bool clockPosX = human == 0;  // the clock stands at the human player's right
        anim::Animator an[2];
        sh::PadFrame pad[2];
        std::vector<SheetEntry> entries[2];
        std::vector<float> downsDue[2], strokeLen[2];   // pen-down instants and stroke lengths owed
        for (int seat = 0; seat < 2; ++seat) {
            const float zs = seat == 0 ? 1.0f : -1.0f;
            // GameScene::initAnimators: the hand on the clock side plays, the other one writes.
            const bool clockOnRight = (seat == 0) == clockPosX;
            an[seat].init(character::robotSkeleton(), m::vec3(0, layout::PLAYER_PELVIS_Y, zs * layout::PLAYER_PELVIS_Z), zs,
                          clockOnRight ? character::Side::Right : character::Side::Left);
            const float side = an[seat].playHand() == character::Side::Right ? zs : -zs;
            an[seat].setRestHand(m::vec3(side * 0.24f, layout::TABLE_TOP_Y, zs * 0.34f));
            // Scorekeeper: pads, pen on the table, the writing rest beside the first row, 1. e4 Nf6.
            pad[seat] = sh::padFrame(seat, clockPosX);
            const float restX = pad[seat].outerSign > 0.0f ? sh::PAGE_W - 4.0f : 4.0f;
            an[seat].setWritingRest(pad[seat].padToWorld(sh::pageToPad(restX, sh::cellRect(sh::cellOf(0)).cy(), sh::PAD_TOP)));
            WriteTask pick;
            pick.type = WriteTaskType::PickPen;
            pick.frame = sh::penRestTransform(pad[seat]);
            an[seat].enqueueWriting(pick);
            const uint32_t seed = 1u + uint32_t(seat) * 7919u;
            entries[seat].push_back(sheetEntry(pad[seat], 0, "e4", seed * 31u));
            entries[seat].push_back(sheetEntry(pad[seat], 1, "Nf6", seed * 37u));
            float start = anim::Timing::PickPen;
            for (const SheetEntry& en : entries[seat]) {
                WriteTask w;
                w.type = WriteTaskType::Write;
                w.path = en.keys;
                an[seat].enqueueWriting(w);
                const auto& K = en.keys;
                for (size_t i = 0; i + 1 < K.size(); ++i) {
                    if (!K[i].down || (i > 0 && K[i - 1].down)) continue;
                    size_t e = i + 1;
                    while (e + 1 < K.size() && K[e].down) ++e;
                    downsDue[seat].push_back(start + anim::Timing::WriteApproach + K[i].t);
                    strokeLen[seat].push_back(K[e].t - K[i].t);
                }
                start += anim::writeTaskDuration(w);
            }
        }
        CHECK(an[human].writingHand() == character::Side::Left);   // his pad lies at his left
        an[human].setHeadOverride(true, 0.0f, kHumanGazePitch);
        an[1 - human].lookAt(m::vec3(0, layout::BOARD_TOP_Y, 0));
        // His playing hand is busy meanwhile: it presses the clock while he writes.
        anim::Task wait;
        wait.type = anim::TaskType::Wait;
        wait.duration = 1.2f;
        an[human].enqueue(wait);
        an[human].enqueue(pressClock(clockPosX ? 1.0f : -1.0f, clockPosX ? 1.0f : -1.0f));

        // Offline mixers (default volumes): his own pen, the opponent's pen (the same random draws:
        // both players write the same number of strokes), a piece being placed.
        audio::Mixer mix[3] = {audio::Mixer(21u), audio::Mixer(21u), audio::Mixer(23u)};
        for (audio::Mixer& mx : mix) {
            mx.prepare(48000.0f);
            mx.setVolumes(0.9f, 1.0f, 0.7f);
            mx.setAmbienceEnabled(false, true);
            for (audio::Sfx sfx : {audio::Sfx::PenTap, audio::Sfx::PenWrite, audio::Sfx::PiecePlace})
                for (int v = 0; v < audio::bankVariants(sfx); ++v) {
                    audio::SoundBuffer* b = new audio::SoundBuffer();
                    b->samples = audio::synthesize(sfx, 700u + uint32_t(int(sfx) * 13 + v));
                    b->sfx = int(sfx);
                    b->variant = v;
                    mx.install(b);
                }
        }
        std::vector<float> out[3];
        std::vector<std::pair<size_t, size_t>> heard[2];   // frames of the strokes: his, the opponent's
        size_t entry[2] = {0, 0}, strokes[2] = {0, 0}, revealed[2] = {0, 0};
        int offPad = 0, lateDown = 0, shortSound = 0, wrongEars = 0, inkBack = 0, inkMissing = 0, clockPressed = 0;
        float tipErr = 0.0f;
        bool picked = false, heldWhileWriting = true;
        for (int step = 0; step < int(4.2f / dt); ++step) {
            // GameScene::render: the camera, and so the listener, is at the human player's eyes.
            const m::mat4 eyes = an[human].eyeCameraTransform();
            audio::ListenerPose lis;
            lis.pos = eyes.translation();
            lis.fwd = -m::normalize(eyes.c[2].xyz());
            lis.up = m::normalize(eyes.c[1].xyz());
            for (int seat = 0; seat < 2; ++seat) {
                std::vector<anim::Event> ev;
                an[seat].update(dt, ev);
                for (const anim::Event& e : ev) {
                    if (e.type == EventType::ClockPressed) ++clockPressed;
                    if (e.type == EventType::PenPicked && seat == human) picked = true;
                    if (e.type == EventType::WritingDone && entry[seat] < entries[seat].size()) {
                        if (revealed[seat] != entries[seat][entry[seat]].path.bands.size()) ++inkMissing;
                        ++entry[seat];
                        revealed[seat] = 0;
                    }
                    if (e.type != EventType::PenDown || entry[seat] >= entries[seat].size()) continue;
                    // On this seat's own pad, on the paper, at the instant of the path.
                    const size_t k = strokes[seat]++;
                    const m::vec3 rel = e.position - pad[seat].center;
                    if (std::fabs(m::dot(rel, pad[seat].right)) > 0.5f * layout::SCORESHEET_WIDTH ||
                        std::fabs(m::dot(rel, pad[seat].down)) > 0.5f * layout::SCORESHEET_LENGTH ||
                        std::fabs(rel.y - layout::SCORESHEET_THICKNESS) > 0.001f)
                        ++offPad;
                    if (k >= downsDue[seat].size() || std::fabs(e.time - downsDue[seat][k]) > 1e-4f) {
                        ++lateDown;
                        continue;
                    }
                    // Scorekeeper::onEvent.
                    const float late = std::max(0.0f, an[seat].time() - e.time), now = an[seat].writingPathTime();
                    const sh::PenStrokeSound snd =
                        sh::penStrokeSound(&entries[seat][entry[seat]].path, now >= 0.0f ? now - late : -1.0f, late, e.position,
                                           an[seat].eyeCameraTransform().translation(), lis.pos);
                    // The friction lasts what is left of the stroke (all of them are long here).
                    if (!(snd.seconds > 0.03f) || std::fabs(snd.seconds - (strokeLen[seat][k] - late)) > 2e-3f) ++shortSound;
                    // Heard from the writer's posture by the writer only.
                    if (snd.writersOwn != (seat == human)) ++wrongEars;
                    if (seat != human && m::length(snd.position - e.position) > 1e-6f) ++wrongEars;
                    if (seat == human && std::fabs(m::length(snd.position - lis.pos) - sh::WRITER_EAR_DISTANCE) > 1e-4f) ++wrongEars;
                    audio::PlayRequest req[2];
                    const int n = audio::penStrokeRequests(snd.position, snd.seconds, snd.gain, req);
                    const int stem = seat == human ? 0 : 1;
                    CHECK_EQ(n, 2);
                    for (int i = 0; i < n; ++i) CHECK(mix[stem].play(req[i]));
                    const size_t at = out[stem].size() / 2;   // plays from the next block on
                    heard[stem].push_back({at, at + size_t(snd.seconds * 48000.0f)});
                }
                // The pen follows the path; the ink appears where the tip has passed (Scorekeeper::update).
                const float tp = an[seat].writingPathTime();
                if (tp >= 0.0f && entry[seat] < entries[seat].size()) {
                    const SheetEntry& en = entries[seat][entry[seat]];
                    m::mat4 pen;
                    if (!an[seat].penTransform(pen)) heldWhileWriting = false;
                    else tipErr = std::max(tipErr, m::length(pen.translation() - anim::penPathPoint(en.keys, tp)));
                    size_t r = 0;
                    for (const sh::InkBand& b : en.path.bands) {
                        const sh::PathKey& mid = en.path.keys[size_t((b.k0 + b.k1) / 2)];
                        const float rt = sh::revealTime(en.path, b, mid.x, mid.y);
                        if (rt >= 0.0f && rt <= tp) ++r;
                    }
                    if (r < revealed[seat]) ++inkBack;
                    revealed[seat] = r;
                }
            }
            if (step == 12) {   // a piece put down on e4, for comparison
                audio::PlayRequest r;
                r.sfx = audio::Sfx::PiecePlace;
                r.pos = layout::squareCenter(4, 3);
                r.gain = 0.9f;
                r.pitch = 1.05f;
                CHECK(mix[2].play(r));
            }
            for (int k = 0; k < 3; ++k) {
                mix[k].setListener(lis);
                const size_t o = out[k].size();
                out[k].resize(o + 2 * size_t(spf));
                mix[k].process(out[k].data() + o, spf);
            }
        }
        const float own = levelDb(out[0], heard[0]), opp = levelDb(out[1], heard[1]), piece = loudest50msDb(out[2]);
        std::fprintf(stderr, "  human %s: %zu / %zu strokes on his pad, pen tip error %.2f mm; from his head: his pen %.1f dBFS, "
                     "the opponent's %.1f dBFS (mean while writing), a piece placed %.1f dBFS (loudest 50 ms)\n",
                     human == 0 ? "White" : "Black", strokes[human], downsDue[human].size(), tipErr * 1000.0f, own, opp, piece);
        CHECK(picked);
        CHECK(heldWhileWriting);
        CHECK(tipErr < 5e-4f);
        CHECK_EQ(clockPressed, 1);
        for (int seat = 0; seat < 2; ++seat) {
            CHECK_EQ(entry[seat], entries[seat].size());
            CHECK_EQ(strokes[seat], downsDue[seat].size());
            CHECK(!an[seat].writingBusy());
        }
        CHECK(strokes[human] >= 5);
        CHECK_EQ(offPad, 0);
        CHECK_EQ(lateDown, 0);
        CHECK_EQ(shortSound, 0);
        CHECK_EQ(wrongEars, 0);
        CHECK_EQ(inkBack, 0);
        CHECK_EQ(inkMissing, 0);
        // Clearly audible and nearer than the opponent's pen, well below a piece being placed.
        CHECK(own > -55.0f);
        CHECK(own - opp > 6.0f);
        CHECK(piece - own > 6.0f && piece - own < 22.0f);
    }
}

// =============================================================================================
// Coach gestures: the coach is Black and always left-handed (clock at +X).
// =============================================================================================
namespace {

using character::Side;

// Pieces standing on the board and the table (ids are indices), with obstacle callbacks as the
// game wires them: pieces the animator holds are skipped.
struct CoachBoard {
    std::vector<m::mat4> xf;
    std::vector<int> type;
    const anim::Animator* an = nullptr;
    float mirror = 1.0f;   // -1: the whole layout mirrored about X = 0

    int add(m::vec3 base, int t, float yaw = 0.0f) {
        xf.push_back(m::translate(m::vec3(mirror * base.x, base.y, base.z)) * m::rotateY(yaw));
        type.push_back(t);
        return int(xf.size()) - 1;
    }
    // The starting position (ids as in the anim viewer: 0-7 White pawns a-h, 8-15 White's back
    // rank, 16-23 Black pawns, 24-31 Black's back rank), then the given pawn moves.
    void startPosition() {
        static const int back[8] = {4, 2, 3, 5, 6, 3, 2, 4};
        for (int i = 0; i < 32; ++i) {
            const int color = i >= 16 ? 1 : 0, k = i & 15, file = k & 7;
            const int rank = color == 0 ? (k < 8 ? 1 : 0) : (k < 8 ? 6 : 7);
            add(layout::squareCenter(file, rank), k < 8 ? 1 : back[file], color ? 3.1415927f : 0.0f);
        }
    }
    void moveTo(int id, int sq) {
        const m::vec3 c = layout::squareCenter(sq);
        xf[id] = m::translate(m::vec3(mirror * c.x, c.y, c.z)) * m::rotateY(id >= 16 ? 3.1415927f : 0.0f);
    }
    bool standing(int i) const { return !(an && an->holding(i)) && xf[i].translation().y < layout::BOARD_TOP_Y + 0.01f; }
    int at(int sq) const {
        const m::vec3 c = layout::squareCenter(sq);
        for (int i = 0; i < int(xf.size()); ++i) {
            const m::vec3 p = xf[i].translation();
            if (std::fabs(p.x - mirror * c.x) < 0.005f && std::fabs(p.z - c.z) < 0.005f && std::fabs(p.y - c.y) < 0.005f) return i;
        }
        return -1;
    }
    float topAt(m::vec3 p, float radius, int ignore) const {
        float top = layout::BOARD_TOP_Y;
        for (int i = 0; i < int(xf.size()); ++i) {
            if (i == ignore || !standing(i)) continue;
            const m::vec3 c = xf[i].translation();
            if (m::length(m::vec3(p.x - c.x, 0, p.z - c.z)) < layout::PIECE_BASE_RADIUS[type[i]] + radius)
                top = std::max(top, c.y + layout::PIECE_HEIGHT[type[i]]);
        }
        return top;
    }
    float topNear(m::vec3 from, m::vec3 to) const {
        float top = layout::BOARD_TOP_Y;
        const m::vec3 d(to.x - from.x, 0, to.z - from.z);
        const float len2 = std::max(1e-8f, m::length2(d));
        for (int i = 0; i < int(xf.size()); ++i) {
            if (!standing(i)) continue;
            const m::vec3 p = xf[i].translation();
            const float s = m::clamp(m::dot(m::vec3(p.x - from.x, 0, p.z - from.z), d) / len2, 0.0f, 1.0f);
            const m::vec3 c = from + d * s;
            if (m::length(m::vec3(p.x - c.x, 0, p.z - c.z)) < layout::PIECE_BASE_RADIUS[type[i]] + 0.028f)
                top = std::max(top, p.y + layout::PIECE_HEIGHT[type[i]]);
        }
        return top;
    }
    void wire(anim::Animator& a) {
        an = &a;
        a.pieceTransform = [this](int id) { return id >= 0 && id < int(xf.size()) ? xf[id] : m::mat4(); };
        a.pieceGripInfo = [this](int id) {
            const int t = id >= 0 && id < int(type.size()) ? type[id] : 1;
            return m::vec3(layout::PIECE_HEIGHT[t], layout::PIECE_GRIP_HEIGHT[t], layout::PIECE_GRIP_RADIUS[t]);
        };
        a.pathObstacleTop = [this](m::vec3 f, m::vec3 t) { return topNear(f, t); };
        a.obstacleTopNear = [this](m::vec3 p, float r, int ignore) { return topAt(p, r, ignore); };
    }
    // Deepest overlap of the hand and fingers of 'side' (joints and bone ends, 7 mm) with the
    // standing pieces (Staunton-like profile: the full base up to 12% of the height, then a body
    // of about 60% of the base radius), pieces skip / skip2 excepted.
    float handOverlap(const m::mat4* g, Side side, int skip = -1, int skip2 = -1) const {
        using namespace character;
        const Skeleton& sk = robotSkeleton();
        float worst = 0.0f;
        auto test = [&](m::vec3 p) {
            const float rad = 0.007f;
            for (int i = 0; i < int(xf.size()); ++i) {
                if (i == skip || i == skip2 || !standing(i)) continue;
                const m::vec3 c = xf[i].translation();
                const float H = layout::PIECE_HEIGHT[type[i]], rb = layout::PIECE_BASE_RADIUS[type[i]];
                const float r = (p.y - c.y < 0.12f * H + rad ? rb * 0.95f : rb * 0.62f) + rad;
                worst = std::max(worst, std::min(r - m::length(m::vec3(p.x - c.x, 0, p.z - c.z)), c.y + H + rad - p.y));
            }
        };
        const int hand = sideBone(HandL, side);
        for (int b = hand; b <= hand + (PinkyL3 - HandL); ++b) {
            const bool distal = b != hand && (b - hand) % 3 == 0;
            const m::vec3 dir = b == hand ? sk.restOffset[b + 7] : distal ? sk.restOffset[b] : sk.restOffset[b + 1];
            test(g[b].translation());
            test(m::transformPoint(g[b], m::normalize(dir) * sk.boneLength[b]));
        }
        return worst;
    }
};

// The coach: Black, playing with the left hand (mirror = -1: the right-handed mirror image).
void initCoach(anim::Animator& an, float mirror = 1.0f) {
    an.init(character::robotSkeleton(), m::vec3(0, layout::PLAYER_PELVIS_Y, -layout::PLAYER_PELVIS_Z), -1.0f,
            mirror > 0.0f ? Side::Left : Side::Right);
    an.setRestHand(m::vec3(mirror * 0.24f, layout::TABLE_TOP_Y, -0.34f));
}

anim::Task coachTask(anim::TaskType type, int pieceId = -1, m::vec3 pos = m::vec3(0), int tag = 0) {
    anim::Task t;
    t.type = type;
    t.pieceId = pieceId;
    t.position = pos;
    t.tag = tag;
    return t;
}

m::vec3 mirrorX(m::vec3 p, float mirror) { return m::vec3(mirror * p.x, p.y, p.z); }

// Distance from p to the horizontal polyline 'path' (board plane).
float polylineDistXZ(const std::vector<m::vec3>& path, m::vec3 p) {
    float best = 1e9f;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const m::vec3 a(path[i].x, 0, path[i].z), b(path[i + 1].x, 0, path[i + 1].z), q(p.x, 0, p.z);
        const m::vec3 d = b - a;
        const float s = m::clamp(m::dot(q - a, d) / std::max(1e-9f, m::length2(d)), 0.0f, 1.0f);
        best = std::min(best, m::length(q - (a + d * s)));
    }
    return best;
}

}  // namespace

// The coach points at every square of the starting position (a piece when there is one, else the
// square centre): PointReached after PointApproach, the index aimed at the target, its tip
// 'height' above the piece tops around the target, still during the hold, and no finger in a piece.
TEST(anim_point_reaches_every_square_left_handed) {
    using anim::EventType;
    const float dt = 1.0f / 120.0f;
    float worstAim = 0.0f, worstAbove = 1e9f, worstHold = 0.0f, worstOverlap = 0.0f, worstReached = 0.0f, worstReleased = 0.0f;
    int badEvents = 0, behind = 0;
    for (int sq = 0; sq < 64; ++sq) {
        anim::Animator an;
        initCoach(an);
        CoachBoard b;
        b.startPosition();
        b.wire(an);
        const int id = b.at(sq);
        m::vec3 aim = layout::squareCenter(sq);
        float top = layout::BOARD_TOP_Y;
        if (id >= 0) {
            aim.y += 0.8f * layout::PIECE_HEIGHT[b.type[id]];
            top += layout::PIECE_HEIGHT[b.type[id]];
        }
        top = std::max(top, b.topAt(aim, 0.035f, -1));
        an.enqueue(coachTask(anim::TaskType::Point, id, id >= 0 ? m::vec3(0) : aim, 100 + sq));
        float reached = -1.0f, released = -1.0f;
        m::vec3 tip0;
        std::vector<anim::Event> ev;
        while (an.time() < anim::Timing::PointApproach + anim::Timing::PointHold + 0.05f) {
            ev.clear();
            an.update(dt, ev);
            for (const anim::Event& e : ev) {
                if (e.type == EventType::PointReached) reached = e.time;
                if (e.type == EventType::PointReleased) released = e.time;
                if ((e.type == EventType::PointReached || e.type == EventType::PointReleased) && (e.tag != 100 + sq || m::length(e.position - aim) > 1e-4f))
                    ++badEvents;
            }
            worstOverlap = std::max(worstOverlap, b.handOverlap(an.globals(), Side::Left));
            m::vec3 tip;
            CHECK(an.pointerTip(tip));
            if (reached < 0.0f || released >= 0.0f) continue;
            if (an.time() - reached < dt) {   // just arrived: the aim, the height
                tip0 = tip;
                const m::vec3 mcp = an.globals()[character::IndexL1].translation(), dir = m::normalize(tip - mcp);
                worstAim = std::max(worstAim, m::length(m::cross(aim - tip, dir)));
                if (m::dot(aim - tip, dir) <= 0.0f) ++behind;
                worstAbove = std::min(worstAbove, tip.y - (top + 0.045f));
            } else {
                worstHold = std::max(worstHold, m::length(tip - tip0));
            }
        }
        worstReached = std::max(worstReached, std::fabs(reached - anim::Timing::PointApproach));
        worstReleased = std::max(worstReleased, std::fabs(released - (anim::Timing::PointApproach + anim::Timing::PointHold)));
    }
    std::fprintf(stderr, "  coach points at 64 squares: aim off the finger line %.2f mm, tip above the tops + height %.2f mm, tip drift "
                 "while holding %.3f mm, deepest finger in a piece %.2f mm\n",
                 worstAim * 1000.0f, worstAbove * 1000.0f, worstHold * 1000.0f, worstOverlap * 1000.0f);
    CHECK(worstReached < 1e-4f);
    CHECK(worstReleased < 1e-4f);
    CHECK_EQ(badEvents, 0);
    CHECK_EQ(behind, 0);
    CHECK(worstAim < 0.004f);
    CHECK(worstAbove > -0.001f);
    CHECK(worstHold < 0.0005f);
    CHECK(worstOverlap < 0.001f);
}

// The point lock lets go without a jump: pointing at e8 / f8 (close to the coach, where the wrist
// clamps and the lock shifts the hand), then a Retract. Both seats' handedness.
TEST(anim_point_lock_lets_go_smoothly) {
    float worst = 0.0f;
    for (float mirror : {1.0f, -1.0f})
        for (int sq : {60, 61}) {
            anim::Animator an;
            initCoach(an, mirror);
            CoachBoard b;
            b.startPosition();
            b.wire(an);
            an.enqueue({coachTask(anim::TaskType::Point, -1, layout::squareCenter(sq)), coachTask(anim::TaskType::Retract, -1, m::vec3(0), 1)});
            const character::Bone hand = an.playHand() == Side::Left ? character::HandL : character::HandR;
            std::vector<anim::Event> ev;
            m::vec3 prev = an.globals()[hand].translation();
            int after = -1;   // frames since the Retract started
            while (an.time() < 3.0f) {
                ev.clear();
                an.update(1.0f / 120.0f, ev);
                for (const anim::Event& e : ev)
                    if (e.type == anim::EventType::TaskStarted && e.tag == 1) after = 0;
                const m::vec3 cur = an.globals()[hand].translation();
                if (after >= 0 && after < 3) worst = std::max(worst, m::length(cur - prev));   // the boundary frame and the next two
                if (after >= 0) ++after;
                prev = cur;
            }
            CHECK(after > 0);
        }
    std::fprintf(stderr, "  point lock let go: largest wrist step around the boundary %.2f mm\n", worst * 1000.0f);
    CHECK(worst < 0.001f);
}

// Point, Trace and the three gesture shapes, a nod, a head shake and speech on the left-handed
// coach against a right-handed coach in the mirrored world: the same motion bone for bone, the
// same events (with their tags) at the same instants, the fingertip mirrored.
TEST(anim_gestures_left_handed_mirror) {
    using namespace character;
    anim::Animator L, R;
    initCoach(L, 1.0f);
    initCoach(R, -1.0f);
    CoachBoard bl, br;
    br.mirror = -1.0f;
    for (CoachBoard* b : {&bl, &br}) {
        b->startPosition();
        b->moveTo(4, 28);    // 1.e4
        b->moveTo(19, 35);   // 1...d5
        b->moveTo(9, 18);    // 2.Nc3
    }
    bl.wire(L);
    br.wire(R);
    for (int k = 0; k < 2; ++k) {
        anim::Animator& a = k == 0 ? L : R;
        const float mx = k == 0 ? 1.0f : -1.0f;
        std::vector<anim::Task> ts;
        ts.push_back(coachTask(anim::TaskType::Gesture, -1, m::vec3(0), 1));
        ts.back().shape = anim::HandShape::Open;
        ts.back().duration = 1.0f;
        ts.push_back(coachTask(anim::TaskType::Gesture, -1, m::vec3(0), 2));
        ts.back().shape = anim::HandShape::Beat;
        ts.back().duration = 0.9f;
        ts.push_back(coachTask(anim::TaskType::Point, -1, mirrorX(layout::squareCenter(1), mx), 3));   // b1
        ts.back().duration = 1.2f;
        ts.back().gazeHold = 0.5f;
        ts.push_back(coachTask(anim::TaskType::Point, 9, m::vec3(0), 4));                              // the knight on c3
        ts.back().duration = 1.2f;
        ts.back().emphasis = true;
        ts.push_back(coachTask(anim::TaskType::Trace, -1, m::vec3(0), 5));
        for (const m::vec3& p : anim::moveTracePath(6, 21)) ts.back().path.push_back(mirrorX(p, mx));   // g1-f3
        ts.push_back(coachTask(anim::TaskType::Gesture, -1, mirrorX(layout::squareCenter(28), mx), 6));
        ts.back().shape = anim::HandShape::Present;
        ts.back().duration = 1.0f;
        ts.push_back(coachTask(anim::TaskType::Retract, -1, m::vec3(0), 7));
        a.enqueue(ts);
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
    const m::mat4 S = m::scale(m::vec3(-1, 1, 1));
    float worst = 0.0f, tipDiff = 0.0f;
    int evBad = 0, nGesture = 0;
    std::vector<anim::Event> evL, evR;
    const float dt = 1.0f / 120.0f;
    for (int step = 0; step < int(12.0f / dt); ++step) {
        const float t = float(step) * dt;
        for (anim::Animator* a : {&L, &R}) {
            a->setSpeechLevel(t > 0.5f && t < 4.0f ? 0.5f + 0.5f * std::sin(t * 28.0f) : 0.0f);
            if (step == int(1.0f / dt)) a->nod();
            if (step == int(3.0f / dt)) a->shakeHead();
            if (step == int(4.0f / dt)) a->blink();
            a->lookAt(m::vec3(0, layout::EYE_HEIGHT, layout::PLAYER_PELVIS_Z));
        }
        evL.clear();
        evR.clear();
        L.update(dt, evL);
        R.update(dt, evR);
        if (evL.size() != evR.size()) ++evBad;
        for (size_t i = 0; i < std::min(evL.size(), evR.size()); ++i) {
            const m::vec3 pr = evR[i].position;
            if (evL[i].type >= anim::EventType::PointReached) ++nGesture;
            if (evL[i].type != evR[i].type || evL[i].tag != evR[i].tag || std::fabs(evL[i].time - evR[i].time) > 1e-6f ||
                m::length(evL[i].position - m::vec3(-pr.x, pr.y, pr.z)) > 1e-4f)
                ++evBad;
        }
        for (int b = 0; b < BoneCount; ++b) {
            const m::mat4 want = S * R.globals()[mirrorBone(b)] * S;
            worst = std::max(worst, m::length(L.globals()[b].translation() - want.translation()));
        }
        m::vec3 tl, tr;
        CHECK(L.pointerTip(tl) && R.pointerTip(tr));
        tipDiff = std::max(tipDiff, m::length(tl - m::vec3(-tr.x, tr.y, tr.z)));
    }
    CHECK(nGesture >= 12);
    CHECK_EQ(evBad, 0);
    CHECK(worst < 1e-4f);
    CHECK(tipDiff < 1e-4f);
    CHECK(!L.busy() && !R.busy());
}

// A knight's move traced by the coach: the L through the corner square, PointReached, TraceCorner
// and TraceDone at the scheduled instants, the fingertip on the L at one height all the way.
TEST(anim_trace_knight_path) {
    using anim::EventType;
    const std::vector<m::vec3> path = anim::moveTracePath(6, 21);   // g1-f3
    CHECK_EQ(path.size(), size_t(3));
    CHECK(m::length(path[0] - layout::squareCenter(6)) < 1e-6f);
    CHECK(m::length(path[1] - layout::squareCenter(22)) < 1e-6f);    // g3
    CHECK(m::length(path[2] - layout::squareCenter(21)) < 1e-6f);
    const std::vector<m::vec3> across = anim::moveTracePath(1, 18);  // b1-c3: the long leg up the b-file
    CHECK(across.size() == 3 && m::length(across[1] - layout::squareCenter(17)) < 1e-6f);   // b3
    const std::vector<m::vec3> side = anim::moveTracePath(57, 51);   // b8-d7: the long leg along the 8th rank
    CHECK(side.size() == 3 && m::length(side[1] - layout::squareCenter(59)) < 1e-6f);        // d8
    CHECK_EQ(anim::moveTracePath(5, 33).size(), size_t(2));          // f1-b5, a straight line

    anim::Animator an;
    initCoach(an);
    CoachBoard b;
    b.startPosition();
    b.wire(an);
    anim::Task t = coachTask(anim::TaskType::Trace, -1, m::vec3(0), 5);
    t.path = path;
    an.enqueue(t);
    const float T = anim::taskDuration(t);
    const float S = anim::Timing::TraceSpeed, leg0 = 2.0f * layout::SQUARE_SIZE / S, leg1 = layout::SQUARE_SIZE / S;
    const float tReach = anim::Timing::PointApproach, tCorner = tReach + anim::Timing::TraceDwell + leg0;
    const float tDone = tCorner + anim::Timing::TraceCornerPause + leg1;
    CHECK(std::fabs(T - (tDone + anim::Timing::TraceSettle)) < 1e-5f);
    float reached = -1, corner = -1, done = -1, released = -1;
    int corners = 0, badPos = 0;
    float offPath = 0.0f, yMin = 1e9f, yMax = -1e9f, overlap = 0.0f;
    std::vector<anim::Event> ev;
    const float dt = 1.0f / 240.0f;
    while (an.time() < T + 0.1f) {
        ev.clear();
        an.update(dt, ev);
        for (const anim::Event& e : ev) {
            if (e.tag != 5 && e.type != EventType::TaskStarted && e.type != EventType::QueueEmpty) ++badPos;
            switch (e.type) {
                case EventType::PointReached: reached = e.time; badPos += m::length(e.position - path[0]) > 1e-4f; break;
                case EventType::TraceCorner:
                    corner = e.time;
                    ++corners;
                    badPos += m::length(e.position - path[1]) > 1e-4f;
                    break;
                case EventType::TraceDone: done = e.time; badPos += m::length(e.position - path[2]) > 1e-4f; break;
                case EventType::PointReleased: released = e.time; break;
                default: break;
            }
        }
        overlap = std::max(overlap, b.handOverlap(an.globals(), Side::Left));
        m::vec3 tip;
        an.pointerTip(tip);
        if (reached >= 0.0f && (done < 0.0f || an.time() <= done + 1e-6f)) {
            offPath = std::max(offPath, polylineDistXZ(path, tip));
            yMin = std::min(yMin, tip.y);
            yMax = std::max(yMax, tip.y);
        }
    }
    const float topPath = std::max(b.topNear(path[0], path[1]), b.topNear(path[1], path[2]));
    std::fprintf(stderr, "  knight trace: tip off the L %.2f mm, height %.1f..%.1f mm above the board (tops %.1f mm), deepest finger in a "
                 "piece %.2f mm\n",
                 offPath * 1000.0f, (yMin - layout::BOARD_TOP_Y) * 1000.0f, (yMax - layout::BOARD_TOP_Y) * 1000.0f,
                 (topPath - layout::BOARD_TOP_Y) * 1000.0f, overlap * 1000.0f);
    CHECK(std::fabs(reached - tReach) < 1e-4f);
    CHECK(std::fabs(corner - tCorner) < 1e-4f);
    CHECK(std::fabs(done - tDone) < 1e-4f);
    CHECK(std::fabs(released - T) < 1e-4f);
    CHECK_EQ(corners, 1);
    CHECK_EQ(badPos, 0);
    CHECK(offPath < 0.002f);
    CHECK(yMax - yMin < 0.001f);
    CHECK(yMin > topPath + 0.030f);
    CHECK(overlap < 0.001f);
}

// endHold cuts a hold short (PointReleased then, the next task starts at once) but never an
// approach; notBefore delays a task (the hand holds meanwhile) and counts in remainingTime.
TEST(anim_end_hold_and_not_before) {
    using anim::EventType;
    const float dt = 1.0f / 120.0f;
    const m::vec3 e4 = layout::squareCenter(28);
    std::vector<anim::Event> ev;
    auto run = [&](anim::Animator& an, float until, std::vector<anim::Event>& all) {
        while (an.time() < until - 1e-6f) {
            ev.clear();
            an.update(dt, ev);
            all.insert(all.end(), ev.begin(), ev.end());
        }
    };
    auto find = [](const std::vector<anim::Event>& all, EventType type, int tag) {
        for (const anim::Event& e : all)
            if (e.type == type && e.tag == tag) return e.time;
        return -1.0f;
    };
    {   // Cut during the hold.
        anim::Animator an;
        initCoach(an);
        anim::Task p = coachTask(anim::TaskType::Point, -1, e4, 1);
        p.duration = 3.0f;
        an.enqueue({p, coachTask(anim::TaskType::Retract, -1, m::vec3(0), 2)});
        std::vector<anim::Event> all;
        run(an, 1.0f, all);
        const float cut = an.time();
        an.endHold();
        run(an, 2.5f, all);
        CHECK(std::fabs(find(all, EventType::PointReached, 1) - anim::Timing::PointApproach) < 1e-4f);
        CHECK(std::fabs(find(all, EventType::PointReleased, 1) - cut) < 1e-4f);
        CHECK(std::fabs(find(all, EventType::TaskStarted, 2) - cut) < dt + 1e-4f);
        CHECK(std::fabs(find(all, EventType::QueueEmpty, 2) - (find(all, EventType::TaskStarted, 2) + anim::Timing::Retract)) < 1e-4f);
    }
    {   // endHold during the approach: released when it arrives.
        anim::Animator an;
        initCoach(an);
        anim::Task p = coachTask(anim::TaskType::Point, -1, e4, 1);
        p.duration = 3.0f;
        an.enqueue(p);
        std::vector<anim::Event> all;
        run(an, 0.2f, all);
        an.endHold();
        run(an, 1.0f, all);
        const float reached = find(all, EventType::PointReached, 1);
        CHECK(std::fabs(reached - anim::Timing::PointApproach) < 1e-4f);
        CHECK(std::fabs(find(all, EventType::PointReleased, 1) - reached) < 1e-4f);
        CHECK(!an.runningTask(anim::TaskType::Point));
    }
    {   // endHold does not touch a move.
        anim::Animator an;
        initCoach(an);
        anim::Task w = coachTask(anim::TaskType::Wait, -1, m::vec3(0), 1);
        w.duration = 1.0f;
        an.enqueue(w);
        std::vector<anim::Event> all;
        run(an, 0.1f, all);
        const float before = an.remainingTime();
        an.endHold();
        CHECK(std::fabs(an.remainingTime() - before) < 1e-6f);
    }
    {   // notBefore.
        anim::Animator an;
        initCoach(an);
        std::vector<anim::Event> all;
        run(an, 0.5f, all);
        anim::Task p = coachTask(anim::TaskType::Point, -1, e4, 7);
        p.duration = 1.0f;
        p.notBefore = an.time() + 1.25f;
        an.enqueue(p);
        CHECK(an.busy());
        CHECK(std::fabs(an.remainingTime() - 2.25f) < 1e-4f);
        run(an, 1.0f, all);
        CHECK(std::fabs(an.remainingTime() - 1.75f) < 1e-4f);
        CHECK(find(all, EventType::TaskStarted, 7) < 0.0f);
        run(an, 3.0f, all);
        const float started = find(all, EventType::TaskStarted, 7);
        CHECK(std::fabs(started - p.notBefore) < 1e-4f);
        CHECK(std::fabs(find(all, EventType::PointReached, 7) - (p.notBefore + std::min(anim::Timing::PointApproach, 0.6f))) < 1e-4f);
        CHECK(std::fabs(find(all, EventType::PointReleased, 7) - (p.notBefore + 1.0f)) < 1e-4f);
        CHECK(std::fabs(find(all, EventType::QueueEmpty, 7) - (p.notBefore + 1.0f)) < 1e-4f);
    }
}

// A demonstration on the real position (1.e4 d5): the coach plays 1...dxe4 itself, the captured
// White pawn to the coach's own capture slots, then takes the move back (its pawn home, the White
// pawn back on e4), one move per batch as the game does. Every piece is released within 2 mm of
// where it was asked to go, and the board ends exactly as it was.
TEST(anim_demo_rewind_restores_board) {
    using anim::EventType;
    using anim::TaskType;
    anim::Animator an;
    initCoach(an);
    CoachBoard b;
    b.startPosition();
    b.moveTo(4, 28);    // e4
    b.moveTo(19, 35);   // d5
    b.wire(an);
    const std::vector<m::mat4> start = b.xf;
    const int pawnB = 19, pawnW = 4;
    const m::vec3 d5 = layout::squareCenter(35), e4 = layout::squareCenter(28);
    const m::vec2 s0 = layout::captureSlot(0);
    const m::vec3 slot(s0.x, layout::TABLE_TOP_Y, -s0.y);   // the coach's half, clock side
    auto task = [](TaskType type, int id, m::vec3 pos, float dur, float h = 0.0f) {
        anim::Task t = coachTask(type, id, pos);
        t.duration = dur;
        t.height = h;
        return t;
    };
    std::vector<std::vector<anim::Task>> batches = {
        {task(TaskType::Reach, pawnB, m::vec3(0), 0.55f), task(TaskType::Lift, pawnB, m::vec3(0), 0.20f), task(TaskType::Carry, pawnB, e4, 0.60f),
         task(TaskType::TakeCaptured, pawnW, m::vec3(0), 0.25f), task(TaskType::Place, pawnB, e4, 0.30f),
         task(TaskType::Discard, pawnW, slot, 0.50f), task(TaskType::Retract, -1, m::vec3(0), 0.0f)},
        {task(TaskType::Reach, pawnB, m::vec3(0), 0.50f), task(TaskType::Lift, pawnB, m::vec3(0), 0.18f), task(TaskType::Carry, pawnB, d5, 0.55f),
         task(TaskType::Place, pawnB, d5, 0.28f), task(TaskType::Reach, pawnW, m::vec3(0), 0.55f),
         task(TaskType::Lift, pawnW, m::vec3(0), 0.22f, 0.06f), task(TaskType::Carry, pawnW, e4, 0.60f), task(TaskType::Place, pawnW, e4, 0.30f),
         task(TaskType::Retract, -1, m::vec3(0), 0.0f)}};
    const float dt = 1.0f / 480.0f;
    std::vector<anim::Event> ev;
    std::vector<std::pair<int, m::vec3>> wanted;   // releases asked for, in order
    int releases = 0, gone = 0;
    float worstRelease = 0.0f, worstEvent = 0.0f, overlap = 0.0f;
    bool slotUsed = false;
    m::mat4 lastHeld[32];
    for (size_t k = 0; k < batches.size(); ++k) {
        wanted.clear();
        for (const anim::Task& t : batches[k])
            if (t.type == TaskType::Place || t.type == TaskType::Discard) wanted.push_back({t.pieceId, t.position});
        an.enqueue(batches[k]);
        size_t next = 0;
        for (int step = 0; step < int(8.0f / dt) && an.busy(); ++step) {
            ev.clear();
            an.update(dt, ev);
            for (const anim::Event& e : ev) {
                if (e.type != EventType::PieceReleased && e.type != EventType::CapturedReleased) continue;
                ++releases;
                if (next >= wanted.size() || wanted[next].first != e.pieceId) {
                    ++gone;
                    continue;
                }
                const m::vec3 want = wanted[next++].second;
                // Where the hand really had the piece a moment before letting go, and where it rests.
                worstRelease = std::max(worstRelease, m::length(lastHeld[e.pieceId].translation() - want));
                worstEvent = std::max(worstEvent, m::length(e.transform.translation() - want));
                b.xf[e.pieceId] = e.transform;
                if (e.pieceId == pawnW && k == 0) slotUsed = m::length(e.transform.translation() - slot) < 0.002f;
            }
            for (int i = 0; i < 32; ++i) {
                m::mat4 x;
                if (an.heldPieceTransform(i, x)) {
                    lastHeld[i] = x;
                    b.xf[i] = x;
                }
            }
            // The two pawns are gripped: the hand touches nothing else.
            overlap = std::max(overlap, b.handOverlap(an.globals(), Side::Left, pawnB, pawnW));
        }
        CHECK(!an.busy());
        CHECK_EQ(next, wanted.size());
    }
    float moved = 0.0f;
    for (int i = 0; i < 32; ++i) moved = std::max(moved, m::length(b.xf[i].translation() - start[i].translation()));
    std::fprintf(stderr, "  coach demo dxe4 and back: %d releases, worst %.2f mm off before letting go (%.3f mm at rest), board %.3f mm "
                 "from the start after the rewind, deepest finger in another piece %.2f mm\n",
                 releases, worstRelease * 1000.0f, worstEvent * 1000.0f, moved * 1000.0f, overlap * 1000.0f);
    CHECK_EQ(releases, 4);
    CHECK_EQ(gone, 0);
    CHECK(slotUsed);
    CHECK(worstRelease < 0.002f);
    CHECK(worstEvent < 0.002f);
    CHECK(moved < 0.002f);
    CHECK(overlap < 0.004f);
}
