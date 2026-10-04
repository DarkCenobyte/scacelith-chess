HAND/ARM GEOMETRY REPORT (read-only; no tracked files edited)

All numbers come from the code. Mesh-derived numbers were measured on the real meshes: I ran buildHand/buildArm in a scratch program and ray-cast the bone-local meshes along axes. The scratch tools are listed in section 5.

==================================================================
1. BONE HIERARCHY, REST OFFSETS, AXIS CONVENTIONS
==================================================================
Global conventions (src/character/skeleton.h:3-18):
- Character space: +Y up, +Z forward, +X = the character's LEFT.
- Every bone frame is axis-aligned with character space at rest (all local rotations are identity). The joint pivot is the bone origin.
- Limb children lie along local -Y.

Hierarchy (skeleton.cpp:115-138; right side, sx = -1):
- Spine2 -> ClavicleR: offset (-0.025, 0.225, 0.020), len 0.16
- ClavicleR -> UpperArmR (shoulder): (-0.165, 0.005, -0.030), len 0.300
- UpperArmR -> ForeArmR (elbow): (0, -0.300, 0), len 0.265
- ForeArmR -> HandR (wrist pivot): (0, -0.265, 0), boneLength 0.085
- HandR -> Index/Middle/Ring/PinkyR1 (MCP): (sx*0.004, fy, fz) = (-0.004, fy, fz)

| Finger | fy | fz | Phalanx lengths L1/L2/L3 |
|---|---|---|---|
| Index | -0.086 | +0.030 | 0.039 / 0.024 / 0.019 |
| Middle | -0.088 | +0.010 | 0.043 / 0.027 / 0.020 |
| Ring | -0.085 | -0.009 | 0.040 / 0.026 / 0.019 |
| Pinky | -0.078 | -0.026 | 0.032 / 0.019 / 0.017 |

- Finger X2 sits at (0, -L1, 0) from X1, and X3 at (0, -L2, 0) from X2.
- HandR -> ThumbR1 (CMC): (-sx*0.012, -0.022, 0.026) = (+0.012, -0.022, +0.026), boneLength 0.040.
- ThumbR2 (MCP): (0, -0.028, 0.028) from R1, so |o2| = 0.0396. boneLength 0.032.
- ThumbR3 (IP): (0, -0.020, 0.024) from R2, so |o3| = 0.03124. boneLength 0.024.
- Shoulder at rest in character space (shoulderRest, animator_impl.h:1071) = (-0.19, 0.47, -0.02). Arm lengths L1 = 0.300 and L2 = 0.265 (animator.cpp:1322-1323).

HandR local frame (robot_hand.cpp:11-12):
- Origin = wrist pivot.
- Fingers run along -Y.
- Palm normal = +X.
- Back of the hand = -X.
- Thumb / index side = +Z; pinky (ulnar) side = -Z.
- Z = X × Y (handBasis, animator_impl.h:422-428).
- Each finger bone has the same axes at rest: -Y along the finger, +X = pad, Z = hinge axis.
- Thumb bones: segment direction is a1 = norm(0, -0.028, 0.028) = (0, -.707, .707) for the metacarpal, and a2 = norm(0, -0.020, 0.024) = (0, -.640, .768) for both phalanges. The hinge (width) axis is X. Pad direction padOf(a) = (0, -a.z, a.y): (0, -.707, -.707) and (0, -.768, -.640).
- So at rest the thumb lies in the palm plane at x = +0.012, pointing down and radially at 45°. Its pad faces the index/fingertip side, not the palm normal. The nail faces (0, +.768, +.640).

LEFT hand:
- Skeleton offsets are X-mirrored (sx = +1).
- Meshes are the right meshes with transform scale(-1, 1, 1) (robot_model.cpp:54-65, 85-90).
- Palm normal = -X; fingers -Y; thumb +Z.
- palmSign(L) = -1 and sideX(L) = +1 (animator_impl.h:189-190).
- handPoint(s, p) mirrors x for the left (animator_impl.h:416-417).
- FingerPose values are shared in right-hand convention.

