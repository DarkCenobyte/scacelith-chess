// The coach's gestures with the playing hand (see animator.h: TaskType::Point / Trace / Gesture)
// and its speaking body language (nods, head shakes, the voice envelope).
//
// Planned in character space like the other tasks (animator.cpp). A pointing hand's orientation
// is searched on the arm's strain (armStrain), the torso lean it takes and how close the hand comes
// to the pieces: no fixed orientation reaches the whole board (from the coach's seat, one tuned
// for the far ranks fails on every square of its own half). The index tip is pinned to its planned
// point (HandSample::pinW, see evaluate), so it stays exactly there even where a wrist limit
// turns the hand a little. A Trace is a procedural segment (Segment::follow) moving the tip over
// the waypoints in straight legs with a stop on each, so a knight's L has crisp corners.
#include "animator_impl.h"
#include <algorithm>

using namespace m;
using namespace character;

namespace anim {

std::vector<vec3> moveTracePath(int from, int to) {
    std::vector<vec3> p;
    if (from < 0 || from > 63 || to < 0 || to > 63) return p;
    const int ff = from & 7, fr = from >> 3, tf = to & 7, tr = to >> 3;
    const int df = std::abs(tf - ff), dr = std::abs(tr - fr);
    p.push_back(layout::squareCenter(from));
    if (df * dr == 2) {
        // A knight: the corner two steps along the long leg, then one step across.
        p.push_back(dr == 2 ? layout::squareCenter(ff, tr) : layout::squareCenter(tf, fr));
    }
    if (to != from) p.push_back(layout::squareCenter(to));
    return p;
}

// =============================================================================================
// Planning helpers
// =============================================================================================
float Animator::Impl::handDepth(vec3 w, quat q, const FingerPose& f, float margin, int ignoreId) const {
    float worst = 0.0f;
    auto test = [&](vec3 lp, float rad) {
        const vec3 pw = toWorld(w + rotate(q, lp));
        // Above their foot, Staunton pieces are much slimmer than the base (about 60%).
        const float r = pw.y - layout::BOARD_TOP_Y > 0.012f ? std::max(0.001f, rad - 0.006f) : rad;
        const float top = topNear(pw, r, ignoreId);
        if (top <= layout::BOARD_TOP_Y + 1e-3f) return;   // no piece there
        worst = std::max(worst, top + margin - (pw.y - rad));
    };
    test(vec3(0), 0.020f);                        // wrist
    test(palmCenter(Side::Right), 0.020f);
    for (int fi = 0; fi < 5; ++fi) {
        mat4 fr[3];
        fingerFrames(*sk, Side::Right, f, fi, fr);
        for (int j = 0; j < 3; ++j) {
            test(fr[j].translation(), j == 0 ? 0.011f : 0.009f);          // joint
            test(phalanxMidFrom(*sk, Side::Right, fr, fi, j), 0.009f);   // phalanx middle
        }
        test(fingerTipFrom(*sk, Side::Right, fr, fi), 0.008f);
    }
    return worst;
}

// Hand rotation and wrist for the index tip of 'fp' on a pointing spot (character space):
//   Point (fixedTip null): the finger aimed at 'aim', the tip 'hover' above topAim (the highest
//     piece top around the target) and above the pieces under the tip itself, as close to the
//     target as that allows;
//   Trace key (fixedTip set): the tip exactly there, the finger pointing down and forwards,
//     turned as little as possible from prevQ (the previous waypoint's rotation).
// Cost: arm strain (clamps, reach), torso lean beyond a slight one, the hand dipping into the
// space of the pieces, and how far the orientation is from the natural one (the finger's azimuth
// from the shoulder, flatter for far targets, the thumb side a little up).
Animator::Impl::PointChoice Animator::Impl::choosePoint(vec3 aim, float topAim, float hover, const vec3* fixedTip, const quat* prevQ,
                                                        const FingerPose& fp) {
    const Side R = Side::Right;
    const vec3 tipL = fingerTip(*sk, R, fp, Index);
    const vec3 dirL = normalize(tipL - sk->restOffset[IndexR1]);
    const vec3 ref = fixedTip ? *fixedTip : aim;
    const vec3 d = ref - shoulderRest(R);
    const float horiz = length(vec3(d.x, 0.0f, d.z));
    const float yaw0 = std::atan2(d.x, std::max(0.08f, d.z)) * 0.9f;
    const float pitch0 = fixedTip ? 0.95f : lerp(0.95f, 0.60f, smoothstep(0.30f, 0.70f, horiz));
    const float roll0 = 0.35f;
    struct Cand {
        quat q;
        vec3 tip;
        float geo;
    };
    std::vector<Cand> cands;
    cands.reserve(256);
    static const float dys[] = {0.0f, -0.15f, 0.15f, -0.30f, 0.30f, -0.45f, 0.45f};
    static const float pitches[] = {0.45f, 0.60f, 0.75f, 0.90f, 1.05f, 1.20f, 1.35f};
    static const float rolls[] = {0.35f, 0.10f, 0.60f, -0.15f};
    for (float pitch : pitches)
        for (float dy : dys)
            for (float roll : rolls) {
                const quat q = handRot(R, yaw0 + dy, pitch, roll);
                vec3 tip;
                float standoff = 0.0f;
                if (fixedTip) {
                    tip = *fixedTip;
                } else {
                    const vec3 dir = rotate(q, dirL);
                    if (dir.y > -0.15f) continue;   // the finger points down at its target
                    standoff = std::max(0.025f, (topAim + hover - aim.y) / -dir.y);
                    tip = aim - dir * standoff;
                    // The tip stands off towards the coach: over a taller piece, higher.
                    const float topTip = topNear(toWorld(tip), 0.02f, -1) - pelvisWorld.y;
                    if (tip.y < topTip + hover) {
                        standoff = (topTip + hover - aim.y) / -dir.y;
                        tip = aim - dir * standoff;
                    }
                }
                float geo = 0.20f * std::fabs(dy) + 0.15f * std::fabs(pitch - pitch0) + 0.10f * std::fabs(roll - roll0) + 0.8f * standoff;
                if (prevQ) geo += 0.6f * std::acos(clamp(std::fabs(dot(q, *prevQ)), 0.0f, 1.0f));   // 0.3 per radian turned
                cands.push_back({q, tip, geo});
            }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.geo < b.geo; });
    PointChoice best;
    for (const Cand& c : cands) {
        if (c.geo >= best.cost) break;   // sorted: nothing further can win
        const vec3 w = c.tip - rotate(c.q, tipL);
        const float depth = handDepth(w, c.q, fp, 0.012f, -1);
        float cost = c.geo + 60.0f * depth;
        if (cost >= best.cost) continue;
        float flex = 0.0f;
        const float strain = armStrain(w, c.q, nullptr, &flex);
        cost += 8.0f * strain + 0.8f * std::max(0.0f, flex - 0.20f);
        if (cost < best.cost) {
            best.q = c.q;
            best.wrist = w;
            best.tip = c.tip;
            best.cost = cost;
            best.strain = strain;
            best.flex = flex;
            best.clear = depth;
        }
    }
    if (best.cost >= 1e9f && !cands.empty()) {   // (cannot happen with a finite cost, kept for safety)
        best.q = cands.front().q;
        best.tip = cands.front().tip;
        best.wrist = best.tip - rotate(best.q, tipL);
    }
    return best;
}

