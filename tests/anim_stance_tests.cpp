// Stance tests (no GPU): a robot getting up, walking to both ends of the table and sitting down
// again (anim/stance.h, Animator::setStance), from both seats, right- and left-handed. The spots
// are reached exactly (StanceReached), the chair is pushed back and drawn in, the pose stays
// finite, planted feet never slide, the body keeps clear of the table and the chairs (capsules
// round the bones against boxes round the furniture of scene/furniture.cpp), the eyes (the first
// person camera) move smoothly, and the hand tasks wait for a seated robot (and a stance for the
// hands). The animator's sources are compiled into anim_tests.cpp.
#include "../src/anim/animator.h"
#include "../src/character/skeleton.h"
#include "test.h"
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

namespace {

using anim::Event;
using anim::EventType;
using anim::Stance;
using character::Side;
using m::mat4;
using m::vec3;
namespace B = character;

constexpr float kDt = 1.0f / 120.0f;

void init(anim::Animator& a, float seat, Side hand = Side::Right) {
    a.init(character::robotSkeleton(), vec3(0, layout::PLAYER_PELVIS_Y, seat * layout::PLAYER_PELVIS_Z), seat, hand);
    a.setRestHand(vec3((hand == Side::Right ? 1.0f : -1.0f) * seat * layout::REST_HAND_X, layout::TABLE_TOP_Y, seat * layout::REST_HAND_Z));
}

bool finite(const mat4& x) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!std::isfinite(x[c][r])) return false;
    return true;
}

// ---- The furniture as boxes (centre, half sizes), world space (scene/furniture.cpp).
struct Box {
    vec3 c, h;
    const char* what;
};
// Signed distance from p to the box (negative inside).
float boxDist(const Box& b, vec3 p) {
    const vec3 q(std::fabs(p.x - b.c.x) - b.h.x, std::fabs(p.y - b.c.y) - b.h.y, std::fabs(p.z - b.c.z) - b.h.z);
    const vec3 o(std::max(q.x, 0.0f), std::max(q.y, 0.0f), std::max(q.z, 0.0f));
    return m::length(o) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
}
std::vector<Box> tableBoxes() {
    const float Y = layout::TABLE_TOP_Y, T = layout::TABLE_TOP_THICKNESS, hx = layout::TABLE_WIDTH * 0.5f, hz = layout::TABLE_DEPTH * 0.5f;
    std::vector<Box> b;
    b.push_back({vec3(0, Y - T * 0.5f, 0), vec3(hx, T * 0.5f, hz), "table top"});
    // The players' aprons (|z| 0.353..0.375, down to 0.655) and the short ones at the ends.
    for (float s : {-1.0f, 1.0f}) {
        b.push_back({vec3(0, 0.5f * (0.655f + Y - T), s * 0.364f), vec3(0.545f, 0.5f * (Y - T - 0.655f), 0.011f), "table apron"});
        b.push_back({vec3(s * 0.534f, 0.5f * (0.61f + Y - T), 0), vec3(0.011f, 0.5f * (Y - T - 0.61f), 0.375f), "table end apron"});
    }
    for (float sx : {-1.0f, 1.0f})
        for (float sz : {-1.0f, 1.0f}) b.push_back({vec3(sx * 0.513f, 0.5f * (Y - T), sz * 0.343f), vec3(0.032f, 0.5f * (Y - T), 0.032f), "table leg"});
    return b;
}
// A chair at seat sign s (+1: White's at +Z, facing -Z) pushed back by 'slide'.
std::vector<Box> chairBoxes(float s, float slide) {
    const float zc = s * (layout::CHAIR_Z + slide);
    auto at = [&](float x, float y, float zLocal) { return vec3(x, y, zc - s * zLocal); };   // chair local z is towards the table
    std::vector<Box> b;
    b.push_back({at(0, 0.3975f, 0.02f), vec3(0.27f, 0.0625f, 0.225f), "chair seat"});
    b.push_back({at(0, 0.815f, -0.23f), vec3(0.23f, 0.255f, 0.05f), "chair back"});
    for (float sx : {-1.0f, 1.0f}) {
        b.push_back({at(sx * 0.24f, 0.17f, 0.21f), vec3(0.025f, 0.17f, 0.025f), "chair front leg"});
        b.push_back({at(sx * 0.20f, 0.17f, -0.17f), vec3(0.025f, 0.17f, 0.025f), "chair back leg"});
    }
    return b;
}