fingerLocal (animator_impl.h:193-205):
- Long fingers:
  - out[0] = qx(-spread) * axisAngle(curl, MCP)
  - out[1] = axisAngle(curl, PIP)
  - out[2] = axisAngle(curl, DIP)
  - curl = fingerCurlAxis = (0, 0, +1) right, (0, 0, -1) left (skeleton.h:71).
  - Flexion + rotates the finger direction (0, -1, 0) to (sinθ, -cosθ, 0), i.e. towards +X (the palm) for the right hand.
  - Flexion is applied first (local), then spread about X. Spread + turns the finger towards +Z (the thumb side) on both hands, since a rotation about X is invariant under the X mirror.
- Thumb:
  - out[0] = qy(ps*opp) * qx(cmcFlex); out[1] = qx(mcpFlex); out[2] = qx(ipFlex).
  - qx(+) curls the thumb within the palm plane towards its pad (-Y, -Z: towards the fingers / index side).
  - qy(+ps*opp) then swings it about the hand's long axis towards the palm side (+X for the right hand). At opp = π/2 the thumb axis is (0.707, -0.707, 0).
- FingerPose row layout (animator_impl.h:94-98, 126): thumb {opposition, CMC, MCP, IP}; fingers {spread, MCP, PIP, DIP}. kDipCoupling = 0.62 (line 267).
- Current shake presets (animator_impl.h:151-160):
  - ShakeOpen thumb {0.18, -0.12, 0.10, 0.08}
  - ShakeGrip thumb {0.25, 0.10, 0.25, 0.20}; fingers about {0.55-0.72, 0.85-0.92, 0.45}

==================================================================
2. DIMENSIONS (meters, HandR frame unless stated)
==================================================================

Key points, all joints at 0:

| Point | Position |
|---|---|
| Wrist pivot | (0, 0, 0) |
| palmCenter(Right) | (+0.0135, -0.052, +0.003) (kPalmHalf = 0.0135, animator_impl.h:217, 264) |
| Index MCP | (-0.004, -0.086, +0.030) |
| Index PIP | (-0.004, -0.125, +0.030) |
| Index DIP | (-0.004, -0.149, +0.030) |
| Index tip | (-0.004, -0.168, +0.030) |
| Middle MCP | (-0.004, -0.088, +0.010) |
| Middle PIP | (-0.004, -0.131, +0.010) |
| Middle DIP | (-0.004, -0.158, +0.010) |
| Middle tip | (-0.004, -0.178, +0.010) |
| Ring MCP | (-0.004, -0.085, -0.009) |
| Ring PIP | (-0.004, -0.125, -0.009) |
| Ring DIP | (-0.004, -0.151, -0.009) |
| Ring tip | (-0.004, -0.170, -0.009) |
| Pinky MCP | (-0.004, -0.078, -0.026) |
| Pinky PIP | (-0.004, -0.110, -0.026) |
| Pinky DIP | (-0.004, -0.129, -0.026) |
| Pinky tip | (-0.004, -0.146, -0.026) |
| Thumb CMC | (+0.012, -0.022, +0.026) |
| Thumb MCP | (+0.012, -0.050, +0.054) |
| Thumb IP | (+0.012, -0.070, +0.078) |
| Thumb tip | (+0.012, -0.0854, +0.0964) |

Pads (fingerPad = 0.72·L3 along the bone + kPadRadius 0.0068 towards the pad):
- Index (+0.0028, -0.1627, 0.030)
- Thumb (+0.012, -0.0863, +0.0869)

Mesh tips coincide with the bone ends.

Whole hand AABB:
- Zero pose: x[-0.0172, +0.0258], y[-0.178, +0.0205], z[-0.0345, +0.0973]
- ShakeGrip: x[-0.0172, +0.0659], y[-0.130, +0.0205]

ShakeGrip tips:
- Index (+0.058, -0.118, +0.031)
- Middle (+0.066, -0.118, +0.010)
- Ring (+0.063, -0.109, -0.010)
- Pinky (+0.051, -0.092, -0.027)
- Thumb (+0.024, -0.102, +0.073)

Palm length:
- Wrist pivot to MCP line: mcpLineY(z) = -0.088 + 6.9(z - 0.012)² (robot_hand.cpp:169), i.e. 0.078-0.088.
- Palm mesh y-span: +0.0205 (carpal dome) to -0.0886.

Palm width:
- MCP centres: index z +0.030 to pinky z -0.026 = 0.056.
- Mesh z-span at x = 0: y = -0.03 [-0.0256, +0.0252]; y = -0.05 [-0.031, +0.033]; y = -0.06 [-0.0325, +0.0365]; y = -0.07 [-0.0337, +0.0377].
- Overall AABB z [-0.0345, +0.0396].