// A hand holding a piece does not gesture (its fingers are busy): the task only holds.
bool Animator::Impl::gestureBlocked(const HandSample& from, float T, Motion& mo) {
    const Hand& h = right();
    if (h.heldId < 0 && h.capId < 0) return false;
    LOGW("anim: gesture %d while the playing hand holds piece %d: it only holds", int(cur.type), h.heldId >= 0 ? h.heldId : h.capId);
    mo.segs.push_back(makeSeg(from, std::max(T, 1e-3f), from.p + from.v * 0.02f, vec3(0), from.q, from.f));
    curLook = false;
    return true;
}

// From wherever the hand is onto a pointing pose (over the pieces, the fingers forming the point
// on the way; the tip lock blends in).
Segment Animator::Impl::pointApproach(const HandSample& from, float Ta, vec3 w, quat q, const FingerPose& fp, vec3 tipL) const {
    const float tableC = layout::TABLE_TOP_Y - pelvisWorld.y;
    Segment a = makeSeg(from, std::max(1e-3f, Ta), w, vec3(0), q, fp);
    a.he = 0.85f;
    a.rot.keys.clear();
    a.rot.add(0.0f, from.q);
    a.rot.add(0.80f, q);
    a.fing.keys.clear();
    a.fing.add(0.0f, from.f);
    a.fing.add(0.55f, fp);
    if (from.p.y - tableC < 0.10f && length(from.v) < 0.05f) a.hs = 0.08f;   // from the table: up first
    clearPath(a, from, fpLerp(from.f, fp, 0.5f), tableC);
    a.pinLocal = tipL;
    a.pinU0 = 0.3f;
    a.pinU1 = 0.9f;
    a.pinFrom = from.pinW;   // (pointing already: stays locked)
    a.pinTo = 1.0f;
    return a;
}