// ---- The robot as capsules round its bones (world).
struct Capsule {
    vec3 a, b;
    float r;
    int part;   // 0 trunk / head, 1 thigh, 2 shin / foot, 3 arm, 4 hand
    const char* what;
};
std::vector<Capsule> bodyCapsules(const mat4* g) {
    std::vector<Capsule> c;
    auto P = [&](B::Bone b) { return g[b].translation(); };
    c.push_back({P(B::Pelvis) + vec3(0, 0.02f, 0), P(B::Spine2), 0.13f, 0, "trunk"});
    c.push_back({P(B::Spine2), P(B::Neck), 0.13f, 0, "chest"});
    c.push_back({m::transformPoint(g[B::Head], vec3(0, 0.10f, 0.02f)), m::transformPoint(g[B::Head], vec3(0, 0.10f, 0.02f)), 0.11f, 0, "head"});
    for (int s = 0; s < 2; ++s) {
        const int o = s == 0 ? 0 : int(B::ThighR) - int(B::ThighL);
        const B::Bone th = B::Bone(B::ThighL + o), sh = B::Bone(B::ShinL + o), ft = B::Bone(B::FootL + o);
        c.push_back({P(th), P(sh), 0.072f, 1, "thigh"});
        c.push_back({P(sh), P(ft), 0.055f, 2, "shin"});
        c.push_back({P(ft) - vec3(0, 0.04f, 0), m::transformPoint(g[ft], vec3(0, -0.05f, 0.15f)), 0.04f, 2, "foot"});
        const int a = s == 0 ? 0 : int(B::ClavicleR) - int(B::ClavicleL);
        const B::Bone ua = B::Bone(B::UpperArmL + a), fa = B::Bone(B::ForeArmL + a), hd = B::Bone(B::HandL + a);
        c.push_back({P(ua), P(fa), 0.048f, 3, "upper arm"});
        c.push_back({P(fa), P(hd), 0.040f, 3, "forearm"});
        c.push_back({m::transformPoint(g[hd], vec3(0, -0.06f, 0)), m::transformPoint(g[hd], vec3(0, -0.06f, 0)), 0.045f, 4, "hand"});
    }
    return c;
}
float capsuleDist(const Capsule& c, const Box& b) {
    float d = 1e9f;
    for (int i = 0; i <= 12; ++i) d = std::min(d, boxDist(b, m::lerp(c.a, c.b, float(i) / 12.0f)));
    return d - c.r;
}

// ---- A tour: Standing, SideLeft, SideRight, Seated, each asked for once the previous one is
// reached (SideLeft goes through Standing, SideRight through Standing again).
struct Reached {
    Stance s;
    float t;
    vec3 pelvis, forward, eventPos;
    float slide;
};
struct Tour {
    std::vector<Reached> reached;
    std::vector<Event> events;
    bool finiteAll = true;
    float maxPlantSlide = 0.0f, plantedFrames = 0.0f;
    float minFloor = 1e9f;                       // lowest sole point (ankle - 0.09) of a flat foot
    float worstTable = 1e9f, worstOwnChair = 1e9f, worstOtherChair = 1e9f;
    std::string worstTableWhat, worstOwnWhat, worstOtherWhat;
    float tTable = 0, tOwn = 0, tSlide = 0, tAcc = 0, tYaw = 0;   // when the worst values happen
    vec3 tableA, tableB;                                          // the capsule then
    float maxEyeBob = 0.0f, maxEyeAcc = 0.0f, maxYawRate = 0.0f;
    float slideMax = 0.0f;
    bool busyWhileMoving = true;
    float endT = 0.0f;
    mat4 seatedLegs[6];                          // ThighL..FootR at the end, seated again
};

