// Procedural character animation for the seated robots (see animator.h): the playing hand.
//
// Layout of the package:
//   animator_impl.h: small math helpers (quintic Hermite, bumps), the hand model (finger poses,
//     finger forward kinematics and IK, grasp geometry), hand trajectories (Segment, Motion) and
//     the Impl state
//   animator.cpp (this file): body solver (torso lean/twist, clavicle, analytic two-bone arm IK,
//     wrist limits), gaze, blinks, idle life, thinking poses, task planning (one case per
//     TaskType) and the task/event machine
//   animator_writing.cpp: the writing hand (pen, paths, page turns), the left-handed mirror layer
//     and the public writing API
//
// Everything is planned in CHARACTER space (+Y up, +Z forward, +X = character's left, origin at
// the pelvis joint). The root never moves, so character space is inertial; world inputs (pieces,
// clock, partner) are converted once when a task starts.
#include "animator_impl.h"

using namespace m;
using namespace character;

namespace anim {

float taskDuration(const Task& t) {
    if (t.duration > 0.0f) return t.duration;
    switch (t.type) {
        case TaskType::Reach: return Timing::Reach;
        case TaskType::Lift: return Timing::Lift;
        case TaskType::Carry: return Timing::Carry;
        case TaskType::Place: return Timing::Place;
        case TaskType::TakeCaptured: return Timing::TakeCaptured;
        case TaskType::Discard: return Timing::Discard;
        case TaskType::PressClock: return Timing::PressClock;
        case TaskType::Retract: return Timing::Retract;
        case TaskType::Handshake: return Timing::Handshake;
        case TaskType::Wait: return 0.0f;
        case TaskType::Point: return Timing::PointApproach + Timing::PointHold;
        case TaskType::Trace: return detail::traceSchedule(t.path, 0.0f).total();
        case TaskType::Gesture: return Timing::GestureDefault;
    }
    return 0.0f;
}


// =============================================================================================
// Task planning
// =============================================================================================
void Animator::Impl::planTask(const Task& t, float start, float T) {
    Hand& h = right();
    const Side R = Side::Right;
    HandSample from = h.motion.sample(start);
    // The clock lever stops the finger: most of the tap velocity is absorbed at contact.
    if (prevWasClock) from.v = from.v * 0.3f;
    prevWasClock = false;
    // Peek at the next task for fluid pass-through velocities.
    const Task* next = queue.empty() ? nullptr : &queue.front();
    Motion mo;
    mo.start = start;
    const float boardC = layout::BOARD_TOP_Y - pelvisWorld.y;   // board surface, character Y
    const float tableC = layout::TABLE_TOP_Y - pelvisWorld.y;
    curEvents.clear();
    rightIdle = false;
    snapshotPieces();
    validateRests(&t);

    auto heldBase = [&](const HandSample& s) {   // character-space base of the held piece
        mat4 hand = toMat4(s.q, s.p);
        mat4 attachC = h.heldAttach;             // attach is relative to the hand bone: same in any space
        return transformPoint(hand * attachC, vec3(0));
    };
    (void)heldBase;
    auto passVelocity = [&](vec3 p0, vec3 p1, vec3 p2, float T1, float T2, float horizScale) {
        vec3 v = (p2 - p0) / std::max(0.05f, T1 + T2);
        v.x *= horizScale;
        v.z *= horizScale;
        return v;
    };

    switch (t.type) {
        case TaskType::Reach: {
            vec3 gi = gripInfo(t.pieceId);
            mat4 pw = pieceWorld(t.pieceId);
            vec3 baseC = toChar(pw.translation());
            vec3 grip = baseC + vec3(0, gi.y, 0);
            // Where the piece goes (the queued Place), to choose a grip that suits both ends.
            vec3 dstGrip;
            bool haveDst = false;
            for (const Task& qt : queue) {
                if (qt.type == TaskType::Reach) break;
                if (qt.type == TaskType::Place) {
                    dstGrip = toChar(qt.position) + vec3(0, gi.y, 0);
                    haveDst = true;
                    break;
                }
            }
            quat q1 = chooseGrip(grip, gi.z, t.pieceId, haveDst ? &dstGrip : nullptr, h.pinch);
            if (h.capId >= 0) {   // a captured piece already rides in the ring/pinky
                h.pinch.pose = withRingPinky(h.pinch.pose, posePocketClosed());
                h.pinch.open = withRingPinky(h.pinch.open, posePocketClosed());
            }
            h.gripBelow = gi.y;
            h.gripPos = grip;
            vec3 p1 = wristFor(grip, q1, h.pinch.point);
            if (debugLog) {
                const FingerPose& fp = h.pinch.pose;
                vec3 fd = rotate(q1, vec3(0, -1, 0)), pn = rotate(q1, vec3(1, 0, 0));
                LOGI("anim: reach r=%.4f err=%.1fmm axis(l)=%.2f %.2f %.2f fingers(c)=%.2f %.2f %.2f palm(c)=%.2f %.2f %.2f", gi.z, h.pinch.err * 1000,
                     h.pinch.axis.x, h.pinch.axis.y, h.pinch.axis.z, fd.x, fd.y, fd.z, pn.x, pn.y, pn.z);
                LOGI("anim:   thumb %.2f %.2f %.2f %.2f index %.2f %.2f %.2f %.2f middle %.2f %.2f %.2f %.2f", fp.v[0][0], fp.v[0][1], fp.v[0][2],
                     fp.v[0][3], fp.v[1][0], fp.v[1][1], fp.v[1][2], fp.v[1][3], fp.v[2][0], fp.v[2][1], fp.v[2][2], fp.v[2][3]);
                vec3 tp = fingerPad(*sk, R, fp, Thumb), ip = fingerPad(*sk, R, fp, Index), mp = fingerPad(*sk, R, fp, Middle);
                LOGI("anim:   pads(l) thumb %.3f %.3f %.3f index %.3f %.3f %.3f middle %.3f %.3f %.3f", tp.x, tp.y, tp.z, ip.x, ip.y, ip.z, mp.x, mp.y, mp.z);
            }
            Segment sg = makeSeg(from, T, p1, vec3(0), q1, h.pinch.pose);
            // Pass above the other pieces (fingertips hang ~0.12 m under the wrist), finish the
            // horizontal travel early and come down onto the piece.
            float clear = obstacleTop(toWorld(from.p), pw.translation()) - pelvisWorld.y + 0.012f;
            float below = std::max(0.0f, -rotate(q1, fingerTip(*sk, R, h.pinch.open, Middle)).y);
            sg.arcH = arcFor(from.p, p1, clear, below) * 0.85f;
            sg.arcPeak = 0.40f;
            sg.he = 0.74f;                                   // the last part is a descent onto the piece
            if (from.p.y - tableC < 0.10f && length(from.v) < 0.05f) sg.hs = 0.07f;   // from the table: up first
            sg.rot.keys.clear();
            sg.rot.add(0.0f, from.q);
            sg.rot.add(0.70f, q1);
            sg.fing.keys.clear();
            sg.fing.add(0.0f, from.f);
            sg.fing.add(0.50f, h.pinch.open);
            sg.fing.add(0.84f, h.pinch.open);
            sg.fing.add(1.0f, h.pinch.pose);
            mo.segs.push_back(sg);
            curEvents.push_back({start + T, EventType::PieceGripped, ActGripPrimary, false});
            curTargetWorld = toWorld(grip);
            break;
        }
        case TaskType::Lift: {
            float lift = t.height > 0.0f ? t.height : layout::PIECE_LIFT_HEIGHT;
            h.liftH = lift;
            vec3 p1 = from.p + vec3(0, lift, 0);
            vec3 v1(0);
            if (next && next->type == TaskType::Carry && h.heldId >= 0) {
                // Continue into the carry: keep rising, start drifting towards the target.
                vec3 dst = toChar(next->position);
                mat4 hand = toMat4(from.q, from.p);
                vec3 base = transformPoint(hand * h.heldAttach, vec3(0));
                vec3 dir = vec3(dst.x - base.x, 0, dst.z - base.z);
                // (A tall lift, e.g. to hop a rook over the king, must not fling the hand upwards.)
                v1 = vec3(0, std::min(lift / T * 0.9f, 0.40f), 0) + dir * (0.9f / taskDuration(*next));
            }
            Segment sg = makeSeg(from, T, p1, v1, h.gripQ, from.f);
            mo.segs.push_back(sg);
            curTargetWorld = toWorld(from.p + rotate(from.q, h.pinch.point) + vec3(0, lift, 0));
            break;
        }
        case TaskType::Carry: {
            vec3 dst = toChar(t.position);
            float hover = t.height > 0.0f ? t.height : h.liftH;
            if (next && next->type == TaskType::TakeCaptured && next->pieceId >= 0)
                hover = std::max(hover, gripInfo(next->pieceId).x + 0.012f);   // stop above the victim
            // Wrist so the held piece base ends at dst + hover (same rotation as at grip time).
            vec3 attachPos = h.heldId >= 0 ? h.heldAttach.translation() : rotate(conjugate(h.gripQ), -h.pinch.point) * 0.0f;
            // The hand turns about the vertical towards the natural pinch azimuth of the
            // destination (keeps the wrist within its limits); the piece stays upright.
            float dyaw = h.heldId >= 0 ? wrapPi(pinchYawFor(dst + vec3(0, h.gripBelow, 0)) - pinchYawFor(h.gripPos)) * 0.9f : 0.0f;
            if (h.heldId >= 0) {
                // Fine-tune the turn so the arm can set the piece down without straining the wrist.
                const vec3 ap = h.heldAttach.translation();
                float best = 1e9f, bestYaw = dyaw;
                for (int k = -4; k <= 4; ++k) {
                    float y = dyaw + 0.1f * float(k);
                    quat q = normalize(qy(y) * h.gripQ);
                    float c = armStrain(dst - rotate(q, ap), q) + 0.04f * std::fabs(y - dyaw);
                    if (c < best - 1e-4f) {
                        best = c;
                        bestYaw = y;
                    }
                }
                dyaw = bestYaw;
            }
            quat q1 = normalize(qy(dyaw) * h.gripQ);
            h.carryQ = q1;
            if (debugLog)
                LOGI("anim: carry dyaw %.2f rad, from.q-q1 %.2f rad, from.p %.3f %.3f %.3f", dyaw, 2.0f * std::acos(clamp(std::fabs(dot(from.q, q1)), 0.0f, 1.0f)),
                     from.p.x, from.p.y, from.p.z);
            vec3 p1 = dst + vec3(0, hover, 0) - rotate(q1, attachPos);
            if (h.heldId < 0) p1 = wristFor(dst + vec3(0, hover + h.gripBelow, 0), q1, h.pinch.point);
            vec3 v1(0);
            if (next && next->type == TaskType::Place) {
                v1 = vec3(0, -hover / taskDuration(*next) * 0.9f, 0);
            } else if (next && next->type == TaskType::TakeCaptured) {
                v1 = vec3(0, -0.12f, 0);
            }
            Segment sg = makeSeg(from, T, p1, v1, q1, from.f);
            if (next && next->type == TaskType::TakeCaptured && next->pieceId >= 0 && h.heldId >= 0) {
                // Get ready for the capture: ring/pinky open the pocket during the approach.
                sg.fing.keys.clear();
                sg.fing.add(0.0f, from.f);
                sg.fing.add(0.40f, from.f);
                sg.fing.add(1.0f, withRingPinky(from.f, posePocketOpen()));
            }
            // Clearance: the held piece bottom must pass PIECE_LIFT_HEIGHT above the piece tops.
            mat4 hand0 = toMat4(from.q, from.p);
            vec3 base0 = transformPoint(hand0 * h.heldAttach, vec3(0));
            // Stopping above a victim: it is not an obstacle to clear by the full lift height.
            vec3 qTo = t.position;
            if (next && next->type == TaskType::TakeCaptured) {
                vec3 dW = vec3(t.position.x, 0, t.position.z) - vec3(toWorld(base0).x, 0, toWorld(base0).z);
                float dl = length(dW);
                if (dl > 1e-4f) qTo = t.position - dW * (std::min(dl, layout::SQUARE_SIZE * 0.95f) / dl);
            }
            float top = obstacleTop(toWorld(base0), qTo) - pelvisWorld.y;
            float clearY = top + layout::PIECE_LIFT_HEIGHT;
            float bottom0 = base0.y, bottom1 = dst.y + hover;
            float hd = length(vec3(dst.x - base0.x, 0, dst.z - base0.z));
            float need = clearY - 0.5f * (bottom0 + bottom1);
            float s = smoothstep(0.02f, 0.10f, hd);
            // The rise carried over from the lift already lifts the middle of the path.
            float rise = sg.basePos(0.5f * T).y - 0.5f * (sg.p0.y + sg.p1.y);
            sg.arcH = std::max(0.0f, std::max(0.0f, need) * 1.12f * s + 0.012f * s - std::max(0.0f, rise));
            sg.arcPeak = 0.5f;
            sg.swing = 0.0045f;
            sg.pivot = h.pinch.point;
            if (h.heldId >= 0) {
                sg.usePivot = true;   // the turn happens about the piece
                sg.rotPivot = attachPos;
            }
            mo.segs.push_back(sg);
            curTargetWorld = t.position + vec3(0, hover, 0);
            break;
        }
        case TaskType::Place: {
            vec3 dst = toChar(t.position);
            quat q1 = h.carryQ;
            vec3 attachPos = h.heldAttach.translation();
            vec3 p1 = h.heldId >= 0 ? dst - rotate(q1, attachPos) : wristFor(dst + vec3(0, h.gripBelow, 0), q1, h.pinch.point);
            // Release: fingers open slightly right at the contact (end of the task) and relax
            // during the next task.
            FingerPose rel = fpLerp(h.pinch.pose, h.pinch.open, 0.35f);
            if (h.capId >= 0) rel = withRingPinky(rel, posePocketClosed());
            FingerPose held = from.f;
            Segment sg = makeSeg(from, T, p1, vec3(0), q1, held);
            sg.he = 0.78f;
            sg.rot.keys.clear();
            sg.rot.add(0.0f, from.q);
            sg.rot.add(0.74f, q1);   // upright before the final approach
            if (h.heldId >= 0) {
                sg.usePivot = true;       // the piece base, not the wrist, follows the descent
                sg.rotPivot = attachPos;
            }
            sg.fing.keys.clear();
            sg.fing.add(0.0f, from.f);
            sg.fing.add(0.90f, held);
            sg.fing.add(1.0f, fpLerp(held, rel, 0.25f));
            mo.segs.push_back(sg);
            curEvents.push_back({start + T, EventType::PieceReleased, ActReleasePrimary, false});
            curTargetWorld = t.position;
            break;
        }
        case TaskType::TakeCaptured: {
            vec3 gi = gripInfo(t.pieceId);
            mat4 pw = pieceWorld(t.pieceId);
            vec3 baseC = toChar(pw.translation());
            FingerPose closed = withRingPinky(from.f, posePocketClosed());
            FingerPose open = withRingPinky(from.f, posePocketOpen());
            if (h.heldId < 0) {
                closed = withRingPinky(poseRelaxed(), posePocketClosed());
                open = withRingPinky(poseRelaxed(), posePocketOpen());
            }
            h.pocket = pocketPoint(*sk, closed);
            quat q1 = pinchRotFor(baseC + vec3(0, gi.y, 0));
            float gy = gi.y * 0.85f;
            if (h.heldId >= 0) {
                CapturePlan cp = capturePlan(h, t.pieceId, baseC, boardC);
                q1 = cp.q;
                gy = cp.gy;
            }
            vec3 grip = baseC + vec3(0, gy, 0);
            vec3 p1 = wristFor(grip, q1, h.pocket);
            if (debugLog && h.heldId >= 0) {
                vec3 capBase = h.pocket - rotate(conjugate(q1), vec3(0, gy, 0));
                float atPlace = rotate(h.carryQ, capBase - h.heldAttach.translation()).y;
                LOGI("anim: take captured gy %.3f (grip %.3f top %.3f), roll %.2f rad, victim base at place %.1f mm", gy, gi.y, gi.x,
                     2.0f * std::acos(clamp(std::fabs(dot(q1, h.carryQ)), 0.0f, 1.0f)), atPlace * 1000.0f);
            }
            Segment sg = makeSeg(from, T, p1, vec3(0), q1, closed);
            sg.he = 0.85f;
            sg.fing.keys.clear();
            sg.fing.add(0.0f, from.f);
            sg.fing.add(0.30f, open);
            sg.fing.add(0.46f, open);
            sg.fing.add(1.0f, closed);
            sg.rot.keys.clear();
            sg.rot.add(0.0f, from.q);
            sg.rot.add(0.82f, q1);
            sg.arcH = 0.012f;
            mo.segs.push_back(sg);
            curEvents.push_back({start + T, EventType::CapturedGripped, ActGripCaptured, false});
            curTargetWorld = toWorld(grip);
            break;
        }
        case TaskType::Discard: {
            vec3 dst = toChar(t.position);
            // Keep the rotation of the capture (piece upright) with a natural yaw towards the spot.
            quat qc = h.capQ;
            vec3 S = shoulderRest(R);
            float yawNow = std::atan2(from.p.x - S.x, std::max(0.08f, from.p.z - S.z));
            float yawDst = std::atan2(dst.x - S.x, std::max(0.08f, dst.z - S.z));
            vec3 attachPos = h.capAttach.translation();
            // Any turn about the vertical keeps the piece upright: take the one the arm can do
            // comfortably (spots far out to the side need the hand turned more than naturally).
            const float yaw0 = clamp(wrapPi(yawDst - yawNow) * 0.6f, -0.9f, 0.9f);
            float yaw = yaw0, best = 1e9f;
            for (int k = -6; k <= 6; ++k) {
                float y = yaw0 + 0.1f * float(k);
                quat q = normalize(qy(y) * qc);
                float c = armStrain(dst - rotate(q, attachPos), q) + 0.04f * std::fabs(y - yaw0);
                if (c < best - 1e-4f) {
                    best = c;
                    yaw = y;
                }
            }
            if (debugLog) LOGI("anim: discard yaw %.2f rad (natural %.2f), strain %.2f", yaw, yaw0, best);
            quat q1 = normalize(qy(yaw) * qc);
            vec3 p1 = dst - rotate(q1, attachPos);
            FingerPose held = withRingPinky(fpLerp(poseRelaxed(), from.f, 0.3f), posePocketClosed());
            FingerPose rel = withRingPinky(held, posePocketOpen());
            // The free fingers hang lower than the piece when the hand is rolled: curl them so no
            // fingertip touches the surface the piece is set on.
            auto lowestTip = [&](const FingerPose& f) {
                float lo = 1e9f;
                for (int i = 0; i < 5; ++i) lo = std::min(lo, p1.y + rotate(q1, fingerTip(*sk, R, f, i)).y - kPadRadius);
                return lo;
            };
            {
                const FingerPose held0 = held, fist = withRingPinky(poseLooseFist(), held0);
                for (int k = 1; k <= 4 && std::min(lowestTip(held), lowestTip(fpLerp(held, rel, 0.25f))) < dst.y + 0.004f; ++k) {
                    held = fpLerp(held0, fist, 0.25f * float(k));
                    rel = withRingPinky(held, posePocketOpen());
                }
                if (debugLog) LOGI("anim: discard lowest fingertip %.1f mm above the surface", (std::min(lowestTip(held), lowestTip(fpLerp(held, rel, 0.25f))) - dst.y) * 1000.0f);
            }
            Segment sg = makeSeg(from, T, p1, vec3(0), q1, held);
            sg.hs = 0.0f;
            sg.he = 0.82f;
            vec3 base0 = transformPoint(toMat4(from.q, from.p) * h.capAttach, vec3(0));
            float top = obstacleTop(toWorld(base0), t.position) - pelvisWorld.y;
            float need = top + 0.02f - 0.5f * (base0.y + dst.y);
            sg.arcH = std::max(0.02f, need * 0.9f);
            sg.arcPeak = 0.45f;
            sg.fing.keys.clear();
            sg.fing.add(0.0f, from.f);
            sg.fing.add(0.40f, held);
            sg.fing.add(0.88f, held);
            sg.fing.add(1.0f, fpLerp(held, rel, 0.25f));
            sg.rot.keys.clear();
            sg.rot.add(0.0f, from.q);
            sg.rot.add(0.75f, q1);
            mo.segs.push_back(sg);
            curEvents.push_back({start + T, EventType::CapturedReleased, ActReleaseCaptured, false});
            curTargetWorld = t.position;
            break;
        }
        case TaskType::PressClock: {
            vec3 lever = toChar(t.position);
            vec3 S = shoulderRest(R);
            vec3 d = lever - S;
            float yaw = std::atan2(d.x, std::max(0.08f, d.z)) * 0.85f + 0.10f;
            quat q1 = handRot(R, yaw, 0.62f, 0.10f);
            FingerPose fp = fpHumanize(posePress(), 0.4f, 0.03f);
            if (h.capId >= 0) fp = withRingPinky(fp, posePocketClosed());
            vec3 pp = pressPoint(*sk, fp) + vec3(0.0f, 0.0f, 0.0f);
            // Pad contact: the pads are kPadRadius under the tip centre line.
            vec3 contact = lever + vec3(0, kPadRadius, 0);
            vec3 p1 = wristFor(contact, q1, pp);
            const float vContact = 0.22f;   // m/s downwards at contact (a decisive tap)
            Segment sg = makeSeg(from, T, p1, vec3(0, -vContact, 0), q1, fp);
            sg.he = 0.80f;
            float below = handBelow(qslerp(from.q, q1, 0.4f), fpLerp(from.f, fp, 0.5f));
            float clear = std::max(lever.y + 0.01f, pathTop(from, p1, q1, fp) + 0.012f);
            sg.arcH = arcFor(from.p, p1, clear, below) * 0.9f + 0.01f;
            sg.arcPeak = 0.40f;
            sg.rot.keys.clear();
            sg.rot.add(0.0f, from.q);
            sg.rot.add(0.72f, q1);
            sg.fing.keys.clear();
            sg.fing.add(0.0f, from.f);
            sg.fing.add(0.60f, fp);
            mo.segs.push_back(sg);
            curEvents.push_back({start + T, EventType::ClockPressed, ActNone, false});
            curTargetWorld = t.position;
            prevWasClock = true;
            break;
        }
        case TaskType::Retract: {
            HandSample r = h.rest;
            Segment sg = makeSeg(from, T, r.p, vec3(0), r.q, r.f);
            bool fromBoard = from.p.y - boardC < 0.30f;
            if (length(from.v) < 0.05f && fromBoard) sg.hs = 0.10f;
            sg.arcH = length(vec3(r.p.x - from.p.x, 0, r.p.z - from.p.z)) > 0.08f ? 0.03f : 0.0f;
            sg.arcPeak = 0.35f;
            clearPath(sg, from, fpLerp(from.f, poseRelaxed(), 0.7f), tableC);
            sg.rot.keys.clear();
            sg.rot.add(0.0f, from.q);
            sg.rot.add(0.85f, r.q);
            sg.fing.keys.clear();
            sg.fing.add(0.0f, from.f);
            sg.fing.add(0.35f, fpLerp(from.f, poseRelaxed(), 0.7f));
            sg.fing.add(1.0f, r.f);
            mo.segs.push_back(sg);
            curTargetWorld = toWorld(r.p);
            break;
        }
        case TaskType::Handshake: {
            if (!mirrored) {
                planHandshake(t, start, T, from, mo);
            } else {
                // Left-handed player: the real right hand (the solver's left one, the writing hand)
                // shakes hands; the playing hand settles at its rest meanwhile.
                Motion lm;
                lm.start = start;
                planHandshake(t, start, T, left().motion.sample(start), lm);
                left().motion = lm;
                const float Tr = std::min(T, 0.5f);
                mo.segs.push_back(makeSeg(from, Tr, h.rest.p, vec3(0), h.rest.q, h.rest.f));
                mo.segs.push_back(makeSeg(mo.segs.back().sample(Tr), std::max(1e-3f, T - Tr), h.rest.p, vec3(0), h.rest.q, h.rest.f));
            }
            break;
        }
        case TaskType::Wait: {
            HandSample s = from;
            Segment sg = makeSeg(s, std::max(T, 1e-3f), s.p + s.v * 0.02f, vec3(0), s.q, s.f);
            mo.segs.push_back(sg);
            break;
        }
        // Coach gestures (animator_gesture.cpp).
        case TaskType::Point: planPoint(t, start, T, from, mo); break;
        case TaskType::Trace: planTrace(t, start, T, from, mo); break;
        case TaskType::Gesture: planGesture(t, start, T, from, mo); break;
    }
    (void)tableC;
    (void)passVelocity;
    h.motion = mo;
    if (debugLog)
        for (auto& sg : mo.segs) {
            vec3 a = sg.sample(sg.T * 0.25f).p, b = sg.sample(sg.T * 0.5f).p, c = sg.sample(sg.T * 0.75f).p;
            LOGI("anim: seg T %.3f p0 %.3f %.3f %.3f v0 %.2f %.2f %.2f a0 %.1f %.1f %.1f p1 %.3f %.3f %.3f arc %.3f | %.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f", sg.T,
                 sg.p0.x, sg.p0.y, sg.p0.z, sg.v0.x, sg.v0.y, sg.v0.z, sg.a0.x, sg.a0.y, sg.a0.z, sg.p1.x, sg.p1.y, sg.p1.z, sg.arcH, a.x, a.y, a.z, b.x, b.y, b.z,
                 c.x, c.y, c.z);
        }
    if (t.type == TaskType::Reach || t.type == TaskType::PressClock || t.type == TaskType::Retract || t.type == TaskType::Discard ||
        t.type == TaskType::Handshake || isGesture(t.type))
        relaxWrist(h);
    if (t.type != TaskType::Wait) liftForearm(h);
    if (t.type == TaskType::Trace) finishTracePlan(h);
    h.releasedId = -1;
}

// Handshake with the real right hand: the solver's right hand, or its left one (the writing hand)
// for a left-handed player. Written for either side, so the left-handed handshake is the exact
// mirror image of the right-handed one. A writing hand still holding the pen lays it down first.
void Animator::Impl::planHandshake(const Task& t, float start, float T, HandSample from, Motion& mo) {
    const Side R = shakeSide();
    Hand& h = shakeHand();
    const float tableC = layout::TABLE_TOP_Y - pelvisWorld.y;
    partner = t.partner;
    shakeStart = start;
    // Clasp centre (world): above the board centre, between the two players.
    vec3 cW = t.position;
    if (length(cW) < 1e-6f) {
        vec3 mid = pelvisWorld;
        if (partner && partner->impl_) mid = (pelvisWorld + partnerPoint(partner->impl_->pelvisWorld)) * 0.5f;
        cW = vec3(mid.x, layout::BOARD_TOP_Y + 0.225f, mid.z);
    }
    vec3 C = toChar(cW);
    // Palm plane: vertical, through C, along the line joining both right shoulders.
    vec3 S = shoulderRest(R);
    vec3 dirH = safeNormalize(vec3(C.x - S.x, 0, C.z - S.z), vec3(0, 0, 1));
    float yaw = std::atan2(dirH.x, dirH.z);
    // Fingers towards the partner, pitched down; thumb up; palm facing the partner's palm.
    quat qs = handRot(R, yaw, 0.28f, PI * 0.5f);
    const vec3 palm = handPoint(R, palmCenter(Side::Right) + vec3(0.0015f, 0, 0));
    const vec3 palmLocalN(palmSign(R), 0, 0);
    {
        // Turn about the palm normal (keeps both palms in the same plane) for a comfortable wrist.
        vec3 pn = rotate(qs, palmLocalN);
        float best = 1e9f;
        quat qb = qs;
        for (int k = -6; k <= 6; ++k) {
            quat q = normalize(axisAngle(pn, 0.1f * float(k)) * qs);
            float c = armStrainSide(R, wristFor(C, q, palm), q) + 0.03f * std::fabs(0.1f * float(k));
            if (c < best) {
                best = c;
                qb = q;
            }
        }
        qs = qb;
    }
    FingerPose fo = fpHumanize(poseShakeOpen(), 0.2f, 0.03f), fg = fpHumanize(poseShakeGrip(), 0.6f, 0.04f);
    vec3 pClasp = wristFor(C, qs, palm);
    // Pre-contact: 7 cm back along the fingers, 1.5 cm off the palm plane.
    vec3 fingerDir = rotate(qs, vec3(0, -1, 0));
    vec3 palmN = rotate(qs, palmLocalN);
    vec3 pPre = pClasp - fingerDir * 0.07f - palmN * 0.015f + vec3(0, 0.01f, 0);
    float tClasp = Timing::HandshakeClaspAt, tRel = Timing::HandshakeReleaseAt;
    float scale = T / Timing::Handshake;
    float t1 = 0.74f * scale, t2 = tClasp * scale, t3 = tRel * scale;
    float t4 = t3 + 0.15f * scale;
    // 0. The pen first goes back onto the table (where the game wanted it, else where it was taken).
    float t0 = 0.0f;
    FingerPose letGo = from.f;
    shakeTookPut = false;
    if (R == Side::Left && wr.penHeld) {
        shakePutFrame = wr.penTable;
        for (auto it = wr.queue.begin(); it != wr.queue.end(); ++it)
            if (it->type == WriteTaskType::PutPen) {
                shakePutFrame = it->frame;
                wr.queue.erase(it);
                shakeTookPut = true;
                break;
            }
        t0 = 0.30f * scale;
        penPutSegments(toCharM(shakePutFrame), from, t0, mo, &letGo);
        from = mo.segs.back().sample(t0);
        curEvents.push_back({start + t0, EventType::PenPut, ActPutPen, false});
    }
    HandSample s = from;
    // 1. extend
    Segment a = makeSeg(s, t1 - t0, pPre, (pClasp - pPre) * (0.8f / (t2 - t1)), qs, fo);
    a.arcH = 0.05f;
    a.arcPeak = 0.45f;
    a.rot.keys.clear();
    a.rot.add(0.0f, from.q);
    a.rot.add(0.80f, qs);
    a.fing.keys.clear();
    a.fing.add(0.0f, from.f);
    if (t0 > 0.0f) a.fing.add(0.20f, letGo);   // off the pen
    a.fing.add(0.60f, fo);
    clearPath(a, from, fpLerp(from.f, fo, 0.5f), tableC, R);
    mo.segs.push_back(a);
    s = a.sample(a.T);
    // 2. slide in and close the grip (clasp event at the end)
    Segment b = makeSeg(s, t2 - t1, pClasp, vec3(0), qs, fg);
    b.fing.keys.clear();
    b.fing.add(0.0f, s.f);
    b.fing.add(0.15f, s.f);
    b.fing.add(1.0f, fg);
    mo.segs.push_back(b);
    s = b.sample(b.T);
    // 3. two pumps (both animators compute the same vertical motion)
    Segment c = makeSeg(s, t3 - t2, pClasp, vec3(0), qs, fg);
    c.oscAmp = 0.032f;
    c.oscCycles = 2.0f;
    c.os = 0.04f;
    c.oe = 0.92f;
    mo.segs.push_back(c);
    s = c.sample(c.T);
    // 4. release: fingers open, hand slides back a little
    Segment d = makeSeg(s, t4 - t3, pClasp - fingerDir * 0.02f - palmN * 0.006f, vec3(0), qs, fpLerp(fg, fo, 0.85f));
    mo.segs.push_back(d);
    s = d.sample(d.T);
    // 5. back to rest
    HandSample r = h.rest;
    Segment e = makeSeg(s, T - t4, r.p, vec3(0), r.q, r.f);
    e.arcH = 0.03f;
    e.arcPeak = 0.35f;
    e.rot.keys.clear();
    e.rot.add(0.0f, s.q);
    e.rot.add(0.80f, r.q);
    clearPath(e, s, poseRelaxed(), tableC, R);
    e.fing.keys.clear();
    e.fing.add(0.0f, s.f);
    e.fing.add(0.4f, poseRelaxed());
    e.fing.add(1.0f, r.f);
    mo.segs.push_back(e);
    curEvents.push_back({start + t2, EventType::HandshakeClasp, ActNone, false});
    curEvents.push_back({start + t3, EventType::HandshakeRelease, ActNone, false});
    curTargetWorld = cW;
}

// Resting spots clear of the pieces on the table, including the one the current task is about to
// set down there: the idle left hand makes room before the right hand arrives.
void Animator::Impl::validateRests(const Task* t) {
    pending.clear();
    const Hand& r = right();
    if (t && t->type == TaskType::Discard && r.capId >= 0) pending.push_back(knownPiece(r.capId, t->position));
    if (t && t->type == TaskType::Place && r.heldId >= 0 && t->position.y < layout::BOARD_TOP_Y - 0.01f) pending.push_back(knownPiece(r.heldId, t->position));
    if (!knowsAnyPiece()) return;
    for (Hand& h : hands) {
        HandSample want = safeRest(h.side, h.restContact);
        if (length(want.p - h.rest.p) < 0.004f) continue;
        h.rest = want;
        // The idle hands make room (the right one only between tasks: a task moves it anyway).
        const bool slide = h.side == Side::Left ? !h.chinFollow && leftChin == 0 && writingHandFree()
                                                : !t && rightIdle && h.heldId < 0 && h.capId < 0 && !h.chinFollow;
        if (slide) {
            HandSample from = h.motion.sample(time);
            Motion mo;
            mo.start = time;
            Segment sg = makeSeg(from, 0.45f, want.p, vec3(0), want.q, want.f);
            sg.arcH = 0.012f;   // lifts a little while it slides
            clearPath(sg, from, want.f, layout::TABLE_TOP_Y - pelvisWorld.y, h.side);
            mo.segs.push_back(sg);
            h.motion = mo;
        }
    }
    pending.clear();
}

// Between its end rotations a transport's hand rotation is free: where the interpolated rotation
// would bend the wrist beyond its limits (long cross-body moves), bend the rotation path towards
// what the forearm naturally gives.
void Animator::Impl::relaxWrist(Hand& h) {
    for (auto& sg : h.motion.segs) {
        // (A procedural segment ignores rotCorr; a hold with the fingertip pinned must keep its
        // rotation, or the pinned tip would move with it.)
        if (sg.T < 0.12f || sg.usePivot || sg.follow || sg.pinFrom >= 1.0f) continue;
        auto worst = [&](float& atU) {
            float w = 0.0f;
            atU = -1.0f;
            for (int k = 0; k <= 14; ++k) {
                float u = 0.15f + 0.05f * float(k);
                HandSample s = sg.sample(u * sg.T);
                float st = armStrain(s.p, s.q);
                if (st > w) { w = st; atU = u; }
            }
            return w;
        };
        float u0;
        float w0 = worst(u0);
        if (w0 < 0.04f || u0 < 0.0f) continue;
        HandSample s = sg.sample(u0 * sg.T);
        armStrain(s.p, s.q);                      // leaves the solved arm in G
        quat achieved = rotOf(G[HandR]), neutral = rotOf(G[ForeArmR]);
        quat key = qslerp(achieved, neutral, 0.35f);
        quat base = s.q;
        sg.rotCorr = normalize(key * conjugate(base));
        if (sg.rotCorr.w < 0) sg.rotCorr = quat(-sg.rotCorr.x, -sg.rotCorr.y, -sg.rotCorr.z, -sg.rotCorr.w);
        float uEnd = sg.rot.keys.empty() ? 1.0f : std::max(0.3f, sg.rot.keys.back().u);
        if (u0 >= uEnd - 0.05f) continue;         // strained where the end rotation is required
        sg.corrPeak = clamp(u0 / uEnd, 0.2f, 0.8f);
        float u1;
        float w1 = worst(u1);
        if (w1 > w0) sg.rotCorr = quat();
        if (debugLog) LOGI("anim: wrist relax: strain %.2f -> %.2f rad (at u=%.2f)", w0, std::min(w0, w1), u0);
    }
}

// The planned arcs only look at the hand; the forearm sweeps over the pieces too (reaching across
// one's own back rank, discarding to the far side). Samples the planned motion and raises a
// segment's arc until the forearm stays clear of the pieces under it.
void Animator::Impl::liftForearm(Hand& h) {
    if (!knowsPieces()) return;   // needs to know where the pieces are
    Pose tmp;
    mat4 W[BoneCount];
    const float radius = 0.030f;
    float segStart = h.motion.start;
    // The piece being reached for is not an obstacle (its neighbours are).
    const int ignoreId = cur.type == TaskType::Reach || cur.type == TaskType::TakeCaptured ? cur.pieceId : h.heldId;
    // The fingers start on a piece they have just let go: not an obstacle while they leave it.
    const float relT = h.releasedId >= 0 ? segStart + 0.35f * (h.motion.segs.empty() ? 0.0f : h.motion.segs.front().T) : -1.0f;
    auto deficit = [&](float t, bool withHand) {   // how far the arm dips into the space of the pieces
        evaluate(t, tmp, W);
        vec3 e = W[ForeArmR].translation(), w = W[HandR].translation();
        float d = 0.0f;
        for (int j = 0; j <= 8; ++j) {
            vec3 p = e + (w - e) * (0.35f + 0.65f * float(j) / 8.0f);
            float top = topNear(p, 0.028f, ignoreId);
            if (top <= layout::BOARD_TOP_Y + 1e-3f) continue;   // no piece there
            d = std::max(d, top + radius - p.y);
        }
        if (withHand)
            for (int f = 0; f < 5; ++f)
                for (int j = 0; j < 4; ++j) {
                    Bone b = fingerBone(Side::Right, f, std::min(j, 2));
                    vec3 p = j < 3 ? W[b].translation() : transformPoint(W[b], boneDir(*sk, b) * sk->boneLength[b]);
                    float rad = j == 0 ? 0.011f : 0.008f;
                    float q = p.y - layout::BOARD_TOP_Y > 0.012f ? std::max(0.001f, rad - 0.006f) : rad;
                    float top = topNear(p, q, t < relT && ignoreId < 0 ? h.releasedId : ignoreId);
                    if (top <= layout::BOARD_TOP_Y + 1e-3f) continue;
                    d = std::max(d, top + 0.003f - (p.y - rad));
                }
        return d;
    };
    for (size_t i = 0; i < h.motion.segs.size(); ++i) {
        Segment& sg = h.motion.segs[i];
        if (sg.follow) {   // procedural (a Trace): its plan keeps its own clearance (finishTracePlan)
            segStart += sg.T;
            continue;
        }
        // Where the segment ends (hand on a piece, on the clock...): raise the elbow.
        float d = deficit(segStart + sg.T, false);
        const float clamp0 = wristClamp + pronClamp;
        for (int iter = 0; iter < 6 && d >= 0.0005f && sg.elbow1 < 0.9f; ++iter) {
            float prev = sg.elbow1;
            sg.elbow1 = std::min(0.9f, sg.elbow1 + std::max(0.08f, d * 6.0f));
            if (i + 1 < h.motion.segs.size()) h.motion.segs[i + 1].elbow0 = sg.elbow1;
            d = deficit(segStart + sg.T, false);
            if (debugLog) LOGI("anim: elbow lift %.2f rad (segment %d end, remaining %.1f mm)", sg.elbow1, int(i), d * 1000.0f);
            if (wristClamp + pronClamp > clamp0 + 0.03f) {   // the hand could no longer hold its orientation
                sg.elbow1 = prev;
                if (i + 1 < h.motion.segs.size()) h.motion.segs[i + 1].elbow0 = prev;
                break;
            }
        }
        // On the way: a higher arc. Only where the arc actually lifts the hand (not in the final
        // descent onto a piece, which the grip choice and the elbow handle), within a sane budget,
        // and only while raising it helps.
        auto worstOnWay = [&]() {
            float worst = 0.0f;
            for (int k = 0; k <= 14; ++k) {
                float u = 0.15f + 0.05f * float(k);   // 0.15 .. 0.85
                float b = bump(u, sg.arcPeak);
                if (b < 0.30f) continue;
                float d = deficit(segStart + u * sg.T, true);
                worst = std::max(worst, d / b);
            }
            return worst;
        };
        float added = 0.0f;
        float worst = sg.T > 0.05f ? worstOnWay() : 0.0f;
        for (int iter = 0; iter < 4 && worst >= 0.002f; ++iter) {
            float step = std::min(worst * 1.05f + 0.003f, 0.08f - added);
            if (step < 0.001f) break;
            sg.arcH += step;
            float after = worstOnWay();
            if (after > worst * 0.75f && after > 0.004f) {   // does not help: the arm cannot go around it this way
                sg.arcH -= step;
                if (debugLog) LOGI("anim: forearm clearance: segment %d arc +%.1f mm does not help (%.1f -> %.1f mm)", int(i), step * 1000.0f,
                                   worst * 1000.0f, after * 1000.0f);
                break;
            }
            added += step;
            if (debugLog) LOGI("anim: forearm clearance: segment %d arc +%.1f mm (remaining %.1f mm)", int(i), step * 1000.0f, after * 1000.0f);
            worst = after;
        }
        segStart += sg.T;
    }
}

// =============================================================================================
// Pose evaluation
// =============================================================================================
vec3 Animator::Impl::headPointWorld() const { return toWorld(transformPoint(G[Head], vec3(0, 0.08f, 0.07f))); }

void Animator::Impl::evaluate(float t, Pose& pose, mat4* worldOut) {
    for (int i = 0; i < BoneCount; ++i) pose.local[i] = quat();
    pose.rootPosition = pelvisWorld;
    pose.rootRotation = rootQ;
    reachShort = 0;
    wristClamp = 0;
    pronClamp = 0;

    // Each hand's motion is sampled once (a procedural one, e.g. a pen path, is costly): hr and
    // hlMotion stay as sampled until handTarget() below.
    HandSample hr = right().motion.sample(t);
    const HandSample hlMotion = left().motion.sample(t);

    // ---- idle life (breathing always; sway and micro motion only when not the player camera)
    float br = std::sin(t * TAU / 4.2f + seed * 3.0f);
    float breath = 0.0035f * br;
    float sway = headOverride ? 0.0f : 1.0f;
    float idleFlex = breath + sway * 0.012f * (std::sin(t * 0.31f + seed * 5.0f) * 0.6f + std::sin(t * 0.17f + 1.3f) * 0.4f);
    float idleTwist = sway * 0.018f * (std::sin(t * 0.23f + seed * 7.0f) * 0.7f + std::sin(t * 0.41f) * 0.3f);
    float idleSide = sway * 0.012f * std::sin(t * 0.19f + seed * 2.0f);

    // Forward bend of the thinking poses and of setLean (at most 0.2 rad from the hips); speaking
    // leans in a little more, a touch further on the stressed syllables.
    const float flex = thinkLean + 0.2f * lean + 0.022f * speechEnv + 0.02f * speechStress;
    SpineParams sp = solveSpine(pose, hr.p, flex, idleFlex, idleTwist, idleSide);
    if (mirrored && running && cur.type == TaskType::Handshake) {
        // Left-handed player shaking hands with the solver's left hand: the torso follows that
        // hand the way it follows the right one (mirror image of the solve), blended in and out.
        const float u = t - curStart, w = smoothstep(0.0f, 0.3f, u) * (1.0f - smoothstep(curT - 0.3f, curT, u));
        if (w > 0.0f) {
            SpineParams sl = solveSpine(pose, mirrorX(hlMotion.p), flex, idleFlex, -idleTwist, -idleSide);
            sp.flex = lerp(sp.flex, sl.flex, w);
            sp.twist = lerp(sp.twist, -sl.twist, w);
            sp.side = lerp(sp.side, -sl.side, w);
        }
    }
    // The writing hand at work: lean/turn a little towards the sheet, and keep it within reach
    // whatever the playing hand does.
    writingSpine(sp, hlMotion);
    spineOut = sp;
    applySpine(pose, sp);
    fkChain(pose, Pelvis, Spine2);

    // ---- head / neck
    quat chest = rotOf(G[Spine2]);
    quat desired;
    if (headOverride) {
        desired = qy(ovYaw) * qx(-ovPitch);
    } else {
        // (Nods and head shakes on top of the smoothed gaze angles.)
        desired = qy(headYaw + gestYaw) * qx(-(headPitch + gestPitch));
    }
    quat Q = normalize(conjugate(chest) * desired);
    quat neck = qslerp(quat(), Q, 0.30f);
    pose.local[Neck] = neck;
    pose.local[Head] = normalize(conjugate(neck) * Q);
    fkChain(pose, Neck, Head);

    // ---- eyes and lids
    if (headOverride) {
        pose.local[EyeL] = pose.local[EyeR] = quat();
        eyeYaw = eyePitch = 0.0f;
    } else {
        float w = sacDur > 0 ? clamp(sacT / sacDur, 0.0f, 1.0f) : 1.0f;
        vec3 fixW = lerp(fixFrom, fixTo, minJerk(w)) + microOffset;
        vec3 fixC = toChar(fixW);
        float yawSum = 0, pitchSum = 0;
        for (int e = 0; e < 2; ++e) {
            Bone eb = e == 0 ? EyeL : EyeR;
            mat4 eg = G[Head] * toMat4(quat(), sk->restOffset[eb]);
            vec3 dl = rotate(conjugate(rotOf(G[Head])), fixC - eg.translation());
            float yaw = std::atan2(dl.x, std::max(1e-3f, dl.z));
            float pitch = std::atan2(dl.y, length(vec3(dl.x, 0, dl.z)));
            yaw = clamp(yaw, -0.62f, 0.62f);
            pitch = clamp(pitch, -0.55f, 0.42f);
            pose.local[eb] = qy(yaw) * qx(-pitch);
            yawSum += yaw;
            pitchSum += pitch;
        }
        eyeYaw = yawSum * 0.5f;
        eyePitch = pitchSum * 0.5f;
    }
    float blink = 0.0f;
    if (blinkPhase >= 0.0f) {
        float b = blinkPhase;
        blink = b < 0.38f ? minJerk(b / 0.38f) : 1.0f - minJerk((b - 0.38f) / 0.62f);
    }
    const float upperClosed = 0.95f, lowerClosed = -0.28f;
    // (Speaking: the upper lids open a little wider, more on the stressed syllables.)
    float upOpen = clamp(-eyePitch * 0.80f - 0.05f * speechEnv - 0.30f * speechStress, -0.30f, 0.55f),
          loOpen = clamp(-eyePitch * 0.35f, -0.25f, 0.20f);
    float up = lerp(upOpen, upperClosed, blink), lo = lerp(loOpen, lowerClosed, blink * 0.6f);
    pose.local[LidUpperL] = pose.local[LidUpperR] = qx(up);
    pose.local[LidLowerL] = pose.local[LidLowerR] = qx(lo);

    // ---- arms (chin poses follow the head computed above)
    hr = handTarget(right(), t, hr);
    HandSample hl = handTarget(left(), t, hlMotion);
    solveArm(pose, Side::Right, hr.p, hr.q, hr.elbow);
    if (hr.pinW > 0.0f) {
        // Point lock (the pointing fingertip): where the wrist clamps the hand's rotation, the whole
        // hand shifts so the index tip stays on its planned point (weighted in and out by pinW).
        const vec3 planned = hr.p + rotate(hr.q, hr.pinLocal);
        vec3 corr(0.0f);
        for (int it = 0; it < 3; ++it) {
            const vec3 e = planned - transformPoint(G[HandR], hr.pinLocal);
            if (length2(e) < 1e-8f) break;
            corr += e;
            solveArm(pose, Side::Right, hr.p + corr, hr.q, hr.elbow);
        }
        if (hr.pinW < 1.0f && length2(corr) > 0.0f) solveArm(pose, Side::Right, hr.p + corr * hr.pinW, hr.q, hr.elbow);
    }
    static const bool armTrace = std::getenv("SCACELITH_ANIM_ARMTRACE") != nullptr;   // joint-limit diagnostics
    if (armTrace && (wristClamp > 0.05f || pronClamp > 0.05f))
        LOGI("armtrace %s t=%.4f pron %.3f flex %.3f dev %.3f elbow %.3f clampP %.3f clampW %.3f p %.3f %.3f %.3f", facing > 0 ? "White" : "Black", t, lastPron, lastFlex, lastDev, hr.elbow,
             pronClamp, wristClamp, hr.p.x, hr.p.y, hr.p.z);
    applyFingers(*sk, pose, Side::Right, hr.f);
    // (Debug: the writing arm's joint-limit diagnostics, the playing arm's are kept.)
    const float keepDiag[6] = {reachShort, wristClamp, pronClamp, lastFlex, lastDev, lastPron};
    if (debugLog) {
        diagSide = Side::Left;
        reachShort = wristClamp = pronClamp = 0.0f;
    }
    solveArm(pose, Side::Left, hl.p, hl.q, hl.elbow);
    if (hl.pinW > 0.0f) {
        // Point lock (the page pinch): where the wrist clamps the hand's rotation, the whole hand
        // shifts so the pinched point stays on its planned path (weighted in and out by pinW).
        const vec3 planned = hl.p + rotate(hl.q, hl.pinLocal);
        vec3 corr(0.0f);
        for (int it = 0; it < 3; ++it) {
            const vec3 e = planned - transformPoint(G[HandL], hl.pinLocal);
            if (length2(e) < 1e-8f) break;
            corr += e;
            solveArm(pose, Side::Left, hl.p + corr, hl.q, hl.elbow);
        }
        if (hl.pinW < 1.0f && length2(corr) > 0.0f) solveArm(pose, Side::Left, hl.p + corr * hl.pinW, hl.q, hl.elbow);
    }
    const float leftDiag[6] = {reachShort, wristClamp, pronClamp, lastFlex, lastDev, lastPron};
    if (debugLog) {
        diagSide = Side::Right;
        reachShort = keepDiag[0];
        wristClamp = keepDiag[1];
        pronClamp = keepDiag[2];
        lastFlex = keepDiag[3];
        lastDev = keepDiag[4];
        lastPron = keepDiag[5];
    }
    applyFingers(*sk, pose, Side::Left, hl.f);
    // Pen in the writing hand. While a path is followed the tip must be exactly on it: the pen
    // slides in the fingers by whatever the arm solve missed (sub-millimetre when within reach).
    evalPen = hl.pen;
    wr.follow = hl.tipLock;
    if (debugLog && wr.running && wr.cur.type == WriteTaskType::TurnPage && (leftDiag[0] > 1e-3f || leftDiag[1] > 0.02f || leftDiag[2] > 0.02f))
        LOGI("anim: t=%.3f page turn: writing arm short %.1f mm, wrist clamp %.1f deg (flex %.0f dev %.0f deg), pronation %.0f deg clamped by %.1f deg", t,
             leftDiag[0] * 1000.0f, leftDiag[1] / DEG, leftDiag[3] / DEG, leftDiag[4] / DEG, leftDiag[5] / DEG, leftDiag[2] / DEG);
    if (hl.tipLock) {
        evalPen.p = transformPoint(inverseAffine(G[HandL]), hl.tip);
        const float slide = length(evalPen.p - hl.pen.p);
        if (debugLog && slide > 0.002f)
            LOGI("anim: t=%.3f pen slides %.1f mm in the fingers (writing arm: short %.1f mm, wrist clamp %.1f deg, flex %.0f dev %.0f deg, "
                 "pronation %.0f deg clamped by %.1f deg)",
                 t, slide * 1000.0f, leftDiag[0] * 1000.0f, leftDiag[1] / DEG, leftDiag[3] / DEG, leftDiag[4] / DEG, leftDiag[5] / DEG, leftDiag[2] / DEG);
    }
    if (hl.lockW > 0.0f) {
        // Taken from / laid on the table: exactly on its table frame (blended in or out).
        PenPose onTable{normalize(conjugate(rotOf(G[HandL])) * hl.lockC.q), transformPoint(inverseAffine(G[HandL]), hl.lockC.p)};
        evalPen = penLerp(evalPen, onTable, clamp(hl.lockW, 0.0f, 1.0f));
    }

    if (worldOut) computeGlobal(*sk, pose, worldOut);
}

// =============================================================================================
// Gaze, blinks, idle
// =============================================================================================
void Animator::Impl::updateGaze(float dt) {
    gazeWeight += (gazeWeightTarget - gazeWeight) * (1.0f - std::exp(-dt * 6.0f));
    // Default gaze: the board, wandering slowly over the squares (AI) / straight (player).
    float t = time;
    vec3 wander(0.08f * std::sin(t * 0.37f + seed * 4.0f), 0, 0.06f * std::sin(t * 0.29f + seed * 9.0f));
    vec3 idleTarget = vec3(0, layout::BOARD_TOP_Y + 0.02f, 0) + wander;
    // During a handshake look at the partner's face.
    vec3 target = lerp(idleTarget, gazeTarget, gazeWeight);
    // While the hand works, the eyes (and head) lead it to the task target.
    bool taskLook = running && cur.type != TaskType::Handshake && cur.type != TaskType::Wait && cur.type != TaskType::Retract;
    if (running && isGesture(cur.type)) {
        // Gestures: on the target (a Trace: the eyes pursue the fingertip) until gazeHold has run
        // out after the arrival, then back to the lookAt target while the hand keeps pointing. A
        // Beat leaves the eyes to lookAt.
        if (!curLook || (cur.gazeHold >= 0.0f && time > curStart + curArrive + cur.gazeHold)) taskLook = false;
        if (cur.type == TaskType::Trace && trace) curTargetWorld = toWorld(trace->tipAt(time - traceStart));
    }
    taskGaze += ((taskLook ? 1.0f : 0.0f) - taskGaze) * (1.0f - std::exp(-dt * (taskLook ? 14.0f : 2.0f)));
    target = lerp(target, curTargetWorld, taskGaze);
    // The eyes follow the pen (or the page corner) while the writing hand works and the playing
    // hand has nothing to do.
    if (wr.look > 1e-3f) {
        vec3 wt;
        if (wr.running && wr.cur.type == WriteTaskType::TurnPage && wr.corner && wr.turnT > 0.0f)
            wt = wr.corner(pageTurnEase(clamp((time - wr.turnStart) / wr.turnT, 0.0f, 1.0f)));
        else if (wr.penHeld) wt = transformPoint(worldI[HandL], evalPen.p);
        else if (wr.running && (wr.cur.type == WriteTaskType::PickPen || wr.cur.type == WriteTaskType::PutPen)) wt = wr.cur.frame.translation();
        else wt = toWorld(left().motion.sample(time).p);
        target = lerp(target, wt, wr.look * (1.0f - taskGaze));
    }
    float shakeT = time - shakeStart;
    if (partner && partner->impl_ && shakeT >= 0.0f && shakeT < Timing::Handshake) {
        vec3 face = partnerPoint(partner->impl_->headPointWorld());
        float w = smoothstep(0.0f, 0.25f, shakeT) * (1.0f - smoothstep(Timing::Handshake - 0.5f, Timing::Handshake - 0.1f, shakeT));
        // Glance at the hands right before the clasp.
        float gl = smoothstep(0.45f, 0.62f, shakeT) * (1.0f - smoothstep(0.82f, 1.02f, shakeT));
        vec3 hands = toWorld(shakeHand().motion.sample(time).p);
        target = lerp(target, lerp(face, hands, gl * 0.8f), w);
    }
    vec3 headW = toWorld(transformPoint(G[Neck], vec3(0, 0.12f, 0.05f)));
    vec3 dc = rotate(conjugate(rootQ), target - headW);
    float yaw = std::atan2(dc.x, std::max(1e-3f, dc.z));
    float pitch = std::atan2(dc.y, length(vec3(dc.x, 0, dc.z)));
    // Head takes most of large rotations, the eyes the rest.
    float hy = yaw * (0.55f + 0.30f * smoothstep(0.0f, 0.6f, std::fabs(yaw)));
    float hp = pitch * 0.62f;
    // Micro head motion (AI only).
    hy += 0.008f * std::sin(t * 0.9f + seed) + 0.004f * std::sin(t * 2.3f);
    hp += 0.007f * std::sin(t * 0.7f + seed * 2.0f) + 0.003f * std::sin(t * 1.9f);
    // Nod at the clasp.
    if (shakeT > 0.85f && shakeT < 1.6f) hp -= 0.10f * std::sin(PI * (shakeT - 0.85f) / 0.75f);
    // Speaking: the head bobs with the syllables and dips on the stressed ones (nods and head
    // shakes are added after the spring, see gestYaw).
    updateSpeech(dt, hy, hp);
    hy = clamp(hy, -70.0f * DEG, 70.0f * DEG);
    hp = clamp(hp, -45.0f * DEG, 30.0f * DEG);
    // Critically damped spring towards the target angles (sub-stepped).
    const float w0 = 11.0f;
    int n = std::max(1, int(std::ceil(dt / (1.0f / 240.0f))));
    float h = dt / float(n);
    for (int i = 0; i < n; ++i) {
        float ay = w0 * w0 * (hy - headYaw) - 2.0f * w0 * headYawV;
        float ap = w0 * w0 * (hp - headPitch) - 2.0f * w0 * headPitchV;
        headYawV += ay * h;
        headPitchV += ap * h;
        headYaw += headYawV * h;
        headPitch += headPitchV * h;
    }
    // Eyes: saccade to a new fixation when the target moved by more than ~1.5 degrees, otherwise
    // pursue it smoothly; micro-saccades every 0.5-1.6 s.
    sacT += dt;
    vec3 cur = lerp(fixFrom, fixTo, minJerk(clamp(sacT / sacDur, 0.0f, 1.0f)));
    vec3 a = normalize(cur - headW), b = normalize(target - headW);
    float ang = std::acos(clamp(dot(a, b), -1.0f, 1.0f));
    if (ang > 1.5f * DEG && sacT >= sacDur) {
        fixFrom = cur;
        fixTo = target;
        sacT = 0.0f;
        sacDur = clamp(0.022f + 0.0024f * (ang / DEG), 0.03f, 0.11f);
        if (ang > 18.0f * DEG && blinkPhase < 0.0f && rng.uniform() < 0.35f) blinkPhase = 0.0f;
    } else if (sacT >= sacDur) {
        fixFrom = fixTo = target;  // smooth pursuit
    }
    microTimer -= dt;
    if (microTimer <= 0.0f) {
        microTimer = rng.range(0.5f, 1.6f);
        float dist = length(target - headW);
        microOffset = vec3(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)) * (dist * 0.0045f);
    }
    // Blinks every 2-6 s.
    if (blinkPhase >= 0.0f) {
        blinkPhase += dt / 0.17f;
        if (blinkPhase >= 1.0f) blinkPhase = -1.0f;
    } else {
        blinkTimer -= dt;
        if (blinkTimer <= 0.0f) {
            blinkPhase = 0.0f;
            blinkTimer = rng.range(2.0f, 6.0f);
        }
    }
}