// =============================================================================================
// Point
// =============================================================================================
void Animator::Impl::planPoint(const Task& t, float start, float T, const HandSample& from, Motion& mo) {
    const Side R = Side::Right;
    const float Ta = std::min(Timing::PointApproach, 0.6f * T);
    vec3 aimW = t.position;   // solver world
    float topW = layout::BOARD_TOP_Y;
    if (t.pieceId >= 0) {
        const vec3 gi = gripInfo(t.pieceId), base = pieceWorld(t.pieceId).translation();
        aimW = base + vec3(0, 0.8f * gi.x, 0);
        topW = base.y + gi.x;
    }
    topW = std::max(topW, topNear(aimW, 0.035f, -1));
    curTargetWorld = aimW;
    curArrive = Ta;
    curEvents.push_back({start + Ta, EventType::PointReached, ActNone, false, true, aimW});
    curEvents.push_back({start + T, EventType::PointReleased, ActNone, false, true, aimW});
    if (gestureBlocked(from, T, mo)) return;
    const float hover = t.height > 0.0f ? t.height : 0.045f;
    const FingerPose fp = fpHumanize(posePoint(), 0.5f, 0.02f);
    const vec3 tipL = fingerTip(*sk, R, fp, Index);
    const PointChoice c = choosePoint(toChar(aimW), topW - pelvisWorld.y, hover, nullptr, nullptr, fp);
    if (debugLog)
        LOGI("anim: point at %.3f %.3f %.3f: tip %.1f mm from the aim, strain %.3f, torso flex %.2f rad, hand clearance deficit %.1f mm, cost %.3f",
             aimW.x, aimW.y, aimW.z, length(c.tip - toChar(aimW)) * 1000.0f, c.strain, c.flex, c.clear * 1000.0f, c.cost);
    Segment a = pointApproach(from, Ta, c.wrist, c.q, fp, tipL);
    mo.segs.push_back(a);
    // The hold, the tip locked on its spot; with emphasis two small jabs along the finger.
    Segment b = makeSeg(a.sample(a.T), std::max(1e-3f, T - Ta), c.wrist, vec3(0), c.q, fp);
    b.pinLocal = tipL;
    b.pinFrom = b.pinTo = 1.0f;
    if (t.emphasis) {
        b.oscAmp = 0.008f;
        b.oscCycles = 2.0f;
        b.os = 0.0f;
        b.oe = std::min(1.0f, 0.5f / b.T);
        b.oscAxis = rotate(c.q, normalize(tipL - sk->restOffset[IndexR1]));
    }
    mo.segs.push_back(b);
}