const Tour& tour(float seat, Side hand) {
    static std::map<int, Tour> cache;
    const int key = (seat > 0 ? 0 : 1) + (hand == Side::Left ? 2 : 0);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    Tour& T = cache[key];
    anim::Animator a;
    init(a, seat, hand);
    const std::vector<Box> table = tableBoxes(), other = chairBoxes(-seat, 0.0f);
    const Stance order[4] = {Stance::Standing, Stance::SideLeft, Stance::SideRight, Stance::Seated};
    int next = 0;
    float t = 0.0f, askAt = 0.3f;
    vec3 prevAnkle[2];
    bool prevPlanted[2] = {false, false};
    std::vector<vec3> eye;
    std::vector<float> eyeYaw;
    std::vector<bool> walking;
    std::vector<Event> ev;
    while (t < 40.0f) {
        if (next < 4 && t >= askAt) {
            a.setStance(order[next]);
            ++next;
            askAt = 1e9f;
        }
        ev.clear();
        a.update(kDt, ev);
        t += kDt;
        const mat4* g = a.globals();
        for (const Event& e : ev) {
            T.events.push_back(e);
            if (e.type == EventType::StanceReached) {
                Reached r;
                r.s = Stance(e.tag);
                r.t = e.time;
                r.pelvis = g[B::Pelvis].translation();
                vec3 f = m::transformDir(g[B::Pelvis], vec3(0, 0, 1));
                r.forward = m::normalize(vec3(f.x, 0, f.z));
                r.eventPos = e.position;
                r.slide = a.chairSlide();
                T.reached.push_back(r);
                askAt = t + 0.6f;   // the next one a moment later
            }
        }
        if ((a.stanceMoving() || a.stance() != a.stanceTarget()) && !a.busy()) T.busyWhileMoving = false;
        T.slideMax = std::max(T.slideMax, a.chairSlide());
        for (int b = 0; b < int(character::BoneCount); ++b)
            if (!finite(g[b])) T.finiteAll = false;
        const mat4 eyeM = a.eyeCameraTransform();
        if (!finite(eyeM)) T.finiteAll = false;
        // Feet: a flat foot on the floor does not move.
        for (int i = 0; i < 2; ++i) {
            const mat4& F = g[i == 0 ? B::FootL : B::FootR];
            const vec3 ank = F.translation();
            const float up = m::transformDir(F, vec3(0, 1, 0)).y;
            if (up > 0.9999f) T.minFloor = std::min(T.minFloor, ank.y - 0.09f);
            const bool planted = up > 0.99999f && std::fabs(ank.y - 0.09f) < 5e-5f;
            if (planted && prevPlanted[i]) {
                const float sl = m::length(vec3(ank.x - prevAnkle[i].x, 0, ank.z - prevAnkle[i].z));
                if (sl > T.maxPlantSlide) T.tSlide = t;
                T.maxPlantSlide = std::max(T.maxPlantSlide, sl);
                T.plantedFrames += 1.0f;
            }
            prevPlanted[i] = planted;
            prevAnkle[i] = ank;
        }
        // Clearance (only away from the seated pose, which the seated tests cover).
        if (a.stanceMoving() || a.stance() != Stance::Seated) {
            const std::vector<Box> own = chairBoxes(seat, a.chairSlide());
            const bool up = g[B::Pelvis].translation().y > 0.80f;
            for (const Capsule& c : bodyCapsules(g)) {
                for (const Box& b : table) {
                    if (c.part == 4 && b.what[6] == 't') continue;   // hands may rest on the top (pushing back)
                    const float tol = c.part == 3 ? 0.008f : 0.0f;   // forearms over the edge as the hands push
                    const float d = capsuleDist(c, b) + tol;
                    if (d < T.worstTable) {
                        T.tTable = t;
                        T.tableA = c.a;
                        T.tableB = c.b;
                        T.worstTable = d;
                        T.worstTableWhat = std::string(c.what) + " / " + b.what;
                    }
                }
                for (const Box& b : own) {
                    if ((c.part == 0 || c.part == 1) && !up) continue;   // sitting on it
                    const float d = capsuleDist(c, b);
                    if (d < T.worstOwnChair) {
                        T.tOwn = t;
                        T.worstOwnChair = d;
                        T.worstOwnWhat = std::string(c.what) + " / " + b.what;
                    }
                }
                for (const Box& b : other) {
                    const float d = capsuleDist(c, b);
                    if (d < T.worstOtherChair) {
                        T.worstOtherChair = d;
                        T.worstOtherWhat = std::string(c.what) + " / " + b.what;
                    }
                }
            }
        }
        eye.push_back(eyeM.translation());
        // (The yaw of the camera's right axis: well defined however far down the eyes look.)
        const vec3 rt = m::transformDir(eyeM, vec3(1, 0, 0));
        eyeYaw.push_back(std::atan2(rt.z, rt.x));
        walking.push_back(a.stanceMoving() && a.stance() != Stance::Seated && a.stanceTarget() != Stance::Seated &&
                          g[B::Pelvis].translation().y > 0.9f);
        if (next == 4 && a.seated() && !T.reached.empty() && T.reached.back().s == Stance::Seated && t > T.reached.back().t + 0.5f) break;
    }
    T.endT = t;
    for (int i = 0; i < 6; ++i) T.seatedLegs[i] = a.globals()[B::ThighL + i];
    // The eyes: bob = distance from the mean over a step period (walking), acceleration and yaw rate
    // everywhere.
    const int half = int(0.235f / kDt);
    for (size_t k = 1; k + 1 < eye.size(); ++k) {
        const vec3 acc = (eye[k + 1] - eye[k] * 2.0f + eye[k - 1]) * (1.0f / (kDt * kDt));
        if (m::length(acc) > T.maxEyeAcc) T.tAcc = float(k + 1) * kDt;
        T.maxEyeAcc = std::max(T.maxEyeAcc, m::length(acc));
        float dy = eyeYaw[k + 1] - eyeYaw[k];
        while (dy > m::PI) dy -= 2.0f * m::PI;
        while (dy < -m::PI) dy += 2.0f * m::PI;
        if (std::fabs(dy) / kDt > T.maxYawRate) T.tYaw = float(k + 1) * kDt;
        T.maxYawRate = std::max(T.maxYawRate, std::fabs(dy) / kDt);
        if (!walking[k] || k < size_t(half) || k + size_t(half) >= eye.size()) continue;
        float mean = 0.0f;
        for (size_t j = k - size_t(half); j <= k + size_t(half); ++j) mean += eye[j].y;
        mean /= float(2 * half + 1);
        T.maxEyeBob = std::max(T.maxEyeBob, std::fabs(eye[k].y - mean));
    }
    std::printf("    tour seat %+.0f %s: %zu stances in %.2f s, plant slide %.3f mm (%.0f frames, t=%.2f), floor %.2f mm, table %.1f mm (%s, t=%.2f), own chair "
                "%.1f mm (%s, t=%.2f), other chair %.1f mm (%s), eye bob %.1f mm, eye acc %.1f m/s2 (t=%.2f), yaw rate %.0f deg/s (t=%.2f)\n",
                seat, hand == Side::Left ? "left" : "right", T.reached.size(), T.endT, T.maxPlantSlide * 1000.0f, T.plantedFrames, T.tSlide,
                T.minFloor * 1000.0f, T.worstTable * 1000.0f, T.worstTableWhat.c_str(), T.tTable, T.worstOwnChair * 1000.0f, T.worstOwnWhat.c_str(), T.tOwn,
                T.worstOtherChair * 1000.0f, T.worstOtherWhat.c_str(), T.maxEyeBob * 1000.0f, T.maxEyeAcc, T.tAcc, T.maxYawRate / m::DEG, T.tYaw);
    for (const Reached& r : T.reached) std::printf("      reached %d at %.3f\n", int(r.s), r.t);
    std::printf("      table capsule %.3f %.3f %.3f - %.3f %.3f %.3f\n", T.tableA.x, T.tableA.y, T.tableA.z, T.tableB.x, T.tableB.y, T.tableB.z);
    return T;
}

