# The robots' handshake: status, measurements, remaining work

1.0.0-beta.2 replaced the handshake that clipped (the two hands antiparallel at 9°, the fingers
closed inside the partner's wrist and forearm cuff, up to 24 mm deep, the thumbs crossed in an X)
with a crossed clasp fitted offline on the exact meshes, and left a list of unfinished items. A
second pass finished them: an independent review with renders of the whole motion, a new contact
fit (g7), a slower way back for a handshake cut short, the tests, and the grey speckle between the
palms. This page says what the code does, what was measured and tried, and what is still open.
`tools/handshake/` holds the measuring and fitting tools (see its README); `research/` holds the
notes written before the change (they describe the old code: line numbers and the "why it clips"
analysis refer to it; scratch paths in them no longer exist, the tools they name are now in
`tools/handshake/`); `alternative-grasp-solver.patch` is the design that lost (see below).

## What the code does now

`planHandshake` (`src/anim/animator.cpp`) with the constants and finger presets of
`src/anim/animator_impl.h` (`kShakePitch`, `kShakeYaw`, `kShakeElbow`, `kShakePump`,
`shakeAnchor()`, `shakeSlide()`, `poseShakeReach/Open/Grip`, `kShakeLetGo*`, `kShakeBackBefore`):

| Phase (× duration scale) | What happens |
|---|---|
| 0 – 0.60 s | reach out, hand open, thumb up (`poseShakeReach`), elbow rising |
| 0.60 – 0.78 s | slide in along the palm plane, open (`poseShakeOpen`), slowing into the contact |
| 0.78 – 0.92 s | close the grip once the palms touch; `HandshakeClasp` at 0.92 s |
| 0.92 – 1.80 s | two pumps |
| 1.80 – 1.96 s | open in place; `HandshakeRelease` at 1.96 s marks the open hand |
| 1.96 – 2.10 s | withdraw along the slide, gathering speed |
| 2.10 – 2.60 s | back to rest (`shakeRetract`), continuing from the withdraw's speed |

A handshake cut short (`cancelTasks`: an online opponent's move during the opening handshake; in
the game only the remote robot is cancelled, the local one follows):

- In contact (from the slide in until the hand opens as planned), both robots, from the same
  instant and their own plans only, open the hand in 0.14 s while it goes on as planned, then back
  it off along the slide (`shakeLetGo`). The robot that was cut does that in 0.12 s and is back at
  rest 0.70 s after the cut (`kShakeLetGoQuick`), at most 2.2 m/s (2.5 m/s for a left-handed
  player's writing hand); the partner (`followPartnerCut`) takes 0.14 s and is back 0.88 s after
  the cut (by the end of its own handshake at the latest), and its clasp and release events no
  longer fire.
- Before the hands meet, each goes back to rest from where its hand is, in at least 0.45 s
  (`kShakeBackBefore`).
- Once a hand opens as planned (or has already let go after the partner's cut), it goes on as
  planned.
- A left-handed robot still laying its pen down lays it down first, as planned (`PenPut` at its
  own instant), whichever robot was cut.
- Next tasks: a Retract starts at once and keeps that way back (it lasts until the hand is at its
  rest); any other task of a right-handed robot starts once its hand is out of the partner's
  (`shakeClearAt`); a left-handed robot's writing queue waits until the hand is at its rest
  (`wr.suspendUntil`).

The two seats are a half turn apart about the vertical through the clasp point, so the partner's
hand is known in one's own hand frame without reading its motion: the plan uses only the clasp
point, the robot's own pelvis and the constants. A left-handed robot's shaking hand is the exact
mirror of a right-handed one's in contact (0.000 mm, 0.001 rad on every bone, `mirrorcmp`); its
torso and forearm are not (about 0.12 rad apart), which is why the fit checks the grip against the
partner's forearm turned ±0.13 rad (`W_CUFF`).

## Measurements (exact meshes, every 1/120 s of the whole task)

Configurations: rr (right vs right), rl / lr (one robot left-handed, mirrored), rlpen (the
left-handed robot still holding its pen), cut (cut short at 0.70, 0.90, 1.2, 1.85 and 2.0 s, each
configuration).

| Criterion | Before (1.0.0-beta.1) | 1.0.0-beta.2 (fit g5) | Now (fit g7) | Target |
|---|---|---|---|---|
| Worst interpenetration rr / rl / lr / rlpen | 24.35 / 24.35 / – / 20.9 mm | 0.75 / 0.97 / 0.96 / 0.96 mm | 0.51 / 0.66 / 0.66 / 0.65 mm | ≤ 0.7 mm |
| … per phase (rl): approach, slide, close, pumps, open, withdraw, retract | rr: 0, 5.2, 24.4, 11.9, 12.5, 7.8, 0 (rl retract 5.7) | 0, 0, 0.71, 0.97, 0.70, 0, 0 | 0, 0.12, 0.43, 0.66, 0.42, 0, 0 | ≤ 0.7 mm |
| Cut short | 18 – 20 mm for ~0.1 s | ≤ 0.97 mm | ≤ 0.66 mm, no pops | ≤ 0.7 mm |
| Crossing of the hands | 9.2° | 50.5° | 50.7° | 35 – 65° |
| Palm gap | −0.4 mm (overlap) | 1.07 mm | 0.84 mm | ≤ 1 mm |
| Distal pads to the partner's hand, index / middle / ring / pinky | in the cuff | 4.37 / 1.86 / 2.52 / 1.94 mm | 3.54 / 1.54 / 2.22 / 1.74 mm | ≤ 2 mm |
| Distal phalanges' skin gap, same order | – | −0.38 (on the cuff) / 1.89 / 0.77 / −0.10 mm | 1.39 / 1.41 / 1.64 / 1.36 mm | ≤ 2 mm |
| Pads behind the partner's palm mid-plane, along its hand | up to 12.9 mm into the cuff | 10.9 – 11.6 mm behind, wrist + 17.5 mm to knuckles − 0.4 mm | 11.0 – 12.1 mm behind, wrist + 19.2 mm to knuckles − 0.4 mm | 10 – 14 mm, wrist + 15 mm .. knuckles |
| Thumb pad to the partner's back / to its index knuckle | thumbs crossed, 4.6 mm apart | 0.94 mm / 39.9 mm | 1.14 mm / 46.0 mm | ≤ 3 mm / near |
| Thumb IP (hook) / dark CMC ball exposed | – | 0.95 rad / 67 % | 1.00 rad / 61 % | ~0.9 / small |
| Thumb metacarpals apart / webs apart | 4.6 / 18.1 mm | ~64 / 17.2 mm | 58.5 / 18.3 mm | ≥ 20 / ≤ 25 mm |
| Elbow lift in contact (`kShakeElbow`) | – | 0.850 rad | 0.843 rad | (see below) |
| Wrist strain in contact (with the elbow lift) | 0.028 – 0.043 | 0.029 (0.030 with the pen) | 0.030 (0.031 with the pen) | < 0.04 |
| IK clamp in contact | 0.165 rad (rlpen) | 0 | 0 | 0 |
| Fastest bone | 2228°/s, 12 frames over 1800 (rlpen) | 1626°/s | 1625°/s | ≤ 1800°/s |
| Lowest hand point at the pump bottom / pump amplitude | 0.938 m | 0.906 m / 28.6 mm | 0.907 m / 27.0 mm | ≥ 0.90 m |
| Hand speed into the contact | 0.32 m/s | 0.72 m/s (the page said 0.62: measured before g5) | 0.68 m/s, slowing to 0 | ≤ 0.7, smooth |
| Way back to rest, peak | 2.15 m/s | 2.10 m/s (2.15 left-handed) | 2.10 m/s (2.22 left-handed), one rise and fall | ≤ 2.2, continuous |
| Cut robot's way back, peak | – | 4.6 – 5.7 m/s | 2.19 – 2.53 m/s (the partner 1.75 – 2.22) | ≤ 2.7 |

Tests: `./build/scacelith_tests` 739 passed, 0 failed (11 skipped) on Linux; 734 passed, 0 failed
(12 skipped) under Wine (`tools/test_win.sh`). The anim viewer self-test (`--scene anim
--selftest`) passes its crossed-clasp check ("held: palms facing −1.000, mid-planes 27.0 mm apart,
crossing 51°, pads 10.9 mm behind, wrist + 19.2 mm, knuckles − 12.6 mm, webs 18.3 mm"; the
mid-planes' distance is now checked frame by frame against both bounds).
`anim_handshake_hands_clasp_without_going_through` checks the clasp with capsules on the
bones, calibrated against the exact meshes (the old handshake fails 12 of its checks), and now
also that every long fingertip pad stays 10 – 14 mm behind the partner's mid-plane, on the back of
its hand and not off it (11.1 / 11.4 / 11.3 / 12.1 mm at most).
`anim_cancel_handshake_lets_go_of_the_partner` cuts a handshake at 0.40, 1.2, 1.85 and 2.05 s,
right- and left-handed, and checks both robots (rest reached, at most 2.7 m/s, capsule clearances,
events, gaze, no jump of the chest); `anim_cancel_handshake_lays_the_pen_down` covers a cut by the
partner while the pen is laid down.

## The second pass

1. **Independent verification.** A review of the five commits after "Handshake: the hands clasp
   without going through each other" found seven issues, all fixed: a cut during the withdraw made
   the elbow jump and the wrist flick back (the let-go now keeps the elbow it starts from, and a
   cut while the plan lets go changes nothing); a robot cancelled after following its partner's
   cut lunged back to the clasp (it now keeps its way back); the partner ignored a pen it was
   laying down; a Retract could take over another task's motion (`keepsCutWayBack` /
   `durationFrom`); `HandshakeRelease` still fired after a cut; a right-handed robot cut before the
   contact played the rest of the handshake alone; the `writingSpine` ease leaned every writing
   hand (now only while a left-handed robot shakes hands). Its test findings (the self-test's
   mid-plane bounds, the partner never checked in the cancel test) and stale comments are fixed
   too. Renders, all of the final version: the seven close-up views and the side view at 1.10,
   1.45, 1.80, 2.20 and 2.60 s in the anim viewer (shakex, shaked and shakeq at 2.40 and 2.80 s
   too; `tools/handshake/shots.sh`, played into each instant), and the game's first-person view at
   1.40, 1.80, 2.10 and 2.50 s (`tools/shot.sh game out.png 3 1280x720 --start --human white
   --warp 2.10`). They show a clean clasp from the slide in to the release, with no
   visible interpenetration, the fingers on the back of the partner's hand and the thumb across
   it, and a smooth reach and way back. In first person the opponent's arm swings out wide during
   the reach (the V-shaped arm with the raised elbow), and the player's forearm crosses the h-file
   and the clock from the clasp to the release (item 4). One renderer residual is left (item 8).
2. **Contact refit (g7).** Seeded from g6 (the interrupted attempt, finished: g5 with a wider DIP
   range and stronger skin / palm / ball terms) with two new cost terms: the grip against the
   partner's forearm cuff where the pumps put it and turned ±0.13 rad, as a left-handed partner's
   is (`W_CUFF`; the worst case had always been the index tip in a left-handed partner's cuff), and
   the slide's length (`W_SLIDEV`, the speed into the contact). Worst case 0.66 mm over every
   configuration and cut (was 0.97), palms 0.84 mm apart, every distal phalanx 1.4 – 1.6 mm off the
   partner's skin, the dark CMC ball 61 % exposed (was 67 %). The index pad still hovers 3.5 mm
   over the partner's back near its wrist, where the hand narrows into the cuff (its distal
   phalanx is 1.4 mm off the partner's skin).
3. **Cut-short return speed.** 2.19 – 2.53 m/s (was 4.6 – 5.7): the robot that was cut takes
   0.70 s back to rest and its next task waits as described above, instead of squeezing into
   Timing::Retract.
4. **Raised elbow: kept (0.843 rad).** Refits with the elbow lift capped at 0.55, 0.60, 0.65 and
   0.70 rad (crossing target 40 – 44°): below 0.70 the wrist strain goes over 0.04 in the pumps
   (0.049 at 0.60, 0.058 at 0.55); at 0.70 (`fits/l70.txt`) everything holds (worst 0.70 mm,
   strain 0.035) but the thumb moves 55 mm from the partner's index knuckle and the pumps shrink to
   19 mm, and the first-person view barely changes: the forearm still crosses the h-file, and the
   clock only shows with the elbow at 0.4 rad or lower, far over the strain limit (0.058 already at
   0.55). So the raised elbow is the price of the ~50° crossing with this wrist; freeing the
   first-person view needs a wider ulnar-deviation range for the wrist (which changes every other
   gesture) or a flatter clasp.
5. **Thumb placement.** 46 mm from the partner's index knuckle (was 40). A stronger pull towards
   the knuckle reaches 33 – 38 mm only by giving up the rest (0.7 mm interpenetration in the static
   grip alone, strain 0.042 – 0.043, twice the cost or more): the CMC lower bound (−0.40, which
   hides the dark ball) and the partner's thumb hold it back.
6. **`relaxWrist` and the pen.** It now skips the pen-locked segments too (a guard: the correction
   measured 0°).
7. **Tests.** See above.
8. **Renderer: the speckled grey patch.** It was the ambient occlusion (GTAO, half resolution, two
   slices) without its temporal accumulation, for two reasons that only the anim viewer had: its
   robots had no motion vectors of their own (the GTAO, TAA and SSR history was reprojected with
   the camera only, so on a moving hand it was rejected every frame), and its frozen 8-frame
   screenshots stop before the accumulation converges (48 frames are needed). The viewer now passes
   last frame's bones as the game does, and `shots.sh` plays into each instant; the crevice is clean
   through the clasp and the pumps. The game always had both. A residual remains in close-ups while
   the hands open (about 0.15 s): a blotchy grey on the palms, the GTAO's two-slice estimate under
   an occluder that moves away. It goes with the ambient occlusion off, not with the reflections
   off; a tighter history clamp on moving pixels or a wider filter where the history is rejected
   did not change it. At the game's distances it covers a few pixels, so the renderer is unchanged.

## Still open

- **Thumb nearer the knuckle** (item 5): needs a thumb CMC range that shows more of the dark ball,
  or a different thumb socket in the mesh.
- **First-person view during the handshake** (item 4): the player's forearm covers the h-file and
  the clock for about two seconds at the start of the game; a design decision (wrist range, flatter
  clasp, or a see-through arm as when a piece is carried).
- **Ambient occlusion while the hands open** (item 8): more GTAO slices, or a history that follows
  the occluder, would clean the close-ups; both cost more than the few pixels they fix in play.
- **Durations other than the default** (review finding, low): the let-go uses fixed times while the
  plan scales; below a duration scale of about 0.36 the partner's way back would outlast its task.
  The game always uses the default length.

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
Ideas worth grafting from it: `curlOnto` as a capped final pass (+0.25 rad per joint, 0.8 mm gap);
its `relaxWrist` pen-lock skip is in now.

**Timing.** Sliding in over 0.68 – 0.76 s hit the contact at 1.38 m/s and stopped dead within
40 ms; 0.60 – 0.78 s gives 0.62 m/s (0.68 m/s with g7's slide), slowing to zero, and no
interpenetration in the slide. The old
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
| g5 (1.0.0-beta.2) | 4.37 / 1.86 / 2.52 / 1.94 | 0.94 | 0.95 | 1.07 | 67 % | whole task 0.97 mm |
| g6 | 3.77 / 1.45 / 2.10 / 2.00 | 0.79 | 0.92 | 0.86 | 61 % | the interrupted attempt, finished (seed 17): whole task 0.93 mm, the index tip in a left-handed partner's cuff |
| l70 | 4.08 / 1.74 / 1.19 / 0.56 | 1.09 | 1.01 | 0.69 | 59 % | elbow lift ≤ 0.70: whole task 0.70 mm, thumb 55 mm from the knuckle, 19 mm pumps |
| **g7 (baked)** | 3.54 / 1.54 / 2.22 / 1.74 | 1.14 | 1.00 | 0.84 | 61 % | from g6 with `W_CUFF`, `W_SLIDEV`: whole task 0.66 mm |

## Reproducing

```sh
tools/handshake/mk.sh pen && tools/handshake/mk.sh opt9          # into build/handshake/bin
PHASES=0.60,0.78,0.92,1.80,1.96,2.10 build/handshake/bin/pen rl exact   # whole task, exact SDFs
tools/handshake/measure.sh now        # rr, rl, lr, rlpen and the cut cases into build/handshake/now/
tools/handshake/fit.sh show tools/handshake/fits/g7.txt          # a fit's static metrics
W_CUFF=0 W_SLIDEV=0 BOUNDS="dip:-0.6:0.4" tools/handshake/fit.sh fit g6.txt tools/handshake/fits/g5.txt 17 900 0.6 > g6.log
BOUNDS="dip:-0.6:0.4,lift:0:0.85" tools/handshake/fit.sh fit g7.txt g6.txt 32 700 0.4 > g7.log   # CMA-ES from g6
python3 tools/handshake/bake.py tools/handshake/fits/g7.txt tools/handshake/fits/g7.out   # bake into src/anim/animator_impl.h
tools/handshake/shots.sh shots shakex,shaked,side 1.45,1.80,2.20  # renders (anim viewer, Xvfb)
```