// =============================================================================================
// Trace
// =============================================================================================
void Animator::Impl::planTrace(const Task& t, float start, float T, const HandSample& from, Motion& mo) {
    const Side R = Side::Right;
    const std::vector<vec3>& path = t.path;   // solver world
    if (path.empty()) {
        LOGW("anim: Trace without a path: the hand only holds");
        mo.segs.push_back(makeSeg(from, std::max(T, 1e-3f), from.p + from.v * 0.02f, vec3(0), from.q, from.f));
        curEvents.push_back({start + T, EventType::PointReleased, ActNone, false, true, toWorld(from.p)});
        curLook = false;
        return;
    }
    const TraceSchedule sc = traceSchedule(path, T);
    const float Ta = sc.approach;
    const size_t n = path.size();
    auto tp = std::make_shared<TracePlan>();
    tp->arrive.resize(n);
    tp->leave.resize(n);
    float tt = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        tp->arrive[i] = tt;
        tt += i == 0 ? sc.dwell : (i + 1 < n ? sc.corner : 0.0f);
        tp->leave[i] = tt;
        if (i + 1 < n) tt += sc.legs[i];
    }
    tp->P = tp->leave[n - 1];
    curArrive = Ta + tp->P;
    curEvents.push_back({start + Ta, EventType::PointReached, ActNone, false, true, path.front()});
    for (size_t i = 1; i + 1 < n; ++i) curEvents.push_back({start + Ta + tp->arrive[i], EventType::TraceCorner, ActNone, false, true, path[i]});
    curEvents.push_back({start + Ta + tp->P, EventType::TraceDone, ActNone, false, true, path.back()});
    curEvents.push_back({start + T, EventType::PointReleased, ActNone, false, true, path.back()});
    curTargetWorld = path.front();
    if (gestureBlocked(from, T, mo)) return;

    // One height for the whole path: 'hover' above the highest piece top along the way.
    float topW = layout::BOARD_TOP_Y;
    for (size_t i = 0; i < n; ++i) topW = std::max(topW, topNear(path[i], 0.03f, -1));
    for (size_t i = 0; i + 1 < n; ++i) topW = std::max(topW, obstacleTop(path[i], path[i + 1]));
    const float hover = t.height > 0.0f ? t.height : 0.035f;
    float tipY = topW + hover - pelvisWorld.y;
    const FingerPose fp = fpHumanize(posePoint(), 0.5f, 0.02f);
    tp->f = fp;
    tp->tipL = fingerTip(*sk, R, fp, Index);
    tp->elbow.assign(n, 0.0f);
    for (int iter = 0; iter < 3; ++iter) {
        tp->tip.assign(n, vec3(0));
        tp->q.assign(n, quat());
        for (size_t i = 0; i < n; ++i) {
            const vec3 pc = toChar(path[i]);
            tp->tip[i] = vec3(pc.x, tipY, pc.z);
            tp->q[i] = choosePoint(pc, 0.0f, 0.0f, &tp->tip[i], i > 0 ? &tp->q[i - 1] : nullptr, fp).q;
        }
        // Between the waypoints the rotation is interpolated: raise the whole path while the hand
        // would come closer than 12 mm to the piece tops somewhere.
        float worst = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            worst = std::max(worst, handDepth(tp->tip[i] - rotate(tp->q[i], tp->tipL), tp->q[i], fp, 0.012f, -1));
            if (i + 1 == n) break;
            for (int k = 1; k < 8; ++k) {
                const float s = float(k) / 8.0f;
                const quat q = qslerp(tp->q[i], tp->q[i + 1], s);
                const vec3 tip = lerp(tp->tip[i], tp->tip[i + 1], s);
                worst = std::max(worst, handDepth(tip - rotate(q, tp->tipL), q, fp, 0.012f, -1));
            }
        }
        if (debugLog) LOGI("anim: trace of %d waypoints at %.1f mm above the board (hand clearance deficit %.1f mm)", int(n),
                           (tipY + pelvisWorld.y - layout::BOARD_TOP_Y) * 1000.0f, worst * 1000.0f);
        if (worst < 0.001f) break;
        tipY += worst + 0.003f;
    }
    // Approach onto the first waypoint, the path, the settle on the last one.
    Segment a = pointApproach(from, Ta, tp->tip[0] - rotate(tp->q[0], tp->tipL), tp->q[0], fp, tp->tipL);
    mo.segs.push_back(a);
    Segment b;
    b.T = std::max(1e-4f, tp->P);
    std::shared_ptr<const TracePlan> plan = tp;
    b.follow = [plan](float s) { return plan->sample(s); };
    mo.segs.push_back(b);
    const HandSample e = tp->poseAt(tp->P);
    Segment c = makeSeg(e, std::max(1e-3f, T - Ta - tp->P), e.p, vec3(0), e.q, fp);
    c.pinLocal = tp->tipL;
    c.pinFrom = c.pinTo = 1.0f;
    mo.segs.push_back(c);
    trace = tp;
    traceStart = start + Ta;
}