const float kSeats[2] = {1.0f, -1.0f};
const Side kHands[2] = {Side::Right, Side::Left};

float timeOf(const std::vector<Event>& ev, EventType type, int nth = 0, int tag = -1) {
    for (const Event& e : ev)
        if (e.type == type && (tag < 0 || e.tag == tag) && nth-- == 0) return e.time;
    return -1.0f;
}
int countOf(const std::vector<Event>& ev, EventType type, int tag = -1) {
    int n = 0;
    for (const Event& e : ev)
        if (e.type == type && (tag < 0 || e.tag == tag)) ++n;
    return n;
}

}  // namespace

// Every stance of the tour is reached exactly on its spot (stance.h), facing the way it says, with
// one StanceReached each; the chair is fully pushed back standing, back in place seated.
TEST(anim_stance_spots_reached) {
    for (float seat : kSeats)
        for (Side hand : kHands) {
            const Tour& T = tour(seat, hand);
            REQUIRE(T.reached.size() == 4);
            const Stance want[4] = {Stance::Standing, Stance::SideLeft, Stance::SideRight, Stance::Seated};
            for (int i = 0; i < 4; ++i) {
                const Reached& r = T.reached[size_t(i)];
                CHECK(r.s == want[i]);
                const anim::StanceSpot sp = anim::stanceSpot(r.s, seat);
                CHECK(m::length(vec3(r.pelvis.x - sp.pelvis.x, 0, r.pelvis.z - sp.pelvis.z)) < 0.002f);
                CHECK(std::fabs(r.pelvis.y - sp.pelvis.y) < 0.004f);
                CHECK(m::dot(r.forward, sp.forward) > std::cos(3.0f * m::DEG));
                CHECK(m::length(vec3(r.eventPos.x - sp.pelvis.x, 0, r.eventPos.z - sp.pelvis.z)) < 1e-3f && std::fabs(r.eventPos.y) < 1e-3f);
                CHECK(std::fabs(r.slide - (r.s == Stance::Seated ? 0.0f : anim::kChairSlideMax)) < 1e-5f);
            }
            // The robot's own left: White (seat +Z) has it at -X, Black at +X.
            CHECK(T.reached[1].pelvis.x * seat < 0.0f);
            CHECK(T.reached[2].pelvis.x * seat > 0.0f);
            // Own half at the ends of the table.
            CHECK(T.reached[1].pelvis.z * seat > 0.1f && T.reached[2].pelvis.z * seat > 0.1f);
            CHECK(std::fabs(T.slideMax - anim::kChairSlideMax) < 1e-5f);
        }
}

