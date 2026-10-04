# The robots' handshake: status, measurements, remaining work

1.0.0-beta.2 replaced the handshake that clipped (the two hands antiparallel at 9°, the fingers
closed inside the partner's wrist and forearm cuff, up to 24 mm deep, the thumbs crossed in an X)
with a crossed clasp fitted offline on the exact meshes. This page is the hand-over for the work
that was left: what the branch holds, what was measured and tried, and what is still to do.
`tools/handshake/` holds the measuring and fitting tools (see its README); `research/` holds the
notes written before the change (they describe the old code: line numbers and the "why it clips"
analysis refer to it; scratch paths in them no longer exist, the tools they name are now in
`tools/handshake/`); `alternative-grasp-solver.patch` is the design that lost (see below).

## What the code does now

`planHandshake` (`src/anim/animator.cpp`) with the constants and finger presets of
`src/anim/animator_impl.h` (`kShakePitch`, `kShakeYaw`, `kShakeElbow`, `kShakePump`,
`shakeAnchor()`, `shakeSlide()`, `poseShakeReach/Open/Grip`, `kShakeLetGo*`):

| Phase (× duration scale) | What happens |
|---|---|
| 0 – 0.60 s | reach out, hand open, thumb up (`poseShakeReach`), elbow rising |
| 0.60 – 0.78 s | slide in along the palm plane, open (`poseShakeOpen`), slowing into the contact |
| 0.78 – 0.92 s | close the grip once the palms touch; `HandshakeClasp` at 0.92 s |
| 0.92 – 1.80 s | two pumps |
| 1.80 – 1.96 s | open in place; `HandshakeRelease` at 1.96 s marks the open hand |
| 1.96 – 2.10 s | withdraw along the slide, gathering speed |
| 2.10 – 2.60 s | back to rest (`shakeRetract`), continuing from the withdraw's speed |

A handshake cut short (`cancelTasks`: an online opponent's move during the opening handshake)
goes through `shakeLetGo`: both robots, from the same instant and their own plans only, open the
hand in 0.14 s while it goes on as planned, then back off along the slide; the robot that was cut
is back at rest 0.40 s after the cut.

The two seats are a half turn apart about the vertical through the clasp point, so the partner's
hand is known in one's own hand frame without reading its motion: the plan uses only the clasp
point, the robot's own pelvis and the constants. A left-handed robot's shaking hand is the exact
mirror of a right-handed one's in contact (0.000 mm on every bone).

## Measurements (exact meshes, every 1/120 s of the whole task)

Configurations: rr (right vs right), rl / lr (one robot left-handed, mirrored), rlpen (the
left-handed robot still holding its pen), cut (cut short at 0.70, 0.90, 1.2, 1.85 and 2.0 s).

| Criterion | Before (1.0.0-beta.1) | Now | Target |
|---|---|---|---|
| Worst interpenetration rr / rl / lr / rlpen | 24.35 / 24.35 / – / 20.9 mm | 0.75 / 0.97 / 0.96 / 0.96 mm | ≤ 1.0 mm |
| … per phase: approach, slide, close, pumps, open, withdraw, retract | rr: 0, 5.2, 24.4, 11.9, 12.5, 7.8, 0 (rl retract 5.7) | rl: 0, 0, 0.71, 0.97, 0.70, 0, 0 | ≤ 1.0 mm |
| Cut short mid-grip | 18 – 20 mm for ~0.1 s | ≤ 0.97 mm | ≤ 1.0 mm |
| Crossing of the hands | 9.2° | 50.5° | 35 – 65° |
| Palm gap | −0.4 mm (overlap) | 1.07 mm | ≤ 1 mm |
| Distal pads to the partner's hand, index / middle / ring / pinky | in the cuff | 4.37 / 1.86 / 2.52 / 1.94 mm | ≤ 2 mm |
| Distal phalanges' skin gap, same order | – | −0.38 (on the cuff) / 1.89 / 0.77 / −0.10 mm | ≤ 2 mm |
| Pads behind the partner's palm mid-plane, along its hand | up to 12.9 mm into the cuff | 10.9 – 11.6 mm behind, wrist + 17.5 mm to knuckles − 0.4 mm | ≥ 10 mm, wrist + 15 mm .. knuckles |
| Thumb pad to the partner's back / to its index knuckle | thumbs crossed, 4.6 mm apart | 0.94 mm / 39.9 mm | ≤ 3 mm / near |
| Thumb IP (hook) / dark CMC ball exposed | – | 0.95 rad / 67 % | ~0.9 / small |
| Thumb metacarpals apart / webs apart | 4.6 / 18.1 mm | ~64 / 17.2 mm | ≥ 20 / ≤ 25 mm |
| Wrist strain in contact (with the elbow lift) | 0.028 – 0.043 | 0.029 (0.030 with the pen) | < 0.04 |
| IK clamp in contact | 0.165 rad (rlpen) | 0 | 0 |
| Fastest bone | 2228°/s, 12 frames over 1800 (rlpen) | 1626°/s | ≤ 1800°/s |
| Lowest hand point at the pump bottom | 0.938 m | 0.906 m | ≥ 0.90 m |
| Hand speed into the contact | 0.32 m/s | 0.62 m/s, slowing to 0 | ≤ 0.7, smooth |
| Way back to rest, peak | 2.15 m/s | 2.10 m/s (2.15 left-handed), one rise and fall | ≤ 2.2, continuous |
| Cut robot's way back, peak | – | 4.6 – 5.7 m/s | (see below) |

Tests: `./build/scacelith_tests` 738 passed, 0 failed (11 skipped); the anim viewer self-test
(`--scene anim --selftest`) passes its new crossed-clasp check ("held: palms facing −1.000,
mid-planes 27.4 mm apart, crossing 51°, pads 10.9 mm behind, wrist + 17.5 mm, knuckles − 12.9 mm,
webs 17.2 mm"); `anim_handshake_hands_clasp_without_going_through` checks the clasp with capsules
on the bones, calibrated against the exact meshes (the old handshake fails 12 of its checks).

## Remaining work, most important first

1. **Independent verification was skipped** (cost). What was checked: the tests above, the
   self-test, the exact-mesh numbers, three renders at t = 1.80 s (shakex, shaked, side). Not
   done: an adversarial look at renders across the whole motion (1.10 – 2.80 s in the anim viewer,
   every close-up view), the in-game first-person handshake of the final version
   (`tools/shot.sh game out.png 3 1280x720 --start --human white --warp 2.10`), and a code review
   of the five commits after "Handshake: the hands clasp without going through each other".
2. **Contact polish (needs a new fit).** The index pad stays 4.4 mm off the partner's back while
   its tip touches the partner's forearm cuff, which is also the worst interpenetration (0.97 mm,
   only 0.03 mm under the limit, rl/lr/rlpen and cut, during the pumps); ring pad 2.5 mm; palms
   1.07 mm apart; the thumb's dark CMC ball shows (67 %). A refit should aim at a worst case
   ≤ 0.7 mm. The interrupted next attempt was seeded from g5 with a wider DIP range and stronger
   skin / palm terms: `BOUNDS="dip:-0.6:0.4" W_SKIN=200 W_PALM=200 W_BALL=100
   tools/handshake/fit.sh fit g6.txt tools/handshake/fits/g5.txt 16 900 0.6`.
3. **Cut-short return speed.** The robot that was cut goes back to rest at up to 4.6 – 5.7 m/s
   (`kShakeLetGoQuick` = 0.40 s, to stay within Timing::Retract). A longer way back (0.55 – 0.6 s)
   needs the next task to wait for it (as the writing hand's `wr.suspendUntil` does) and the cancel
   tests' chest step re-checked.
4. **Raised elbow.** `kShakeElbow` 0.85 rad: the wrist's ulnar-deviation limit makes a ~50°
   crossing impossible with the elbow down and the hand above 0.90 m. In first person the player's
   forearm covers the h-file and the clock during the handshake. Options: a smaller crossing (≥ 35°)
   with a lower hand, or a wider wrist range.
5. **Thumb placement.** The thumb pad lies on the partner's back 40 mm from its index knuckle (the
   references put it near the knuckle); the CMC lower bound (−0.40, to hide the dark ball) and the
   partner's thumb limit it.
6. **`relaxWrist` and the pen.** It skips the contact segments (`Segment::locked`) but not the
   pen-locked ones (`sg.lockFrom` / `lockTo`); the correction measured 0° in the default case, so
   this is a guard only (the losing design had it).
7. **Tests.** Extend the capsule test with the pad-to-skin gaps and a cut-short case.
8. **Renderer.** A speckled grey patch shows in the crevice between the palms (shading noise, not
   clipping; it was there with both designs).

## What was tried

**Two designs competed**, each in its own worktree, judged on the exact meshes and renders:

| | Plan-time grasp solver (lost; `alternative-grasp-solver.patch`) | Offline fit, baked constants (kept) |
|---|---|---|
| Idea | Partner's hand modelled analytically in one's own hand frame (palm superellipse, cuff cone, thenar capsules); fingers curl until contact; thumb searched on a grid | CMA-ES over 30 parameters on the exact SDFs (pitch, yaw, anchor, elbow lift, slide, pump, grip and open poses), baked as constants |
| Worst interpenetration | 0.00 – 0.11 mm | 0.56 mm |
| Crossing | 59.6° | 49.7° |
| Arm | straight, level at shoulder height (elbow lift 1.20 rad) | V-shaped (lift 0.85 rad) |
| Thumb | at the partner's wrist seam, 65 mm from its index knuckle | on its back, 34 mm from the knuckle |
| Plan time | 19 – 24 ms per robot | 0.4 ms |
| Judge's scores (penetration, realism, arm, motion, constraints, code) | 10, 7.5, 4, 7, 9, 5.5 | 9, 7, 6.5, 4.5 → ~7 with the timing fix, 9.5, 7.5 |

Lowering the solver's elbow failed: lift 0.8 put the left-handed robot's wrist on its limit and
fingers 3.98 mm into fingers; pitch 0.45 with lift 0.9 put the index 1.7 – 2.3 mm into the
partner's forearm and the thumb search found nothing. Its 60° clasp needs the raised, straight arm.
Ideas worth grafting from it: `curlOnto` as a capped final pass (+0.25 rad per joint, 0.8 mm gap),
and the `relaxWrist` pen-lock skip.

**Timing.** Sliding in over 0.68 – 0.76 s hit the contact at 1.38 m/s and stopped dead within
40 ms; 0.60 – 0.78 s gives 0.62 m/s, slowing to zero, and no interpenetration in the slide. The old
release shot the hand back (0 → 1.10 m/s in 50 ms), nearly stopped, then rushed home at 2.36 –
2.5 m/s; the release now opens in place, withdraws in 0.14 s gathering speed, and hands that speed
to the way back.

**Contact refits** (`tools/handshake/fits/`, static metrics from `opt9 show`; o3 is the open thumb
of the arrival step, already baked):

| Fit | Pads to skin I / M / R / P (mm) | Thumb pad | IP | Palm gap | CMC ball | Notes |
|---|---|---|---|---|---|---|
| before (committed with the design) | 5.33 / 3.17 / 3.14 / 4.73 | 3.06 | 1.30 | −0.17 | 57 % | worst 0.56 mm |
| g1 | 5.30 / 2.31 / 2.84 / 3.62 | 2.40 | 1.30 | 1.79 | 55 % | no better |
| g2 | 3.90 / 1.68 / 2.19 / 1.93 | 0.49 | 0.94 | 3.08 | 56 % | palms apart |
| g3 | 4.66 / 1.55 / 0.03 / 4.18 | 0.66 | 0.90 | 1.16 | 67 % | pinky pad past the knuckles |
| g4 | 4.83 / 2.03 / 2.79 / 1.91 | 1.25 | 0.98 | 0.93 | 67 % | whole task 1.04 mm: rejected |
| **g5 (baked)** | 4.37 / 1.86 / 2.52 / 1.94 | 0.94 | 0.95 | 1.07 | 67 % | whole task 0.97 mm |

## Reproducing

```sh
tools/handshake/mk.sh pen && tools/handshake/mk.sh opt9          # into build/handshake/bin
PHASES=0.60,0.78,0.92,1.80,1.96,2.10 build/handshake/bin/pen rl exact   # whole task, exact SDFs
tools/handshake/measure.sh now        # rr, rl, lr, rlpen and the cut cases into build/handshake/now/
tools/handshake/fit.sh show tools/handshake/fits/g5.txt          # a fit's static metrics
tools/handshake/fit.sh fit out.txt tools/handshake/fits/g5.txt 16 900 0.6 > out.log   # CMA-ES from g5
python3 tools/handshake/bake.py out.txt out.log                   # bake into src/anim/animator_impl.h
tools/handshake/shots.sh shots shakex,shaked,side 1.42,1.80,2.55  # renders (anim viewer, Xvfb)
```