// After liftForearm (which leaves the procedural path alone): the path starts with the elbow where
// the approach left it, and raises it at a waypoint where the forearm would sweep over the pieces
// on the way there.
void Animator::Impl::finishTracePlan(Hand& h) {
    if (!trace || h.motion.segs.size() != 3 || !h.motion.segs[1].follow) return;
    TracePlan& tp = *trace;
    Segment& a = h.motion.segs[0];
    Segment& c = h.motion.segs[2];
    const size_t n = tp.tip.size();
    tp.elbow.assign(n, a.elbow1);
    if (knowsPieces()) {
        Pose tmp;
        mat4 W[BoneCount];
        auto forearm = [&](float t) {   // how far the forearm dips into the space of the pieces
            evaluate(t, tmp, W);
            const vec3 e = W[ForeArmR].translation(), w = W[HandR].translation();
            float d = 0.0f;
            for (int j = 0; j <= 8; ++j) {
                const vec3 p = e + (w - e) * (0.35f + 0.65f * float(j) / 8.0f);
                const float top = topNear(p, 0.028f, -1);
                if (top > layout::BOARD_TOP_Y + 1e-3f) d = std::max(d, top + 0.030f - p.y);
            }
            return d;
        };
        const float t0 = h.motion.start + a.T;   // the path starts
        for (size_t i = 1; i < n; ++i)
            for (int iter = 0; iter < 6 && tp.elbow[i] < 0.9f; ++iter) {
                float d = 0.0f;
                for (int k = 1; k <= 4; ++k) d = std::max(d, forearm(t0 + tp.leave[i - 1] + (tp.arrive[i] - tp.leave[i - 1]) * float(k) / 4.0f));
                if (d < 0.0005f) break;
                tp.elbow[i] = std::min(0.9f, tp.elbow[i] + std::max(0.08f, d * 6.0f));
                for (size_t j = i + 1; j < n; ++j) tp.elbow[j] = std::max(tp.elbow[j], tp.elbow[i]);
                if (debugLog) LOGI("anim: trace elbow lift %.2f rad at waypoint %d (forearm %.1f mm into the pieces)", tp.elbow[i], int(i), d * 1000.0f);
            }
    }
    c.elbow0 = tp.elbow.back();
    c.elbow1 = std::max(c.elbow1, c.elbow0);
}