// Chin-on-hand target (loose fist, knuckles under the chin) from the current head pose.
HandSample Animator::Impl::chinTarget(Side s) const {
    HandSample h;
    float sx = sideX(s), ps = palmSign(s);
    h.f = fpHumanize(poseLooseFist(), s == Side::Right ? 0.1f : 0.8f, 0.04f);
    vec3 fingerDir = normalize(vec3(-sx * 0.10f, 1.0f, 0.30f));              // up, a bit forward
    vec3 palmDir = normalize(perp(vec3(-sx * 0.40f, 0.0f, -1.0f), fingerDir));  // towards the face
    vec3 X = palmDir * ps, Y = -fingerDir, Z = cross(X, Y);
    h.q = fromMat3(mat3(X, Y, Z));
    mat4 fi[3], fm[3];
    fingerFrames(*sk, s, h.f, Index, fi);
    fingerFrames(*sk, s, h.f, Middle, fm);
    vec3 knuckle = (fi[1].translation() + fm[1].translation()) * 0.5f + vec3(0, -0.009f, 0);
    vec3 chin = transformPoint(G[Head], vec3(sx * 0.012f, -0.028f, 0.062f));
    h.p = wristFor(chin, h.q, knuckle);
    return h;
}

// Hand target at time t: the motion (s = h.motion.sample(t)), plus the live chin-follow offset of
// an idle chin pose.
HandSample Animator::Impl::handTarget(const Hand& h, float t, HandSample s) const {
    if (h.chinFollow) {
        HandSample live = chinTarget(h.side);
        float d = std::max(1e-3f, h.motion.duration());
        float w = minJerk(clamp((t - h.motion.start) / d, 0.0f, 1.0f));
        s.p = s.p + (live.p - h.chinPlanned.p) * w;
        quat corr = normalize(live.q * conjugate(h.chinPlanned.q));
        s.q = qslerp(s.q, normalize(corr * s.q), w);
    }
    return s;
}