// Rise and sit timings, chair events at the seat's centre, footsteps on the floor.
TEST(anim_stance_events) {
    for (float seat : kSeats)
        for (Side hand : kHands) {
            const Tour& T = tour(seat, hand);
            const std::vector<Event>& ev = T.events;
            CHECK(countOf(ev, EventType::ChairPushed) == 1);
            CHECK(countOf(ev, EventType::ChairPulled) == 1);
            CHECK(countOf(ev, EventType::StanceReached) == 4);
            const float pushed = timeOf(ev, EventType::ChairPushed), up = T.reached[0].t;
            CHECK(pushed > 0.3f && pushed < up);
            CHECK(up - 0.3f > 1.4f && up - 0.3f < 1.8f);   // the rise: ~1.6 s
            const float pulled = timeOf(ev, EventType::ChairPulled), down = T.reached[3].t;
            CHECK(pulled < down && pulled > T.reached[2].t);
            for (const Event& e : ev) {
                if (e.type == EventType::ChairPushed || e.type == EventType::ChairPulled) {
                    const float slide = e.type == EventType::ChairPushed ? 0.0f : anim::kChairSlideMax;
                    CHECK(std::fabs(e.position.x) < 1e-4f && std::fabs(e.position.y - layout::SEAT_HEIGHT) < 1e-4f);
                    CHECK(std::fabs(e.position.z - seat * (layout::CHAIR_Z + slide)) < 1e-4f);
                }
                if (e.type == EventType::Footstep) CHECK(std::fabs(e.position.y) < 1e-4f);
            }
            // Each walk (Standing <-> an end) lasts 2.2 to 2.6 s.
            const float walk1 = T.reached[1].t - (up + 0.6f);
            CHECK(walk1 > 2.2f && walk1 < 2.7f);
            CHECK(countOf(ev, EventType::Footstep) >= 4 + 4 * 4);
        }
}

// No NaN anywhere; planted feet do not slide and no flat foot sinks into the floor.
TEST(anim_stance_feet_planted) {
    for (float seat : kSeats)
        for (Side hand : kHands) {
            const Tour& T = tour(seat, hand);
            CHECK(T.finiteAll);
            CHECK(T.plantedFrames > 1000.0f);
            CHECK(T.maxPlantSlide < 1e-4f);
            CHECK(T.minFloor > -2e-4f);
        }
}