// =============================================================================================
// Speaking gestures
// =============================================================================================
void Animator::Impl::planGesture(const Task& t, float start, float T, const HandSample& from, Motion& mo) {
    const Side R = Side::Right;
    const float tableC = layout::TABLE_TOP_Y - pelvisWorld.y;
    const float sx = sideX(R);
    const bool hasPos = length(t.position) > 1e-6f;
    if (t.shape == HandShape::Beat) {
        const int nb = std::max(1, int(std::lround(T / 0.45f)));
        const float P = T / float(nb);
        curLook = false;
        curArrive = T;   // (nothing to cut short)
        // Beat where a Present / Open hand already is, else in front of the body at the board edge.
        const bool inPlace = prevType == TaskType::Gesture && from.p.y > tableC + 0.06f && length(from.v) < 0.3f;
        HandSample base = from;
        if (!inPlace) {
            base.f = fpHumanize(poseBeat(), 0.2f, 0.03f);
            const vec3 spot = hasPos ? toChar(t.position) : vec3(sx * 0.16f, tableC + 0.11f, 0.20f);
            float bestCost = 1e9f;
            for (float up : {0.0f, 0.03f, 0.06f})
                for (float yaw : {0.35f, 0.55f, 0.15f})
                    for (float pitch : {0.30f, 0.50f})
                        for (float roll : {0.75f, 1.05f, 0.45f}) {
                            const quat q = handRot(R, yaw, pitch, roll);
                            const vec3 w = spot + vec3(0, up, 0);
                            // (handDepth is >= 0: an orientation already worse without it is skipped.)
                            const float pref = 0.6f * up + 0.2f * std::fabs(yaw - 0.35f) + 0.1f * std::fabs(roll - 0.75f);
                            if (pref >= bestCost) continue;
                            float cost = pref + 60.0f * handDepth(w, q, base.f, 0.03f, -1);
                            if (cost >= bestCost) continue;
                            cost += 8.0f * armStrain(w, q);
                            if (cost < bestCost) {
                                bestCost = cost;
                                base.p = w;
                                base.q = q;
                            }
                        }
        } else {
            // The strokes dip below where the hand was offered: keep them clear of the pieces under it.
            base.p.y += handDepth(base.p - vec3(0, 0.018f, 0), base.q, base.f, 0.015f, -1);
        }
        const vec3 top = base.p + vec3(0, 0.022f, 0), bottom = base.p - vec3(0, 0.018f, 0);
        curTargetWorld = toWorld(base.p);
        HandSample s = from;
        for (int k = 0; k < nb; ++k) {
            Segment dn = makeSeg(s, 0.6f * P, bottom, vec3(0), base.q, base.f);
            if (k == 0) {
                // Into the first stroke from wherever the hand is: over the top, then down.
                dn.arcH = std::max(0.0f, top.y - 0.5f * (s.p.y + bottom.y)) * 1.1f;
                dn.arcPeak = 0.42f;
                dn.rot.keys.clear();
                dn.rot.add(0.0f, s.q);
                dn.rot.add(0.70f, base.q);
                dn.fing.keys.clear();
                dn.fing.add(0.0f, s.f);
                dn.fing.add(0.60f, base.f);
                if (s.p.y - tableC < 0.10f && length(s.v) < 0.05f) dn.hs = 0.05f;
                clearPath(dn, s, fpLerp(s.f, base.f, 0.5f), tableC);
            } else {
                dn.vs = 0.25f;   // the stroke speeds into its end (the ictus)
            }
            mo.segs.push_back(dn);
            curEvents.push_back({start + (float(k) + 0.6f) * P, EventType::GestureBeat, ActNone, false, true, toWorld(bottom)});
            s = dn.sample(dn.T);
            Segment up = makeSeg(s, 0.4f * P, k + 1 < nb ? top : base.p, vec3(0), base.q, base.f);
            mo.segs.push_back(up);
            s = up.sample(up.T);
        }
        return;
    }
    // Present / Open: the open hand, palm up, towards the target or the listener.
    const bool open = t.shape == HandShape::Open;
    const float Ta = std::min(Timing::PointApproach, 0.5f * T);
    vec3 targetW = t.position;
    if (!hasPos) targetW = open ? toWorld(vec3(0.0f, layout::EYE_HEIGHT - pelvisWorld.y, 2.0f * layout::PLAYER_PELVIS_Z)) : vec3(0, layout::BOARD_TOP_Y, 0);
    curTargetWorld = targetW;
    curArrive = Ta;
    curEvents.push_back({start + Ta, EventType::GestureBeat, ActNone, false, true, targetW});
    if (gestureBlocked(from, T, mo)) return;
    const vec3 tgt = toChar(targetW);
    const vec3 spot = open ? vec3(sx * 0.20f, tableC + 0.15f, 0.20f) : vec3(sx * 0.14f, tableC + 0.12f, 0.21f);
    const vec3 dirH = safeNormalize(vec3(tgt.x - spot.x, 0.0f, tgt.z - spot.z), vec3(0, 0, 1));
    const vec3 base = spot + dirH * (open ? 0.02f : 0.05f);
    const float yaw0 = std::atan2(dirH.x, dirH.z), pitch0 = open ? -0.10f : 0.20f;
    const FingerPose fp = fpHumanize(poseOpenPalm(), 0.35f, 0.03f);
    quat bq = handRot(R, yaw0, pitch0, 0.8f * PI);
    vec3 bw = base;
    float bestCost = 1e9f;
    for (float up : {0.0f, 0.03f, 0.06f})
        for (float dy : {0.0f, -0.2f, 0.2f, -0.4f, 0.4f})
            for (float dp : {0.0f, -0.2f, 0.2f})
                for (float roll : {0.8f * PI, 0.7f * PI, 0.9f * PI, -0.8f * PI, -0.7f * PI, -0.9f * PI}) {
                    const quat q = handRot(R, yaw0 + dy, pitch0 + dp, roll);
                    const vec3 w = base + vec3(0, up, 0);
                    // (handDepth is >= 0: an orientation already worse without it is skipped.)
                    const float pref = 0.6f * up + 0.2f * std::fabs(dy) + 0.15f * std::fabs(dp) + 0.1f * std::fabs(std::fabs(roll) - 0.8f * PI);
                    if (pref >= bestCost) continue;
                    float cost = pref + 60.0f * handDepth(w, q, fp, 0.03f, -1);
                    if (cost >= bestCost) continue;
                    cost += 8.0f * armStrain(w, q);
                    if (cost < bestCost) {
                        bestCost = cost;
                        bq = q;
                        bw = w;
                    }
                }
    if (debugLog) LOGI("anim: gesture %s cost %.3f", open ? "open" : "present", bestCost);
    Segment a = makeSeg(from, std::max(1e-3f, Ta), bw, vec3(0), bq, fp);
    a.rot.keys.clear();
    a.rot.add(0.0f, from.q);
    a.rot.add(0.85f, bq);
    a.fing.keys.clear();
    a.fing.add(0.0f, from.f);
    a.fing.add(0.30f, fpLerp(from.f, poseRelaxed(), 0.6f));
    a.fing.add(0.90f, fp);
    if (from.p.y - tableC < 0.10f && length(from.v) < 0.05f) a.hs = 0.08f;
    clearPath(a, from, fpLerp(from.f, fp, 0.5f), tableC);
    mo.segs.push_back(a);
    // The hold: the hand keeps offering, drifting a little towards the target.
    Segment b = makeSeg(a.sample(a.T), std::max(1e-3f, T - Ta), bw + dirH * (open ? 0.008f : 0.015f), vec3(0), bq, fp);
    mo.segs.push_back(b);
}