// Replaces a chin-following idle motion by a plain hold at the current target.
void Animator::Impl::bakeFollow(Hand& h) {
    if (!h.chinFollow) return;
    HandSample s = handTarget(h, time);
    s.v = vec3(0);
    s.a = vec3(0);
    Motion mo;
    mo.start = time;
    mo.segs.push_back(makeSeg(s, 1e-3f, s.p, vec3(0), s.q, s.f));
    h.motion = mo;
    h.chinFollow = false;
}

void Animator::Impl::updateIdle(float dt) {
    Hand& R = right();
    Hand& L = left();
    bool rightFree = !running && queue.empty() && R.heldId < 0 && R.capId < 0 && rightIdle;
    int desired = 0;
    if (thinking) {
        thinkTimer -= dt;
        if (thinkTimer <= 0.0f) {
            float r = rng.uniform();
            thinkPose = r < 0.40f ? 1 : (r < 0.72f ? 2 : 0);
            thinkTimer = rng.range(5.0f, 11.0f);
        }
        desired = thinkPose;
        if (desired == 2 && !rightFree) desired = 0;
    }
    thinkLeanTarget = desired == 0 ? 0.0f : 0.16f;
    thinkLean += (thinkLeanTarget - thinkLean) * (1.0f - std::exp(-dt * 1.8f));
    lean += (leanTarget - lean) * (1.0f - std::exp(-dt * 4.0f));

    auto drive = [&](Hand& hand, bool want, int& state) {
        if (int(want) == state) return;
        state = int(want);
        bakeFollow(hand);
        HandSample from = hand.motion.sample(time);
        HandSample to = want ? chinTarget(hand.side) : hand.rest;
        Motion mo;
        mo.start = time;
        Segment sg = makeSeg(from, want ? 1.30f : 1.10f, to.p, vec3(0), to.q, to.f);
        sg.arcH = 0.03f;
        sg.arcPeak = 0.45f;
        sg.rot.keys.clear();
        sg.rot.add(0.0f, from.q);
        sg.rot.add(0.9f, to.q);
        snapshotPieces();
        clearPath(sg, from, fpLerp(from.f, to.f, 0.5f), layout::TABLE_TOP_Y - pelvisWorld.y, hand.side);
        mo.segs.push_back(sg);
        hand.motion = mo;
        hand.chinFollow = want;
        hand.chinPlanned = to;
    };
    // The writing hand only goes to the chin with nothing to do and no pen in it.
    drive(L, desired == 1 && writingHandFree(), leftChin);
    if (rightFree) drive(R, desired == 2, rightChin);
}