Palm thickness, x-span back..palm (palm lambda robot_hand.cpp:274-307; mesh measured):

| y | z = 0 | z = +0.012 | z = +0.024 |
|---|---|---|---|
| -0.03 | [-0.0139, +0.0119] | [-0.0131, +0.0123] | [-0.0068, +0.0069] |
| -0.05 | [-0.0130, +0.0111] (24 mm) | [-0.0126, +0.0116] | [-0.0100, +0.0125] |
| -0.065 | [-0.0127, +0.0108] | | |

- Palm surface at palmCenter's (y, z) is x ≈ +0.0110. palmCenter therefore sits 2.5 mm in front of the skin; planHandshake adds 1.5 mm, so it is 4 mm in front.
- The palm is concave: the edges drop towards the palm (xs = x + 0.001 - 4.5z²).
- Hypothenar pad: ellipsoid centre (0.0078, -0.045, -0.021), radii (0.0068, 0.025, 0.0115). Palm surface reaches x = +0.0154 at (y -0.040..-0.045, z -0.024..-0.018).
- Ulnar border (-Z): at z = -0.030 the slab is y = -0.04 [+0.0014, +0.0132], y = -0.05 [-0.0022, +0.0140], y = -0.06 [-0.0049, +0.0136]. So it is 12-18 mm thick, with its centreline at x ≈ +0.006 (palm side). Edge at z ≈ -0.031..-0.034.
- Back surface: -0.013 mid-palm, -0.0147 at y = -0.02. Knuckle bumps -0.014..-0.015 at y ≈ -0.083. Carpal dome down to -0.0172.
- Carpal dome: sphere r = kWristDome = 0.0205 (robot_build.h:207), clipped to |x + 0.0008| ≤ 0.015. Dark wrist ball r = 0.0148.

Thenar on ThumbR1 (robot_hand.cpp:221-241):
- Bulge centre in the hand frame ≈ (0.016, -0.036, 0.036).
- Thumb mesh at zero pose spans x [+0.0012, +0.0258]: x up to +0.0252 at y -0.03..-0.04, z 0.034-0.040.
- In ShakeGrip the front reaches +0.028..0.030 at y -0.04..-0.09, z 0.04-0.07.
- The thumb leaves the palm's radial edge at z > 0.034.

Long-finger porcelain cross-sections (half thickness along ±X around the bone axis / half width along Z; proximal / middle / distal at mid-segment):

| Finger | Proximal | Middle | Distal |
|---|---|---|---|
| Index | 5.7 / 8.2 mm | 4.75 / 7.5 mm | 3.75 / 6.5 mm |
| Middle | 5.9 / 8.4 mm | 4.95 / 7.8 mm | 3.95 / 6.7 mm |
| Ring | 5.5 / 7.9 mm | 4.55 / 7.3 mm | 3.65 / 6.3 mm |
| Pinky | 4.7 / 7.0 mm | 3.85 / 6.45 mm | 3.05 / 5.5 mm |

- Shells taper about 0.4 mm from proximal to distal end.
- Dark joint bases:
  - MCP balls r: index 7.35, middle 7.57, ring 7.13, pinky 6.32 mm (palm sockets +0.4 mm).
  - PIP barrels r = 4.9·sc mm; DIP barrels r = 4.3·sc mm, with sc = 1 / 1.03 / 0.97 / 0.86 (index / middle / ring / pinky).
- At rest the finger axis is at x = -0.004, so the palmar finger surface is at x ≈ +0.002, about 9 mm behind the palm skin (+0.011).

Thumb cross-sections (pad direction / X half extents):
- Metacarpal (thenar): pad [-0.008, +0.0105], X [-0.0105, +0.0134]
- Proximal phalanx: ±6.4 / ±9.3 mm
- Distal phalanx: ±5.0 / ±7.9 mm
- CMC ball r 9.2 mm; MCP/IP barrels r 6.2 / 5.5 mm along X.

Forearm (robot_body.cpp:80-97). ForeArmR frame; wrist at y_f = -0.265, i.e. y_hand = y_f + 0.265 with a straight wrist.