// The body keeps clear of the table, of its chair where it pushed it and of the other chair, over
// the whole tour.
TEST(anim_stance_clearance) {
    for (float seat : kSeats)
        for (Side hand : kHands) {
            const Tour& T = tour(seat, hand);
            CHECK(T.worstTable > 0.0f);
            CHECK(T.worstOwnChair > 0.0f);
            CHECK(T.worstOtherChair > 0.0f);
        }
}

// The eyes (the first person camera): a small walking bob, no jolt, the yaw turning smoothly.
TEST(anim_stance_eyes_smooth) {
    for (float seat : kSeats)
        for (Side hand : kHands) {
            const Tour& T = tour(seat, hand);
            CHECK(T.maxEyeBob < 0.015f);
            CHECK(T.maxEyeAcc < 13.0f);   // (a jolt of 11 cm/s in one frame would read 13 m/s2)
            CHECK(T.maxYawRate < 200.0f * m::DEG);
        }
}

// Seated again: the legs and the pelvis exactly as in the seated pose, the chair in place.
TEST(anim_stance_seated_again) {
    for (float seat : kSeats)
        for (Side hand : kHands) {
            const Tour& T = tour(seat, hand);
            anim::Animator ref;
            init(ref, seat, hand);
            std::vector<Event> ev;
            ref.update(0.5f, ev);
            for (int i = 0; i < 6; ++i) {
                const mat4& a = T.seatedLegs[i];
                const mat4& b = ref.globals()[B::ThighL + i];
                if (m::length(a.translation() - b.translation()) >= 1e-3f)
                    std::printf("    seated again: bone %d at %.4f %.4f %.4f, fresh %.4f %.4f %.4f\n", i, a.translation().x, a.translation().y, a.translation().z,
                                b.translation().x, b.translation().y, b.translation().z);
                CHECK(m::length(a.translation() - b.translation()) < 1e-3f);
                CHECK(m::dot(m::transformDir(a, vec3(0, 0, 1)), m::transformDir(b, vec3(0, 0, 1))) > 0.99999f);
            }
        }
}

// A move queued while the robot stands: the robot sits down first (the target goes back to
// Seated), then plays it.
TEST(anim_stance_task_sits_first) {
    anim::Animator a;
    init(a, 1.0f);
    std::vector<Event> ev, all;
    a.setStance(Stance::Standing);
    CHECK(a.busy());
    float t = 0.0f;
    while (t < 4.0f && countOf(all, EventType::StanceReached) == 0) {
        ev.clear();
        a.update(kDt, ev);
        t += kDt;
        all.insert(all.end(), ev.begin(), ev.end());
        if (countOf(all, EventType::StanceReached) == 0) CHECK(a.busy());
    }
    REQUIRE(a.stance() == Stance::Standing && !a.seated());
    ev.clear();
    a.update(0.2f, ev);
    CHECK(!a.busy());
    anim::Task press;
    press.type = anim::TaskType::PressClock;
    press.position = vec3(layout::CLOCK_OFFSET_X, layout::TABLE_TOP_Y + layout::CLOCK_HEIGHT + 0.005f, 0.045f);
    a.enqueue(press);
    CHECK(a.stanceTarget() == Stance::Seated);
    CHECK(a.busy());
    all.clear();
    bool startedStanding = false;
    for (int k = 0; k < int(8.0f / kDt) && countOf(all, EventType::ClockPressed) == 0; ++k) {
        ev.clear();
        a.update(kDt, ev);
        for (const Event& e : ev)
            if (e.type == EventType::TaskStarted && !a.seated()) startedStanding = true;
        all.insert(all.end(), ev.begin(), ev.end());
    }
    const float seated = timeOf(all, EventType::StanceReached, 0, int(Stance::Seated));
    const float started = timeOf(all, EventType::TaskStarted), pressed = timeOf(all, EventType::ClockPressed);
    CHECK(seated > 0.0f && started >= seated && pressed > started);
    CHECK(!startedStanding);
    CHECK(a.seated() && a.chairSlide() == 0.0f);
}