// =============================================================================================
// Task machine
// =============================================================================================
void Animator::Impl::startTask(const Task& t, std::vector<Event>& ev) {
    Hand& h = right();
    bakeFollow(h);
    rightChin = 0;
    if (mirrored && t.type == TaskType::Handshake) {
        // The handshake takes the writing hand: whatever it does stops here, its queue waits.
        bakeFollow(left());
        leftChin = 0;
        interruptWriting(ev);
        wr.suspendUntil = time + taskDuration(t);
    }
    cur = t;
    running = true;
    curStart = time;
    curT = taskDuration(t);
    curArrive = curT;   // (gestures: set by their plan)
    curLook = true;
    trace.reset();
    auto c0 = std::chrono::steady_clock::now();
    planTask(t, time, curT);
    float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - c0).count();
    planMsMax = std::max(planMsMax, ms);
    if (debugLog) LOGI("anim: task %d planned in %.2f ms", int(t.type), ms);
    Event e;
    e.type = EventType::TaskStarted;
    e.pieceId = t.pieceId;
    e.position = t.position;
    e.time = time;
    e.tag = t.tag;
    ev.push_back(e);
}

// Upright transform of a piece standing at 'pos' with the yaw of m.
static mat4 uprightAt(const mat4& mm, vec3 pos) {
    vec3 f = transformDir(mm, vec3(0, 0, 1));
    float yaw = std::atan2(f.x, f.z);
    return translate(pos) * rotateY(yaw);
}