| y_f | x half-span | z half-span |
|---|---|---|
| -0.20 | ±0.025 | ±0.027 |
| -0.24 (narrowest) | ±0.0236 | ±0.026 |
| -0.26 (cuff) | ±0.0259 | ±0.027 |
| -0.265 | ±0.0257 | ±0.0265 |
| -0.10 | [-0.033, +0.035] | [-0.035, +0.039] |

- The forearm ends at y_hand = -0.0025.
- Wrist socket inner r = 0.021; cuff sphere r = 0.025.
- The cuff stands 11-15 mm proud of the palm skin, and on the back, right at the wrist.

Arm joint balls: shoulder 0.030, elbow 0.0255.

==================================================================
3. HOW POSES BECOME BONES AND RENDER MATRICES
==================================================================
- HandSample (animator_impl.h:468-481):
  - p = wrist (HandR origin) in CHARACTER space
  - q = HandR GLOBAL rotation in character space
  - f = FingerPose; elbow = swivel
- Segment::sample (animator_impl.h:553-608) produces this sample.
  - Handshake pumps: oscAmp 0.032 m vertical, 2 cycles (animator.cpp:537-542).
- planHandshake (animator.cpp:448-553):
  - qs = handRot(R, yaw, 0.28, π/2), then a ±0.6 rad search about the palm normal minimising armStrainSide.
  - pClasp = wristFor(C, qs, palmCenter + (0.0015, 0, 0)), where wristFor(target, q, lp) = target - rotate(q, lp) (animator_impl.h:1341).
- Impl::evaluate (animator.cpp:775-953):
  - solveSpine + applySpine, then fkChain Pelvis..Spine2.
  - solveArm(pose, side, p, q, elbow), animator_impl.h:926-993: clavicleFor, analytic two-bone IK with a pole.
    - Forearm twist (pronation) = swing-twist of R about the forearm Y, clamped to [-1.75, 1.95].
    - hand local = conj(gF)·R, then the wrist is clamped: flex = atan2(fd.x·ps, -fd.y) ∈ [-1.30, 1.40] (+ = towards the palm); dev = atan2(fd.z, -fd.y) ∈ [-0.75, 0.50] (+ = radial).
    - Any twist left over after the pronation clamp goes into the wrist and is not clamped.
    - Reach is clamped at 0.9995·(L1 + L2).
    - Soft strain thresholds (softWristStrain): |flex| > 1.10, dev > 0.35, dev < -0.55, |pron| > 1.60.
    - With SCACELITH_ANIM_ARMTRACE=1 the current handshake prints no armtrace line, i.e. wrist/pronation clamps stay ≤ 0.05 rad.
  - Optional pinW point lock (animator.cpp:882-895).
  - applyFingers sets pose.local[fingerBone] = fingerLocal (animator_impl.h:209-214).
  - computeGlobal(sk, pose, worldOut) (skeleton.cpp:155-162): world = root·Π toMat4(local, restOffset).
- Left-handed players: the solver runs mirrored and shakes with the solver's Left (shakeSide/shakeHand, animator_impl.h:1464-1465). exportPose (animator_writing.cpp:264-278) maps to the real bones, so Animator::globals()[HandR] is always the real right hand (animator.cpp:1360).
- Rendering: submitRobot (robot_render.cpp:161-185) uses d.model = boneWorld[part.bone]. Meshes are rigid, bone-local, rest pose (no skinning).
- In-solver character-space globals live in Impl::G[] (fkChain, animator.cpp ~837).

==================================================================
4. EXISTING PROXIMITY / COLLISION HELPERS
==================================================================
- animator_impl.h:
  - kPadRadius 0.0068 and kPalmHalf 0.0135 (216-217)
  - fingerFrames (220-229): hand-local mat4 per phalanx
  - boneDir, fingerTipFrom, fingerTip, fingerPad (231-252)
  - phalanxMidFrom, phalanxMid (254-262)
  - restClear (1029-1043): sphere probes at wrist r 0.022, palm r 0.028, joints r 0.010, phalanx mids r 0.009, tips r 0.008
- animator_gesture.cpp:36-58, handDepth: the same probe set (wrist 0.020, palmCenter 0.020, joints 0.011 / 0.009, mids 0.009, tips 0.008) against the pieces.
- animator_writing.cpp:
  - padContact / middleSide (80-95): skin point + outward normal at a finger pad
  - grip support spheres (398-413)
