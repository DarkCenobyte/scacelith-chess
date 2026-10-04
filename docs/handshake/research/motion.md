# Handshake motion pipeline (src/anim): research report

Bottom line: the hands clip because the plan, not the IK, places them that way. Both robots put the same palm point on the same clasp point with almost antiparallel hands. Each closed hand then wraps around the partner's wrist and forearm cuff instead of the partner's ulnar edge. On top of that, the two thumbs cross. During the clasp the rendered hands match the planned (p, q) exactly.

All numbers below come from a read-only probe I compiled in the scratchpad. It includes the anim .cpp files the same way `tests/anim_tests.cpp` does and opens the private `Impl` with a `#define`. Setup: W at (0, 0.56, 0.60) facing −Z, B at (0, 0.56, −0.60), handshake started at 0.3 s, steps of 1/120 s. No tracked file was edited. The probes are reusable for the fix: `/tmp/claude-0/-home-user-scacelith-chess/ea3bb550-e871-579e-a29d-78cac27b4b97/scratchpad/probe/probe.cpp` (pose and penetration tables), `probe2.cpp` (left vs right-handed, pieces, late start), `probe3.cpp` (wrist strain grid), `probe4.cpp` (lowest hand point).

## 0. Entry points and frames
- **Timings:** `animator.h:53-56` sets Handshake 2.60, ClaspAt 0.92, ReleaseAt 1.96. `taskDuration` is at `animator.cpp:37`.
- **Dispatch:** `planTask` case Handshake, `animator.cpp:402-417`.
  - Right-handed: `planHandshake(t, start, T, from, mo)` drives `right()`.
  - Mirrored (left-handed player): it plans into `left().motion` (the real right hand). The playing hand settles at its rest in two segments.