// Where the hand actually put the piece versus the requested spot (IK limits, clamps).
void Animator::Impl::checkRelease(const mat4& actual, vec3 wanted, int id) {
    float off = length(actual.translation() - wanted);
    float tilt = std::acos(clamp(transformDir(actual, vec3(0, 1, 0)).y, -1.0f, 1.0f));
    lastReleaseError = off;
    lastReleaseTilt = tilt;
    if (off > 0.004f || tilt > 0.06f || debugLog)
        LOGW("anim: piece %d released %.1f mm / %.1f deg away from the requested placement (reach short %.1f mm, wrist clamp %.1f deg "
             "flex %.0f dev %.0f, pronation clamp %.1f deg)",
             id, off * 1000.0f, tilt / DEG, reachShort * 1000.0f, wristClamp / DEG, lastFlex / DEG, lastDev / DEG, pronClamp / DEG);
}

void Animator::Impl::fireDue(float upTo, std::vector<Event>& ev) {
    for (auto& e : curEvents) {
        if (e.done || e.t > upTo + 1e-6f) continue;
        e.done = true;
        Event out;
        out.type = e.type;
        out.time = e.t;
        out.pieceId = cur.pieceId;
        out.tag = cur.tag;
        out.position = cur.type == TaskType::Handshake || isGesture(cur.type) ? curTargetWorld : cur.position;
        if (e.hasPos) out.position = e.pos;
        if (e.action == ActPutPen) {
            // The handshake laid the writing hand's pen down.
            wr.penHeld = false;
            wr.penTable = shakePutFrame;
            wr.hasPenTable = true;
            out.pieceId = -1;
            out.transform = shakePutFrame;
            out.position = shakePutFrame.translation();
            ev.push_back(out);
            if (shakeTookPut && wr.queue.empty() && !wr.running) {
                Event q;
                q.type = EventType::WritingQueueEmpty;
                q.time = e.t;
                ev.push_back(q);
            }
            continue;
        }
        if (e.action != ActNone) {
            Pose tmp;
            mat4 W[BoneCount];
            evaluate(e.t, tmp, W);
            mat4 hand = W[HandR];
            quat handC = rotOf(G[HandR]);
            Hand& h = right();
            switch (e.action) {
                case ActGripPrimary: {
                    mat4 pw = pieceWorld(cur.pieceId);
                    forgetTable(cur.pieceId);
                    h.heldId = cur.pieceId;
                    h.heldAttach = inverseAffine(hand) * pw;
                    h.gripQ = handC;
                    h.carryQ = handC;
                    out.transform = pw;
                    out.position = pw.translation();
                    break;
                }
                case ActReleasePrimary: {
                    out.pieceId = h.heldId;
                    out.transform = uprightAt(hand * h.heldAttach, cur.position);
                    checkRelease(hand * h.heldAttach, cur.position, h.heldId);
                    noteReleased(h.heldId, out.transform);
                    h.releasedId = h.heldId;
                    h.heldId = -1;
                    break;
                }
                case ActGripCaptured: {
                    mat4 pw = pieceWorld(cur.pieceId);
                    forgetTable(cur.pieceId);
                    h.capId = cur.pieceId;
                    h.capAttach = inverseAffine(hand) * pw;
                    h.capQ = handC;
                    out.transform = pw;
                    out.position = pw.translation();
                    break;
                }
                case ActReleaseCaptured: {
                    out.pieceId = h.capId;
                    out.transform = uprightAt(hand * h.capAttach, cur.position);
                    checkRelease(hand * h.capAttach, cur.position, h.capId);
                    noteReleased(h.capId, out.transform);
                    h.releasedId = h.capId;
                    h.capId = -1;
                    break;
                }
                default: break;
            }
        }
        ev.push_back(out);
    }
}