- anim_viewer.cpp:
  - DebugBody capsule radii per phalanx (171-183): thumb 10.5 / 9.2 / 8.2 mm, index 8.2 / 7.5 / 6.8, middle 8.6 / 7.8 / 7.0, ring 8.0 / 7.3 / 6.6, pinky 7.2 / 6.6 / 6.0. Hand rbox centre (0, -0.047, 0.002), half extents (0.0135, 0.042, 0.037), r 0.010; forearm capsule 0.035 to 0.025.
  - These radii are about 1.5× the real phalanx half-thickness: too fat for a clasp.
  - pieceOverlapIf (296-335) works against pieces only.
  - Handshake selftest (2443-2465) only checks that the two palmCenters are < 5 cm apart.
- tests/anim_tests.cpp:612+ shows how to run two Animators shaking hands headless; the tests include the anim sources and skeleton.cpp.
- No hand-vs-hand test exists. The hand SDFs sit in an anonymous namespace in robot_hand.cpp.

==================================================================
5. SCRATCH TOOLS (not in the repo) AND BASELINE
==================================================================
Directory: /tmp/claude-0/-home-user-scacelith-chess/ea3bb550-e871-579e-a29d-78cac27b4b97/scratchpad/handgeo/
- handsdf.h: exact bone-local SDFs (palm + wrist ball, thenar + CMC ball, phalanges + bases, forearm). Copied from robot_hand.cpp / robot_body.cpp; include it after "character/robot_hand.cpp".
- shakepen.cpp: runs the real W/B handshake and reports, every 0.05 s, how deep each robot's hand/forearm mesh vertices go inside the other's SDFs, plus the relative pose at the clasp.
- clearance.cpp: minimal palm separation table.
- handgeo.cpp (+ out.txt): all the dimension tables above.
- Build from the repo root:

```
g++ -std=c++17 -O2 -w -Isrc -Ibuild/generated -o X X.cpp src/character/{skeleton,robot_body,robot_head,robot_model,sdf_mesh,sdf_volume}.cpp src/render/mesh.cpp src/gl/gl46.cpp build/libscacelith_core.a -lpthread
```

  handgeo also links src/character/robot_hand.cpp; the others include it.

Baseline, current code:
- Worst interpenetration 19.5 mm: RingR3 inside the partner's forearm cuff while closing (t = 0.85).
- During the whole clasp/pump (0.90-1.95 s) it stays at 11.9 mm. The palm (HandR) is inside the partner's thenar, with about 5000 vertices of each arm inside the other.
- Per pair at the clasp:
  - curled MiddleR2/R3, RingR2/R3 inside the partner ForeArmR 8.3-9.6 mm
  - MiddleR1/R2 inside the partner palm 5.7-6.3 mm
  - thumbs inside each other (ThumbR1/R2) 3-5.4 mm
  - partner forearm cuff inside the fingers up to 4.8 mm
- Relative pose at the clasp, B's hand in W's hand frame:
  - origin (+0.0300, -0.1038, +0.0083)
  - X_B = (-1, 0, 0), Y_B = (0, -0.987, 0.159), Z_B = (0, 0.159, 0.987)
  - So the fingers are anti-parallel (only 9° crossing), with origins 30 mm apart along the palm normal.
  - W's hand in the world: fingers (-0.292, -0.080, -0.953), thumb (-0.023, 0.997, -0.076), wrist (0.030, 1.008, 0.045).
  - Because the fingers are anti-parallel, each hand's MCPs land at the partner's y ≈ -0.018, so the curled fingers wrap the partner's wrist/forearm cuff (r ≈ 0.026) instead of the ulnar edge of the palm.
- Crossing angle α: B = A rotated 180° about Z_A, then α about X_A. α < 0 turns B's fingers towards A's pinky side. With both hands pitched down by φ, α = -2φ.
- Minimal origin separation along the palm normal for zero palm contact (clearance.cpp; B offset dy -0.06..-0.12, dz -0.01..+0.03):
  - Palm only: 26-31 mm. It is set by the hypothenars (+0.0154) and the dome.
  - Palm + thenar with the thumb CMC at rest: 35-51 mm, best about 36-37 mm at dy ≈ -0.10..-0.12. Set by the thenar (+0.025) against the partner's palm (+0.011).
  - Opposition moves the thenar further towards +X, which makes this worse.