// A stance asked for while the writing hand works waits for it; the pen is laid down before the
// robot gets up and picked up again once it is seated.
TEST(anim_stance_waits_for_writing) {
    anim::Animator a;
    init(a, 1.0f);
    anim::WriteTask pick;
    pick.type = anim::WriteTaskType::PickPen;
    // The pen beside White's pad (as anim_tests' penFrame).
    const vec3 tip(-(layout::SCORESHEET_X + layout::SCORESHEET_WIDTH * 0.5f + 0.030f), layout::TABLE_TOP_Y + layout::PEN_RADIUS,
                   layout::SCORESHEET_Z - layout::PEN_LENGTH * 0.5f);
    pick.frame = m::translate(tip) * m::toMat4(m::fromTo(vec3(0, 1, 0), vec3(0, 0, 1)), vec3(0));
    a.enqueueWriting(pick);
    std::vector<Event> ev, all;
    a.update(0.1f, ev);
    a.setStance(Stance::Standing);
    float t = 0.1f;
    bool movedWhileWriting = false;
    while (t < 12.0f && countOf(all, EventType::StanceReached) == 0) {
        ev.clear();
        a.update(kDt, ev);
        t += kDt;
        all.insert(all.end(), ev.begin(), ev.end());
        if (a.stanceMoving() && countOf(all, EventType::PenPut) == 0) movedWhileWriting = true;
        if (countOf(all, EventType::StanceReached) == 0) CHECK(a.busy());
    }
    CHECK(!movedWhileWriting);
    const float picked = timeOf(all, EventType::PenPicked), put = timeOf(all, EventType::PenPut), pushed = timeOf(all, EventType::ChairPushed);
    CHECK(picked > 0.0f && put > picked && pushed > put);
    CHECK(a.stance() == Stance::Standing && !a.holdsPen());
    // Back to the seat: the pen comes back to the hand.
    a.setStance(Stance::Seated);
    all.clear();
    for (int k = 0; k < int(8.0f / kDt) && countOf(all, EventType::PenPicked) == 0; ++k) {
        ev.clear();
        a.update(kDt, ev);
        all.insert(all.end(), ev.begin(), ev.end());
    }
    const float seated = timeOf(all, EventType::StanceReached, 0, int(Stance::Seated)), again = timeOf(all, EventType::PenPicked);
    CHECK(seated > 0.0f && again > seated);
    CHECK(a.holdsPen() && a.seated());
}

// A target changed during a leg is taken up at the end of the leg: no StanceReached for a
// stance nobody wants any more, the final one reached exactly.
TEST(anim_stance_retarget) {
    for (Side hand : kHands) {
        anim::Animator a;
        init(a, -1.0f, hand);
        std::vector<Event> ev, all;
        auto run = [&](float secs) {
            for (int k = 0; k < int(secs / kDt + 0.5f); ++k) {
                ev.clear();
                a.update(kDt, ev);
                all.insert(all.end(), ev.begin(), ev.end());
                if (a.stanceMoving() || a.stance() != a.stanceTarget()) CHECK(a.busy());
            }
        };
        a.setStance(Stance::Standing);
        run(0.8f);   // mid-rise
        CHECK(a.stanceMoving() && a.stance() == Stance::Seated);
        a.setStance(Stance::SideRight);
        run(5.0f);
        CHECK(countOf(all, EventType::StanceReached, int(Stance::Standing)) == 0);
        CHECK(countOf(all, EventType::StanceReached, int(Stance::SideRight)) == 1);
        CHECK(a.stance() == Stance::SideRight && !a.busy());
        // Towards the other end, changed to Seated on the way back to Standing.
        a.setStance(Stance::SideLeft);
        run(1.0f);
        CHECK(a.stanceMoving());
        a.setStance(Stance::Seated);
        run(6.0f);
        CHECK(countOf(all, EventType::StanceReached, int(Stance::SideLeft)) == 0);
        CHECK(countOf(all, EventType::StanceReached, int(Stance::Seated)) == 1);
        CHECK(a.seated() && !a.busy() && a.chairSlide() == 0.0f);
        const vec3 p = a.globals()[B::Pelvis].translation();
        CHECK(m::length(p - vec3(0, layout::PLAYER_PELVIS_Y, -layout::PLAYER_PELVIS_Z)) < 1e-4f);
    }
}