void Animator::Impl::finishTask(std::vector<Event>& ev) {
    fireDue(curStart + curT, ev);
    running = false;
    prevType = cur.type;
    Hand& h = right();
    if ((cur.type == TaskType::Retract || cur.type == TaskType::Handshake) && h.heldId < 0 && h.capId < 0) rightIdle = true;
    if (queue.empty()) {
        Event e;
        e.type = EventType::QueueEmpty;
        e.time = time;
        e.tag = cur.tag;
        ev.push_back(e);
    }
}

// =============================================================================================
// Public API
// =============================================================================================
Animator::Animator() : impl_(std::make_shared<Impl>()) {}

void Animator::init(const Skeleton& sk, vec3 pelvisWorld, float facing, Side playHand) {
    impl_ = std::make_shared<Impl>();
    Impl& I = *impl_;
    I.owner = this;
    I.sk = &sk;
    I.mirrored = playHand == Side::Left;   // left-handed: the solver runs in the mirrored world
    I.pelvisWorld = I.mw(pelvisWorld);
    I.facing = facing >= 0.0f ? 1.0f : -1.0f;
    I.rootQ = I.facing > 0 ? axisAngle(vec3(0, 1, 0), PI) : quat();
    I.root = toMat4(I.rootQ, pelvisWorld);
    I.invRoot = inverseAffine(I.root);
    I.L1 = length(sk.restOffset[ForeArmR]);
    I.L2 = length(sk.restOffset[HandR]);
    I.seed = I.facing > 0 ? 0.37f : 0.81f;
    I.rng.seedWith(I.facing > 0 ? 11u : 23u);
    I.blinkTimer = I.facing > 0 ? 1.7f : 3.1f;
    I.hands[0].side = Side::Left;
    I.hands[1].side = Side::Right;
    float tableC = layout::TABLE_TOP_Y - pelvisWorld.y;
    I.hands[1].restContact = vec3(-0.290f, tableC, 0.305f);
    // The idle left hand rests on the table edge in front of the body, below the left shoulder,
    // fingertips short of the board frame (so it is not pushed out beside the board): clear of
    // the spare queen and the captured pieces standing beside the board even when the game sets
    // no obstacle callbacks.
    I.hands[0].restContact = vec3(0.180f, tableC, 0.215f);
    I.hands[1].rest = I.restSample(Side::Right, I.hands[1].restContact);
    I.hands[0].rest = I.restSample(Side::Left, I.hands[0].restContact);
    for (auto& h : I.hands) {
        Motion mo;
        mo.start = 0.0f;
        mo.segs.push_back(I.makeSeg(h.rest, 1e-3f, h.rest.p, vec3(0), h.rest.q, h.rest.f));
        h.motion = mo;
        h.pinch = pinchFor(sk, h.rest.q, 0.008f);
        h.gripQ = h.rest.q;
        h.carryQ = h.rest.q;
        h.capQ = h.rest.q;
    }
    I.initWriting();
    vec3 look(0, layout::BOARD_TOP_Y, 0);
    I.gazeTarget = look;
    I.fixFrom = I.fixTo = look;
    I.time = 0.0f;
    // Head starts looking at the board.
    I.evaluate(0.0f, I.poseI, I.worldI);
    vec3 headW = I.toWorld(transformPoint(I.G[Neck], vec3(0, 0.12f, 0.05f)));
    vec3 dc = rotate(conjugate(I.rootQ), look - headW);
    I.headPitch = std::atan2(dc.y, length(vec3(dc.x, 0, dc.z))) * 0.62f;
    I.evaluate(0.0f, I.poseI, I.worldI);
    I.exportPose(I.poseI, I.worldI, pose_, globals_);
}