- **After planning** (`animator.cpp:437-440`): `relaxWrist(h)` and `liftForearm(h)` are applied to `h = right()` only. For a left-handed player the shaking hand gets neither.
- **startTask** (`animator.cpp:1156-1166`): for a mirrored handshake it does `bakeFollow(left)`, `interruptWriting`, and sets `wr.suspendUntil = time + T`.
- **Planning space:** character space (+Y up, +Z forward, +X = character's left). The hand bone frame (skeleton.h:6-16, skeleton.cpp:47-64):
  - Fingers run along −Y and the right palm faces +X. Hand +Z is the thumb side.
  - MCP joints at y −0.086/−0.088/−0.085/−0.078 and z +0.030/+0.010/−0.009/−0.026 (index to pinky).
  - Thumb CMC at (+0.012, −0.022, 0.026), its phalanges 0.040/0.032/0.024. Finger lengths are about 0.082, 0.090, 0.085 and 0.068 m.
  - The palm half-thickness is `kPalmHalf` 0.0135 (`animator_impl.h:217`); the mesh is 0.0117-0.0132 (robot_hand.cpp:277-284).
  - `palmCenter(R) = (0.0135, -0.052, 0.003)` (`animator_impl.h:264`).
  - Forearm radius is about 0.026-0.033 (roundCone 0.0330 → 0.0256 m, x-scaled ×0.9-0.97) plus a wrist cuff of 0.0205 + 0.0045 (robot_body.cpp:80-97, robot_build.h:43).
- **`handRot(s, yaw, pitch, roll) = qy(yaw)*qx(pitch)*qz(rs*roll)*handBasis(s)`** (`animator_impl.h:422-433`). The basis is palm down, fingers forward. With roll = π/2 the palm normal is +X char, the thumb side points up, and `pitch` becomes the finger pitch inside the vertical palm plane.

## 1. What planHandshake computes (`animator.cpp:448-553`)
1. **Shared state:** `R = shakeSide()`, `partner = t.partner`, `shakeStart = start` (L449-452).
2. **Clasp centre C**, world, L454-460:
   - `cW = t.position`. The game passes 0 (`game_scene_detail.h:16`).
   - Otherwise `mid = (pelvisWorld + partnerPoint(partner pelvis))/2` and `cW = (mid.x, BOARD_TOP_Y + 0.225, mid.z)` = (0, 1.007, 0).
   - `partnerPoint` (`animator_writing.cpp:259-262`) is `mw(partner->mw(p))`, so mirrored partners convert correctly.
   - `C = toChar(cW)` = (0, 0.447, 0.600) for both robots.
3. **Yaw** (L462-464): `S = shoulderRest(R)` = (−0.19, 0.47, −0.02) (`animator_impl.h:1071-1074`). `dirH` = horizontal S→C, so `yaw = atan2(dirH.x, dirH.z)` = 0.2974 rad. |S→C| is 0.649 m against L1+L2 = 0.565 m; the torso and clavicle make up the difference.
4. **Nominal rotation** (L466): `qs = handRot(R, yaw, 0.28, π/2)`. The palm is vertical, the thumb up, the fingers pitched 0.28 rad down within the palm plane. The contact point (L467) is `palm = handPoint(R, palmCenter + (0.0015, 0, 0))` = (0.015, −0.052, 0.003). The local palm normal is (palmSign, 0, 0).
5. **Comfort search** (L469-483):
   - It tries `q = axisAngle(pn, 0.1k)*qs` for k = −6..6, with cost `armStrainSide(R, wristFor(C,q,palm), q) + 0.03|0.1k|`.
   - Measured result: k = −2, so the effective in-plane pitch is θ = 0.08 rad, not 0.28.
   - Why: at 0.28 the ulnar deviation is −0.719, past the soft limit of −0.55 (strain 0.084). From k = −6 to −2 the strain is 0.
   - Strain definition (`animator_impl.h:691-694, 1256-1267`): clamps + 10·reachShort + 0.5·soft. Soft terms are |flex| > 1.10, dev > 0.35, dev < −0.55, |pron| > 1.60.
6. **Finger poses** (L484):
   - `fo = fpHumanize(poseShakeOpen, 0.2, 0.03)`, `fg = fpHumanize(poseShakeGrip, 0.6, 0.04)` (`animator_impl.h:151-160, 116-123`).
   - The grip is thumb {opp 0.25, CMC 0.10, MCP 0.25, IP 0.20}; fingers {spread, MCP, PIP, DIP} = I {.02, .55, .85, .45}, M {0, .60, .90, .45}, R {−.03, .65, .92, .45}, P {−.07, .72, .92, .45}.
   - Humanize changes only fingers 1-4 by up to ±amount·(0.6 + 0.25f). The seeds are constant, so both robots get identical fingers.
7. **Wrist targets:**
   - `pClasp = C − rot(qs)·palm` (L485).
   - `pPre = pClasp − 0.07·fingerDir − 0.015·palmN + (0, 0.01, 0)` (L486-489).
8. **Phase times** (L490-494): scale = T/2.60, kept as `shakeScale`. t1 = 0.74s, t2 = 0.92s (clasp), t3 = 1.96s (release), t4 = t3 + 0.15s, all multiplied by scale.
9. **Pen first** (L496-512, mirrored only): if the writing hand holds the pen, `t0 = 0.30·scale`, `penPutSegments`, and PenPut at start + t0. That instant is tested.
10. **Segments:**
    - **a, extend** [t0, t1], L515-526: makeSeg to pPre with v1 = (pClasp − pPre)·0.8/(t2 − t1). Arc 0.05 at 0.45; clearPath raises it to 0.056 and sets he = 0.80. Rotation keys: from.q at 0, qs at 0.80. Finger keys: from.f, then letGo at 0.20 (pen case), fo at 0.60.
      - Quirk: past `he`, `basePos` extrapolates p1 + v1·(t − t1) (`animator_impl.h:537`). At v1 ≈ 0.32 m/s for 0.148 s, the hand travels about 47 mm more in XZ.
      - So the 7 cm pull-back is really about 24 mm at t1. The palm centres are 48.5 mm apart at u = 0.74.
    - **b, slide in** [t1, t2], L529-534: to pClasp with v = 0. Fingers stay s.f until u = 0.15, then close to fg by u = 1, i.e. they close while the hand is still sliding the last ~24 mm.
    - **c, hold and pumps** [t2, t3], L537-542: pClasp/qs/fg held, plus `oscAmp 0.032`, `oscCycles 2`, `os 0.04`, `oe 0.92`, vertical axis.
      - Formula: y += 0.032·sin²(πw)·sin(4πw), w = (u − 0.04)/0.88 (`animator_impl.h:514-527`).
      - Measured palm-centre y range is board +197..253 mm. The lowest hand point is board + 163 mm (PinkyR1 at a pump bottom); a king is 95 mm.
    - **d, release** [t3, t4], L545-546: to pClasp − 0.02·fingerDir − 0.006·palmN, fingers to lerp(fg, fo, 0.85). Opening and withdrawing happen together over 0.15 s.
    - **e, retract** [t4, T], `shakeRetract` L555-569: to the rest pose, arc 0.03 (clearPath raises it to 0.066), rotation key at 0.80, fingers relaxed by u = 0.4.
11. **Events** (L550-552): `HandshakeClasp` at start + t2 and `HandshakeRelease` at start + t3, both ActNone. Their position is `curTargetWorld = cW` (fireDue L1219), mirrored on export.

**Where the two animators must agree.** There is no coupling at runtime except `partner` (pelvis for C, head for gaze). Agreement holds only because:
- both compute the same C,
- both use the same char-space constants and seeds,
- both start the task in the same update with the same duration.

The seats are related by a 180° rotation about the vertical axis through C (C2 symmetry), so the two horizontal S→C directions are exactly antiparallel. Measured: off-centre seats still give a 3.00 mm palm gap. In this symmetric layout, each hand's coordinates in the partner's frame are identical (the probe tables for W→B and B→W match to 0.1 mm).

The pumps are phase-locked only by equal start times. If W starts 0.1 s late, the vertical palm mismatch during the clasp is 39.4 mm. The game enqueues both robots in the same frame while both are idle (`game_scene.cpp:1245-1286`).

## 2. Rendered pose vs planned (p, q)
- **Clasp phase (b, c, d): identical.**
  - The rendered `HandR` global equals the plan at 0.00 mm and 0.000 rad at u = 0.40, 0.74, 0.80, 0.93, 1.07 … 2.12, for W and B.
  - Clasp joint values: wristClamp 0, pronClamp 0, reachShort 0, flex −0.174, dev −0.50 to −0.52, pron −0.30.
  - Arm shape: |S−W| = 0.496 m (88% reach), elbow angle 2.14 rad, forearm rising 0.43 rad toward the wrist. The angle between forearm and fingers is 0.535 rad, which is the source of the ulnar deviation.
- **relaxWrist** (`animator.cpp:642-676`):
  - It adds `rotCorr` 0.279 rad to segment a and 0.300 rad to segment e. That bump has faded by u = 0.80, the last rotation key. Segments b, c and d get nothing because their strain is under 0.04.
  - Risk: a new clasp pose with armStrain ≥ 0.04 inside segment c would get a mid-pump rotCorr of up to 35% toward the forearm-neutral rotation (L664). That would hit the right-handed robot only.
- **liftForearm** (`animator.cpp:681-768`): no effect. With the 32 initial pieces known the arcs and elbows are unchanged. It returns early without piece knowledge (L682).
- **IK limits** (`solveArm`, `animator_impl.h:926-993`):
  - Reach clamp at 0.9995·(L1 + L2) (L935); pronation [−1.75, 1.95] (L973); wrist flex [−1.30, 1.40] and deviation [−0.75, 0.50] (L983).
  - A clamp keeps the wrist position and rotates the hand by the excess (L985-990). Today nothing clamps. Above dev −0.75 the hand would rotate away from the plan, symmetrically on both robots.
- **Torso**, right-handed: `solveSpine(hr.p)` (L799; `animator_impl.h:887-923`) gives, at the clasp without idle motion, flex 0.075, twist 0.124, side −0.030. Together with clavicle protraction this brings the right shoulder about 0.10 m forward. It does not move the hand, since the IK reaches the target.
- **Torso, left-handed:**
  - The mirrored solve is blended in (L800-814).
  - `writingSpine` (`animator_writing.cpp:1199-1214`) adds flex while |hl.p − left shoulder| > 0.84(L1 + L2) = 0.475.
  - Result: Spine2 and Neck sit up to 37.9 mm away from a right-handed robot's at u = 1.32. The hand bones are still identical during clasp to release (0.00 mm).
  - Extend and retract rotations differ up to 0.244 rad, because the left-handed robot gets no relaxWrist.
  - The comment at L445-446 ("exact mirror") is true for the hand during the clasp, not for the torso or the extend/retract phases.

## 3. Why the hands interpenetrate (measured at u = 0.93 and u = 1.32, the same at both)
- **Placement:** both robots put hand-local (0.015, −0.052, 0.003) on C. The palm centres are 3.0 mm apart, the normals have dot −1.000, and each palm centre sits at (0.0165, −0.052, 0.003) in the partner's frame. The palms themselves do not overlap.
- **Orientation:** both palms are vertical and the thumb sides point straight up (0.023, 0.997, 0.076). The fingers pitch only 0.08 rad, so the angle between fA and −fB is 0.160 rad: nearly antiparallel.
- **Geometry under C2 symmetry:** A's finger direction in B's frame is (y: cos 2θ toward B's wrist, z: −sin 2θ toward B's ulnar edge). At 2θ = 0.16 that is 99% along B's hand axis and 16% toward its ulnar edge.
- **Knuckles:** A's MCP row lies at B-local y = −0.014 (index), −0.015 (middle), −0.021 (ring), −0.031 (pinky), at x = 0.034 (20 mm off B's palm). That is right over B's wrist end of the palm. The fingers point past B's wrist pivot (y = 0).
- **Curl:** the grip pose curls the fingers 1.85-2.09 rad in total toward A's own palm, which is toward and through B.
  - Index, middle and ring middle phalanges reach B-local x ≈ 0 at y = +0.012 to +0.020 (inside B's wrist).
  - Their tips end at x = −0.029 to −0.037, 15-23 mm behind B's back.
  - A's middle2 is 7.2-8.7 mm from B's forearm axis (at 93% of elbow to wrist), against a forearm radius of about 25 mm, so roughly 17 mm deep. The ring is 12-13 mm from the axis.
  - The fingers grip the partner's wrist and forearm cuff, not its hand.
- **Pinky:** at B-local z ≈ −0.034 it passes through B's palm and hypothenar (x from +0.012 to −0.020, y −0.008 to −0.022).
- **Thumbs:**
  - The grip thumb (opposition only 0.25) stays along the index side, about 45° up and forward (ThumbR2 offset (0, −0.028, 0.028)).
  - A's CMC sits at B-local (0.018, −0.078, 0.031), on B's thenar 4.5 mm off its palm.
  - A's thumb tip is at (0.006, 0.009, 0.064), 34 mm above B's index edge and past B's wrist.
  - The two thumbs cross in an X: thumb2 bases are 10.9 mm apart at the clasp, and thumb3 bases are already 7.5 mm apart at u = 0.74 during the approach. Two finger radii would need about 14 mm.
- **Other clipping:**
  - The fingers close during the slide-in (segment b, from u = 0.15).
  - Release opens and withdraws together (segment d), so curled tips drag back through the partner.
  - A cancelled handshake: the partner pumps on alone and the cut hand leaves with the usual Retract (fingers 70% relaxed by u = 0.35).

## 4. What can change, and the constraints
- **Clasp height h = 0.225** (L458). Ulnar deviation ≈ −(forearm rise) − 1.1θ. Lower h flattens the forearm.

  | h (m above board) | forearm rise (rad) | dev at θ = 0.4 (rad) |
  |---|---|---|
  | 0.10 | 0.12-0.18 | −0.56 (strain 0.01) |
  | 0.15 | 0.24-0.29 | −0.68 (strain 0.07) |
  | 0.225 | 0.41-0.45 | −0.85 (clamped by 0.10, strain 0.25) |

  - Limits on lowering: pieces in the centre (king 95 mm) against the lowest hand point (board + 163 mm today, pumps −28 mm).
  - h also feeds the gaze glance target and the clasp sound, which is hard-coded at `BOARD_TOP_Y + 0.22` (`game_scene.cpp:1994`). The viewer "shake" camera looks at y = 1.0 (`anim_viewer.cpp:1500`).
- **Hand rotation qs:**
  - Yaw is free (palm-plane azimuth). The in-plane pitch θ gives a crossing of 2θ. The roll must stay π/2: under C2 symmetry, palm contact needs a horizontal palm normal, and any tilt opens the palms by 2× the tilt.
  - The comfort search (L469-483) will pull θ back to about 0.08 unless it is replaced or biased.
  - Elbow lift (Segment `elbow0/elbow1` → `HandSample.elbow`, swivel at `animator_impl.h:951-956`; makeSeg sets elbow1 = 0 at L1418): at h = 0.225, a lift of 0.4 gives rise 0.30, dev −0.76 at θ = 0.4 (strain 0.12), but pron −0.69 and flex −0.43.
- **Per-hand clasp anchor:** replace `palm` (L467) with another hand-local point, always through `handPoint(R, ·)`.
  - Under C2 symmetry, an offset of a hand-local amount along the horizontal in-plane direction moves the palms 2× apart along it, and an offset along the normal moves them 2× apart too. Vertical offsets move both hands together, so no relative vertical offset is possible.
  - For the fingers to clear the ulnar edge before the partner's wrist, the knuckles need to sit around partner y ≈ −0.065 (mid-palm), and 2θ needs to be about 45° (θ ≈ 0.39). This is a rough estimate.
- **Finger and thumb poses** (`animator_impl.h:151-160`, rows {spread, MCP, PIP, DIP}; thumb {opposition, CMC, MCP, IP}):
  - They can be solved per finger with `solveLongFinger` and `solveThumb` (`animator_impl.h:277-336`) on pad targets in the partner's frame.
  - That frame is known analytically at plan time: rotate the own clasp pose 180° about the vertical through C.
  - Targets: the partner's back at x = −0.0132 − kPadRadius 0.0068; for the thumb, the partner's index MCP at (−0.004, −0.086, 0.030).
  - Finger FK runs on Side::Right geometry and is mirrored with handPoint (pattern at `animator_impl.h:1003-1010`). The FingerPose values themselves are side-agnostic (`fingerLocal` applies palmSign / fingerCurlAxis).
- **Timing:**
  - Keys and segment boundaries can move, and segments can be added (e.g. close only after contact; open first, then withdraw along the palm normal).
  - Fixed instants: clasp = start + 0.92·scale (tested to 1e-4), release = 1.96·scale (documented), total = T, PenPut = 0.30·scale (tested).
  - Pumps (oscAmp, oscCycles, os, oe, oscAxis) translate both hands identically, so there is no relative motion.
- **Consistency rules:**
  - Plan purely from shared data: C, own pelvis/facing, constants.
  - Do not use `rng` or `seed`; they differ per facing (`animator.cpp:1324-1326`).
  - Do not read the partner's motion at plan time: W updates first, so B has not planned its handshake yet in that frame.
  - Keep `shakeStart`/`shakeScale`, which the gaze uses.
  - Keep the clasp strain under 0.04 (relaxWrist) and stay inside the IK clamps, so the render matches the plan.
  - The left-handed path calls the same function with R = Left (`armStrainSide`, `handRot`, `handPoint`, `palmSign`), so every new hand-local constant must go through those helpers.
- **Gaze** (`animator.cpp:989-998, 1010`):
  - shakeT = (time − shakeStart)/shakeScale.
  - Weight to the partner's face: smoothstep(0, 0.25) × (1 − smoothstep(2.1, 2.5)).
  - Glance at the shaking wrist (not at C): smoothstep(0.45, 0.62) × (1 − smoothstep(0.82, 1.02)) × 0.8.
  - Nod: −0.10·sin over shakeT 0.85-1.6.
  - Moving the wrist or lowering C changes the head pitch.

## 5. Tests and checks that must keep passing
**`tests/anim_tests.cpp`:**
- `anim_left_handed_handshake_cuts_page_turn` (526-560): PageTurned at the start; clasp = start + 0.92 within 1e-4.
- `anim_left_handed_handshake_after_pen_put_mid_frame` (565-607): exactly one PenPut, at its own instant and spot.
- `anim_handshake_gaze_follows_its_duration` (612-650), the most sensitive one:
  - clasp at 0.92·scale for scale 1 and 1.5;
  - deepest head pitch within 0.15 s of the clasp;
  - looks down only after T − 0.5·scale;
  - for scale 1, deepest pitch at 0.90 s ± 1.5/120 s (L648).
- `anim_cancel_handshake_lets_go_of_the_partner` (710-768): cut at 1.2 s, right- and left-handed:
  - hand within 2 mm of an idle robot's rest after Retract + 0.05 s (L761);
  - head within 0.02 rad (L762);
  - chest step change under 5 mm (L763);
  - not busy afterwards.
- `anim_cancel_handshake_lays_the_pen_down` (773-844): PenPut at start + 0.30 (L835), WritingQueueEmpty Retract later, hand within 2 mm (L839).
- The mirror tests (372, 453; gestures at 1476) do not involve the handshake.

**Viewer `--selftest`** (`anim_viewer.cpp`; run by hand, not in CI):
- Handshake with a left-handed player holding the pen (2420-2466). It warns if:
  - PenPut is missing, or the pen is still held at the clasp;
  - claspW and claspB differ by more than 1e-5;
  - the "right palms" gap exceeds 50 mm. That gap is measured at hand-local (0.0135, −0.052, 0.003) on both HandR. Per-hand anchor offsets enlarge it by 2× the offset.
- `timelineCheck` (1324-1398, default demo with the handshake at 0.5 s, lines 1194-1201) logs the lowest fingertip, hand/piece overlap, angular-speed pops over 1800°/s and camera roll. Fast finger closing or opening would show up as pops.

**Other references:** the clasp sound at `game_scene.cpp:1993-1994` (y = board + 0.22), handshake enqueues at `game_scene.cpp:1245-1286`, the coach mention at `lesson.cpp:912`.