// Standing, the first person head goes further down than seated (the back bends: the eyes come
// forward over the board), setLean bends further than seated, and lookAt still finds its target.
TEST(anim_stance_head_and_lean) {
    auto settle = [](anim::Animator& a, float secs) {
        std::vector<Event> ev;
        for (int k = 0; k < int(secs / kDt); ++k) a.update(kDt, ev);
    };
    auto pitchOf = [](const mat4& eye) {
        const vec3 f = m::transformDir(eye, vec3(0, 0, -1));
        return std::atan2(f.y, std::sqrt(f.x * f.x + f.z * f.z));
    };
    for (float seat : kSeats) {
        anim::Animator a;
        init(a, seat);
        a.setStance(Stance::Standing);
        settle(a, 3.0f);
        REQUIRE(a.stance() == Stance::Standing && !a.stanceMoving());
        // The head override: -20 degrees, then -75 (seated, the range ends at -45).
        a.setHeadOverride(true, 0.0f, -20.0f * m::DEG);
        settle(a, 1.0f);
        const mat4 e20 = a.eyeCameraTransform();
        a.setHeadOverride(true, 0.0f, -75.0f * m::DEG);
        settle(a, 1.5f);
        const mat4 e75 = a.eyeCameraTransform();
        float yaw, pitch;
        a.headAngles(yaw, pitch);
        std::printf("    standing seat %+.0f: eye pitch %.1f / %.1f deg, head %.1f deg, eye forward by %.0f mm, down by %.0f mm\n", seat, pitchOf(e20) / m::DEG,
                    pitchOf(e75) / m::DEG, pitch / m::DEG, (e20.translation().z - e75.translation().z) * seat * 1000.0f,
                    (e20.translation().y - e75.translation().y) * 1000.0f);
        CHECK(std::fabs(pitchOf(e20) - (-20.0f * m::DEG)) < 3.0f * m::DEG);
        CHECK(std::fabs(pitchOf(e75) - (-75.0f * m::DEG)) < 3.0f * m::DEG);
        CHECK(std::fabs(pitch - (-75.0f * m::DEG)) < 0.5f * m::DEG);
        CHECK((e20.translation().z - e75.translation().z) * seat > 0.10f);   // the back bends over the board
        // setLean: standing bends further than seated (whose full lean is ~11 degrees).
        a.setHeadOverride(true, 0.0f, -30.0f * m::DEG);
        settle(a, 1.0f);
        const vec3 up = a.eyeCameraTransform().translation();
        a.setLean(1.0f);
        settle(a, 1.5f);
        const float standLean = (up.z - a.eyeCameraTransform().translation().z) * seat;
        anim::Animator s;
        init(s, seat);
        s.setHeadOverride(true, 0.0f, -30.0f * m::DEG);
        settle(s, 1.0f);
        const vec3 sUp = s.eyeCameraTransform().translation();
        s.setLean(1.0f);
        settle(s, 1.5f);
        const float seatLean = (sUp.z - s.eyeCameraTransform().translation().z) * seat;
        std::printf("    lean: standing %.0f mm, seated %.0f mm\n", standLean * 1000.0f, seatLean * 1000.0f);
        CHECK(standLean > seatLean + 0.03f);
        // lookAt from a standing robot: the head turns to a corner of the board.
        a.setLean(0.0f);
        a.setHeadOverride(false);
        const vec3 target(0.15f, layout::BOARD_TOP_Y, -seat * 0.15f);
        a.lookAt(target, 1.0f);
        settle(a, 2.0f);
        const mat4 eye = a.eyeCameraTransform();
        const vec3 want = m::normalize(target - eye.translation());
        // (The eyes take the rest of a look the head does not: the head points within ~25 degrees.)
        const vec3 headFwd = m::transformDir(a.globals()[B::Head], vec3(0, 0, 1));
        std::printf("    lookAt: head %.1f deg off\n", std::acos(m::clamp(m::dot(headFwd, want), -1.0f, 1.0f)) / m::DEG);
        CHECK(m::dot(headFwd, want) > std::cos(25.0f * m::DEG));
    }
}