void Animator::setRestHand(vec3 worldPos) {
    Impl& I = *impl_;
    if (!I.sk) return;
    Impl::Hand& h = I.right();
    h.restContact = I.toChar(I.mw(worldPos));
    h.rest = I.safeRest(Side::Right, h.restContact);
    I.restsDirty = true;
    if (!I.running && I.queue.empty() && I.rightIdle && h.heldId < 0 && !h.chinFollow) {
        HandSample from = h.motion.sample(I.time);
        Motion mo;
        mo.start = I.time;
        mo.segs.push_back(I.makeSeg(from, 0.6f, h.rest.p, vec3(0), h.rest.q, h.rest.f));
        h.motion = mo;
    }
}

void Animator::setLeftRestHand(vec3 worldPos) {
    Impl& I = *impl_;
    if (!I.sk) return;
    Impl::Hand& h = I.left();
    h.restContact = I.toChar(I.mw(worldPos));
    h.rest = I.safeRest(Side::Left, h.restContact);
    I.restsDirty = true;
    if (!h.chinFollow && I.writingHandFree()) {
        HandSample from = h.motion.sample(I.time);
        Motion mo;
        mo.start = I.time;
        mo.segs.push_back(I.makeSeg(from, 0.6f, h.rest.p, vec3(0), h.rest.q, h.rest.f));
        h.motion = mo;
    }
}