// =============================================================================================
// Head and speech
// =============================================================================================
void Animator::Impl::updateSpeech(float dt, float& hy, float& hp) {
    const float lv = speechLevel;
    auto follow = [&](float& x, float rise, float fall) { x += (lv - x) * (1.0f - std::exp(-dt * (lv > x ? rise : fall))); };
    follow(speechFast, 30.0f, 10.0f);
    follow(speechEnv, 12.0f, 4.0f);
    speechSlow += (lv - speechSlow) * (1.0f - std::exp(-dt * 2.5f));
    // A stressed syllable: the level jumps above its recent average.
    speechStress = std::max(0.0f, speechFast - speechSlow - 0.08f);
    const float t = time;
    hp += speechEnv * (0.010f * std::sin(t * 13.2f + seed * 3.0f) + 0.006f * std::sin(t * 5.1f)) - 0.12f * speechStress;
    hy += speechEnv * 0.008f * std::sin(t * 3.7f + 1.1f);
    // Nods and head shakes: smooth one-shot offsets added after the spring (their own shape).
    gestYaw = gestPitch = 0.0f;
    if (nodT >= 0.0f) {
        nodT += dt;
        const float u = nodT / std::max(0.05f, nodDur);
        if (u >= 1.0f) nodT = -1.0f;
        else gestPitch -= nodAmp * bump(u, 0.38f);   // down quickly, back up more slowly
    }
    if (shakeHT >= 0.0f) {
        shakeHT += dt;
        const float u = shakeHT / std::max(0.05f, shakeHDur);
        if (u >= 1.0f) shakeHT = -1.0f;
        else gestYaw += shakeHAmp / 0.92f * std::sin(PI * u) * std::sin(TAU * 2.0f * u);   // two turns each way
    }
}

// =============================================================================================
// Public API
// =============================================================================================
void Animator::endHold() {
    Impl& I = *impl_;
    if (!I.running || !I.isGesture(I.cur.type)) return;
    const float end = std::max(I.time, I.curStart + I.curArrive);   // never before it has arrived
    if (end >= I.curStart + I.curT) return;
    I.curT = end - I.curStart;   // the task machine finishes it there (the next update at the latest)
    for (auto& e : I.curEvents)
        if (!e.done && e.t > end) e.t = end;   // PointReleased now
}

bool Animator::pointerTip(vec3& out) const {
    const Impl& I = *impl_;
    if (!I.sk) return false;
    // The solver's right hand is the playing hand (the real left one of a left-handed player).
    out = I.mw(transformPoint(I.worldI[IndexR3], boneDir(*I.sk, IndexR3) * I.sk->boneLength[IndexR3]));
    return true;
}

void Animator::nod(float amplitude, float duration) {
    Impl& I = *impl_;
    I.nodT = 0.0f;
    I.nodAmp = amplitude;
    I.nodDur = std::max(0.1f, duration);
}

void Animator::shakeHead(float amplitude, float duration) {
    Impl& I = *impl_;
    I.shakeHT = 0.0f;
    I.shakeHAmp = amplitude;
    I.shakeHDur = std::max(0.1f, duration);
}

void Animator::setSpeechLevel(float level) { impl_->speechLevel = clamp(level, 0.0f, 1.0f); }

void Animator::blink() {
    Impl& I = *impl_;
    if (I.blinkPhase < 0.0f) I.blinkPhase = 0.0f;
}

}  // namespace anim