void Animator::enqueue(const Task& t) {
    Task c = t;
    c.position = impl_->mw(t.position);   // into the solver's world
    for (vec3& p : c.path) p = impl_->mw(p);
    impl_->queue.push_back(c);
}
void Animator::enqueue(const std::vector<Task>& tasks) {
    for (auto& t : tasks) enqueue(t);
}
bool Animator::busy() const { return impl_->running || !impl_->queue.empty(); }
bool Animator::runningTask(TaskType type) const { return impl_->running && impl_->cur.type == type; }
void Animator::clearQueue() { impl_->queue.clear(); }
void Animator::cancelTasks() {
    Impl& I = *impl_;
    I.queue.clear();
    I.running = false;
    I.curEvents.clear();
    I.prevWasClock = false;
    I.trace.reset();
    Impl::Hand& h = I.right();
    h.heldId = h.capId = h.releasedId = -1;
}
float Animator::remainingTime() const {
    const Impl& I = *impl_;
    float r = I.running ? std::max(0.0f, I.curStart + I.curT - I.time) : 0.0f;
    float end = I.time + r;
    for (auto& t : I.queue) {
        const float wait = std::max(0.0f, t.notBefore - end), d = taskDuration(t);
        r += wait + d;
        end += wait + d;
    }
    return r;
}
void Animator::lookAt(vec3 target, float weight) {
    impl_->gazeTarget = impl_->mw(target);
    impl_->gazeWeightTarget = clamp(weight, 0.0f, 1.0f);
}
void Animator::setHeadOverride(bool enabled, float yaw, float pitch) {
    Impl& I = *impl_;
    if (I.mirrored) yaw = -yaw;   // the solver's left is the character's right
    yaw = clamp(yaw, -70.0f * DEG, 70.0f * DEG);
    pitch = clamp(pitch, -45.0f * DEG, 30.0f * DEG);
    if (!enabled && I.headOverride) {   // hand over smoothly to the gaze controller
        I.headYaw = I.ovYaw;
        I.headPitch = I.ovPitch;
        I.headYawV = I.headPitchV = 0.0f;
    }
    I.headOverride = enabled;
    I.ovYaw = yaw;
    I.ovPitch = pitch;
}
void Animator::headAngles(float& yaw, float& pitch) const {
    const Impl& I = *impl_;
    yaw = I.headOverride ? I.ovYaw : I.headYaw + I.gestYaw;
    pitch = I.headOverride ? I.ovPitch : I.headPitch + I.gestPitch;
    if (I.mirrored) yaw = -yaw;   // back from the solver's side to the character's
}
void Animator::setLean(float lean) { impl_->leanTarget = clamp(lean, 0.0f, 1.0f); }
void Animator::setThinking(bool thinking) {
    Impl& I = *impl_;
    if (thinking && !I.thinking) I.thinkTimer = I.rng.range(1.0f, 3.0f);
    I.thinking = thinking;
}

void Animator::update(float dt, std::vector<Event>& events) {
    if (!impl_ || !impl_->sk) return;
    Impl& I = *impl_;
    I.owner = this;
    I.fresh.clear();   // the game has seen the release events of the previous call
    if (I.restsDirty && !I.running && I.queue.empty() && pieceTransform) {
        // First look at the pieces (spare pieces beside the board): the idle hands make room.
        I.restsDirty = false;
        I.snapshotPieces();
        I.validateRests(nullptr);
    }
    dt = std::max(0.0f, dt);
    const float tEnd = I.time + dt;
    const size_t ev0 = events.size();
    // Two task machines (playing hand, writing hand), stepped through their boundaries in time
    // order so each task starts exactly when the previous one of its hand ends.
    for (int guard = 0; guard < 512; ++guard) {
        const float never = 1e30f;
        float tm = never, tw = never;
        if (I.running) tm = I.curStart + I.curT;
        else if (!I.queue.empty()) tm = std::max(I.time, I.queue.front().notBefore);   // (the hand holds meanwhile)
        if (!I.nextWriteBoundary(tw)) tw = never;
        const float tn = std::min(tm, tw);
        if (tn > tEnd) break;
        I.time = std::max(I.time, tn);
        if (tm <= tw) {
            if (I.running) {
                I.finishTask(events);
            } else {
                Task t = I.queue.front();
                I.queue.pop_front();
                I.startTask(t, events);
            }
        } else {
            I.stepWriting(events);
        }
    }
    if (I.running) I.fireDue(tEnd, events);
    if (I.wr.running) I.fireWriteDue(tEnd, events);
    I.time = tEnd;
    // Body lean towards the sheet and eyes on the pen while the writing hand works.
    {
        const bool writing = I.wr.running && (I.wr.cur.type == WriteTaskType::Write || I.wr.cur.type == WriteTaskType::TurnPage);
        const bool penWork = I.wr.running && I.wr.cur.type != WriteTaskType::Wait;
        const float leanT = writing ? 1.0f : (I.wr.penHeld ? 0.5f : 0.0f);
        I.wr.lean += (leanT - I.wr.lean) * (1.0f - std::exp(-dt * 3.0f));
        I.wr.look += ((penWork ? 1.0f : 0.0f) - I.wr.look) * (1.0f - std::exp(-dt * (penWork ? 8.0f : 3.0f)));
    }
    I.updateIdle(dt);
    I.updateGaze(dt);
    I.evaluate(I.time, I.poseI, I.worldI);
    I.exportPose(I.poseI, I.worldI, pose_, globals_);
    I.exportEvents(events, ev0);
}

mat4 Animator::eyeCameraTransform() const {
    if (!impl_ || !impl_->sk) return mat4();
    const Skeleton& sk = *impl_->sk;
    vec3 mid = (sk.restOffset[EyeL] + sk.restOffset[EyeR]) * 0.5f;
    const mat4& head = globals_[Head];
    vec3 pos = transformPoint(head, mid);
    mat3 r = head.upper3();
    return mat4(mat3(-r.c[0], r.c[1], -r.c[2]), pos);
}

bool Animator::heldPieceTransform(int pieceId, mat4& out) const {
    const Impl& I = *impl_;
    if (pieceId < 0) return false;
    const mat4& hand = I.worldI[HandR];   // the playing hand in the solver's world
    if (pieceId == I.hands[1].heldId) { out = I.mm(hand * I.hands[1].heldAttach); return true; }
    if (pieceId == I.hands[1].capId) { out = I.mm(hand * I.hands[1].capAttach); return true; }
    return false;
}
bool Animator::holding(int pieceId) const {
    const Impl& I = *impl_;
    return pieceId >= 0 && (pieceId == I.hands[1].heldId || pieceId == I.hands[1].capId);
}
float Animator::time() const { return impl_->time; }

}  // namespace anim
