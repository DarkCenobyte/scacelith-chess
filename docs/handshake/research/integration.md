# Game-side use of the handshake: research report

Nothing tracked was edited. I wrote scratch probes and screenshots only, under `/tmp/claude-0/-home-user-scacelith-chess/ea3bb550-e871-579e-a29d-78cac27b4b97/scratchpad/` (the `gamearea/` folder, plus `fp_clasp.png` and `side_clasp.png`). Coordinates are world space: metres, radians, +Y up, White's chair at +Z.

Side note: HEAD is already commit `70137ca` "Version 1.0.0-beta.2" (`README.md` and `cmake/version.cmake`). It sits on the old branch `claude/modest-lovelace-el0jck`, one commit ahead of `origin/master`. The new PR needs a fresh branch.

## 1. Every place a Handshake task is issued

**Fields the game sets.** All game calls use `task(anim::TaskType::Handshake)` from `src/game/game_scene_detail.h:16-25`, so every field is at its default:
- `position = (0,0,0)`: the animator uses its own clasp centre C (`src/anim/animator.cpp:453-459`).
- `duration = 0`: `Timing::Handshake` = 2.60 s (`src/anim/animator.h:53`).
- `notBefore = -1`, `tag = 0`.
- The partner is always the other seat: `h0.partner = &anim_[1]`, `h1.partner = &anim_[0]`.

**Ordering.** Both tasks are enqueued in the same `simulate()` frame, before the animators update. The animators then update in seat order, 0 then 1, each followed by `handleEvents` (`src/game/game_scene.cpp:1310-1315`). Code that reads the partner's globals mid-frame would therefore see White's new pose but Black's pose from the previous frame. `anim_` is a member array, so the `&anim_[x]` partner pointers stay valid across `initAnimators()`, which move-assigns the animators (`src/game/game_scene.cpp:292`).

**A. Opening handshake (Play, Coach including the lesson, Hot-seat, Watch, Replay)** — `src/game/game_scene.cpp:1253-1262`
- `State::Intro` starts at `stateTime_ = 0`. The handshake is enqueued when `stateTime_ >= kFadeIn*0.75` = 1.2 s (`kFadeIn` = 1.6, line 34), then the state becomes `Handshake`.
- `State::Handshake` (lines 1264-1266) calls `startPlaying()` once `stateTime_ > 0.3` and neither animator is busy, about 2.6 s later. Only then does the clock start (line 698), so offline the handshake costs no clock time.
- Timeline from scene start: handshake start ≈1.2 s, `HandshakeClasp` ≈2.12 s, `HandshakeRelease` ≈3.16 s, task end ≈3.80 s. The fade reaches 0 at about 1.6 s.
- `--no-intro` skips the opening handshake entirely: `FadeToGame` goes to `Handshake` without enqueuing tasks (line 1236), and the `--start`/`--replay` paths call `startPlaying()` directly (lines 248-251, 265-267).

**B. Opening handshake, online** — `src/game/game_scene.cpp:1243-1251`
- Enqueued at `stateTime_ >= kFadeIn*0.3` = 0.48 s, then `startPlaying()` is called at once. The server's clock runs during the handshake.
- `startRecording()` queues `PickPen` at the same moment. The left-handed robot's writing queue waits for the handshake to end (`src/anim/animator.h:200-205`).
- If the human touches a piece during the handshake, the `Reach` is queued after it (`src/game/game_scene.cpp:1599`), but `turn_` becomes `HumanTouched` immediately (see section 3).
- The remote robot's handshake can be cut by `cancelTasks()` (`src/game/game_scene_online.cpp:559`, `LiveStart::Cut` in `src/game/online_live.h:261`). The human's robot then finishes the handshake alone.
- With `--start-online … --no-intro`, `stateTime_` starts at `kFadeIn`, so the handshake fires on the first frame (lines 233-235).

**C. End-of-game handshake, all modes** — `src/game/game_scene.cpp:1275-1286` (`State::GameOver`)
- Conditions: `!endHandshakeDone_`, `stateTime_ > 0.8`, neither animator busy, neither writing hand busy, and (not coach, or `coachHandshakeWanted()`).
- `endGame()` (line 743) first has a held piece put back (lines 745-751) and calls `scorekeeper_.finishGame()` (Write result + PutPen, `src/game/scorekeeper.cpp:222-248`). In practice the pens are already down when the handshake starts.
- Card overlap: non-coach modes show the game-over card when `stateTime_ > 1.2 && endHandshakeDone_` (line 1143; online in `src/game/game_scene_online.cpp:1061`). The card (700×380, centred, `src/ui/ui_screens.cpp:1845`) therefore appears at 1.2 s, before the clasp at about 1.72 s. The clasp stays visible above the card.
- Replay: stepping back from the end card resets `endHandshakeDone_` (`src/game/game_scene_replay.cpp:252`), so the end handshake plays again when the replay reaches its end.

**D. Coach mode**
- `coachGameOver` (`src/game/game_scene_coach.cpp:787`) starts `gameEndScript`: a closing line with a Nod, then `"event.end.handshake"` ("Let's shake hands.") with an Open gesture and `skippable = false` (`src/coach/events.cpp:91-101`).
- `coachHandshakeWanted` (`src/game/game_scene_coach.cpp:817-821`):
  - true when there is no session;
  - false while table jobs run;
  - otherwise `session.handshakeWanted()` (`src/coach/session.cpp:874-879`, true once the end script is said), or the 45 s failsafe (line 60).
- After the handshake (`endHandshakeDone_`, both animators idle, `stateTime_ > 1.0`), `onHandshakeDone` triggers the appraisal (`src/game/game_scene_coach.cpp:1077-1083`). The end card waits for that (`coachEndCardReady`, lines 823-827), so in coach mode the card never covers the handshake.
- The coach's Open gesture uses its playing (left) hand. Lines 1051-1053 end it, and the game waits for `!busy()` before the handshake.

**E. Rules lesson**
- `"lesson.talk.handshake"` (`src/coach/lesson.cpp:907`; EN: "Games start and end with a handshake…") is speech only and issues no task.
- After the last chapter, `session.handshakeWanted()` is true for level 0 (`src/coach/session.cpp:877`). Then `endLesson()` (`src/game/game_scene_coach.cpp:1085` → 794-815) sets `GameOver`, and path C issues the handshake. The lesson also has the opening handshake A.

**Outside the game**
- Animation viewer: `src/anim/anim_viewer.cpp:958-966` (lefty demo at 0.5 s and 15.7 s, lines 998 and 1009) and 1192-1201 (default demo at 0.5 s).
- Its self-test, `src/anim/anim_viewer.cpp:2420-2466`, checks palm gap < 50 mm at the clasp (palm point = HandR local (±0.0135, −0.052, 0.003)), equal clasp times for both robots, and the pen put down before the clasp.

## 2. World geometry

Values from `src/game/layout.h` and `src/character/skeleton.cpp`, checked with a probe that links the real animator.

**Table and board**
- Table top y 0.760; table 1.20 (X) × 0.86 (Z), so its edges are at z = ±0.43.
- Board centre (0, 0.782, 0). Playing area ±0.22, frame ±0.25.
- Tallest piece top 0.877 (king, 0.095 m).

**Seats** (`src/game/game_scene.cpp:286-300`)
- Pelvis: White (0, 0.56, +0.60), Black (0, 0.56, −0.60).
- White faces −Z (`rootQ` = 180° about Y, `src/anim/animator.cpp:1318`), so White's right is +X. Black faces +Z with identity rotation, so Black's right is −X.
- Character space: +Z forward, +X = the robot's left.

**Shoulders and eyes**
- Right shoulder rest (character space) = (−0.19, 0.47, −0.02) relative to the pelvis.
- World: White R ≈ (0.19, 1.03, 0.62), Black R ≈ (−0.19, 1.03, −0.62). Measured at t = 0: (0.201, 1.023, 0.629) and (−0.178, 1.037, −0.597).
- Distance between right shoulders: 1.28 m at rest, 1.08 m at the clasp (each torso leans about 10 cm: White's shoulder moves to (0.173, 1.050, 0.521), Black's to (−0.169, 1.046, −0.502)).
- The shoulder line is 17.0° off the Z axis (atan 0.19/0.62).
- White's eye camera: (0.015, 1.189, 0.485) at rest, (−0.014, 1.160, 0.444) at the clasp.

**Clasp point C** (`src/anim/animator.cpp:453-459`)
- C = (midpoint of the two pelvises in x and z, BOARD_TOP_Y + 0.225) = **(0, 1.007, 0)**: 0.247 m above the table, 0.13 m above a king's top, directly above the board centre.
- Both animators compute the same C. The left-handed one uses `partnerPoint()` (`src/anim/animator_writing.cpp:259`).

**Measured pose at the clasp** (t = 0.925 s from task start; identical for R/R and R/mirrored-L, point-symmetric (x, z) → (−x, −z) about the vertical through C)

| | White (right) | Black (right) |
|---|---|---|
| Wrist (HandR) | (0.030, 1.008, 0.045) | (−0.030, 1.008, −0.045) |
| Elbow | (0.136, 0.901, 0.263) | (−0.138, 0.883, −0.252) |
| MCP (middle) | (0.007, 1.011, −0.040) | (−0.007, 1.011, 0.040) |
| Palm point | (0.001, 1.007, 0.000) | (−0.001, 1.007, 0.000) |
| Palm normal | (−0.956, 0, 0.293) | opposite |
| Finger direction | (−0.292, −0.080, −0.953), pitched down only 4.6° | mirrored |
| Thumb direction | (−0.02, 0.997, −0.08), straight up | mirrored |
| ThumbR3 tip | (−0.016, 1.067, −0.029) | (0.016, 1.067, 0.029) |
| Index tip | (−0.046, 1.029, −0.061) | (0.046, 1.029, 0.061) |

What this shows:
- The two palm points are 3.0 mm apart: the palms sit at the same point.
- The wrists are only 10.8 cm apart, so each hand's knuckles reach the partner's wrist (White MCP z = −0.040 vs Black wrist z = −0.045).
- The curled fingers end inside the partner's wrist and forearm: White's index tip is about 2.3 cm from Black's wrist joint.
- Both thumbs point up and cross 6 cm above C: the "X".
- During the pumps, palm y ranges 0.979–1.035 (oscAmp 0.032). The lowest finger joint is at 0.945, still above a king on e4/d5 (top 0.877).

**Mirroring** — `playsLeftHanded` (`src/game/hotseat.h:23`, `src/game/game_scene.cpp:100-103`); clock side chosen at `src/game/game_scene.cpp:494`.

| Mode | Clock | Right-handed (shakes with playing hand) | Left-handed (shakes with writing hand) |
|---|---|---|---|
| Play, Coach, Online | at the human's right | the human | Stockfish / coach / remote, always |
| Watch, Replay | +X | White | Black |
| Hot-seat | right of `clockRightOf` | that colour | the other colour |

- Exactly one robot is mirrored in every game configuration.
- Handshakes always use the real right hand (`src/anim/animator.h:260`; `shakeSide()` / `shakeHand()` in `src/anim/animator_impl.h:1464-1465`; `src/anim/animator.cpp:402-416`).
- For the left-handed robot that hand is its writing hand. It comes from the scoresheet side (pad at x = −sign(clock x)·0.45, |z| 0.318) and lays a held pen down first (`planHandshake`, t0 = 0.30 s). Meanwhile its playing (left) hand settles at its rest on the clock side, e.g. (0.353, 0.821, −0.341) when the clock is at +X.
- The right-handed human's playing-hand rest is (0.24, 0.76, 0.34). The torso follows the mirrored shaking hand (`src/anim/animator.cpp:800-814`).

## 3. First-person camera during the handshake

**Whose view.** `firstPersonSeat()` / `viewSeat()` (`src/game/game_scene_hotseat.cpp:48-59`) is the human, or in hot-seat `viewSeat_`, which is White at the opening (`src/game/game_scene.cpp:593`).

**Head override.** That robot's head is driven by `setHeadOverride(true, yaw, pitch)` (`src/game/game_scene.cpp:548` and 2324). The animator's handshake gaze (look at the partner's face, glance at the hands at shakeT 0.45–1.02, nod at the clasp; `src/anim/animator.cpp:989-1010`) is ignored for it (lines 824-826 and 837-840). The other robot gets `lookAt(other face)` (`src/game/game_scene.cpp:2399-2400`) plus the animator's own handshake gaze. The coach's `coachGazeTarget` is skipped in Intro/Handshake but applies in GameOver (line 2405); the handshake gaze overrides it at full weight anyway.

**Camera geometry** (`src/game/game_scene.cpp:2327-2346`)
- Camera = `eyeCameraTransform` plus eye offsets.
- Default pitch is `kBaseGazePitch` = −0.62 rad (−35.5°) from the body's forward (`src/game/game_scene.h:160`). FOV is 52° vertical (`src/game/game_scene.cpp:35`), about ±40.9° horizontal at 16:9; near plane 0.02.
- `canLook` is true in Intro, Handshake and GameOver (line 2236): right-drag, mouse-wheel lean (camera +0.11 m forward and −0.05 m down; the body does not move), and the look-up band (`src/game/look_up.h`, face pitch −0.275 rad).
- `coachFaceLift_` only applies in `State::Playing` (`src/game/game_scene_coach.cpp:1064`).
- In screenshot mode with no `--mouse`, the camera stays at −35.5°.

**Projection at the clasp, 1280×720, default look** (eye (−0.014, 1.160, 0.444), forward (0, −0.581, −0.814))

| Point | Screen position |
|---|---|
| C | ≈ (663, 141) px, about 20% from the top |
| Thumb tips | y ≈ 26–49 px, at the top edge |
| Own wrist | ≈ (719, 167) |
| Own elbow | ≈ (1011, 621) |
| Partner's elbow | ≈ (514, 179) |
| Partner's head and shoulders | above the frame (partner eyes at +35.5°, outside the ±26° half-FOV) |

- So the human sees its own right forearm rising from the bottom-right corner to the clasp, the partner's forearm coming down from the upper left, and both thumbs crossing at the top edge. This matches the bug screenshot.
- The palm plane is seen only 17° from edge-on. The eye is on White's palm side, so what faces the camera most is the **back of the partner's hand**. The human's own fingers wrapping the partner's ulnar edge onto its back, and the partner's thumb, are the most visible parts.
- The human's forearm passes about 3 cm over the f1 bishop and e1 king (forearm underside ≈0.90 vs piece tops 0.865/0.877). A lower elbow or forearm in a new pose could clip the pieces.

**Opacity (`armSeeThrough`).** The arm is see-through only in `State::Playing` while the human holds a piece (`src/game/game_scene.cpp:2583-2590`), fading over 0.2 s down to 25% opacity (`src/game/world.cpp:427`). During Intro, Handshake and GameOver it is **opaque**. Two exceptions:
- In the online opening (already `Playing`), touching a piece mid-handshake turns the shaking right arm into the ghost.
- The ghost is always the `playHand` arm, so a left-handed player's shaking arm never fades.

**Hidden parts.** In first person only the head, eyes, lids and neck top are hidden (`firstPersonHidden`, `src/character/robot.h:36`; `src/character/robot_render.cpp:48`). Arms and hands are always drawn.

**Viewer and replay views.** These do not use the head override (`src/game/game_scene.cpp:544-546`):
- viewpoint 0 shows White's eyes looking where the animator gazes;
- viewpoint 7 "duel" is at (1.75, 1.32, 0), aimed at (0, 1.02, 0), 42° FOV (line 2759);
- viewpoint 1 is beside the table.

## 4. What depends on the clasp geometry or timing

**Events**
- `HandshakeClasp` is consumed only at `src/game/game_scene.cpp:1993-1995`: seat 0 plays `Sfx::Handshake` at the fixed point (0, BOARD_TOP_Y + 0.22, 0) = (0, 1.002, 0).
- `HandshakeRelease`, `TaskStarted` and `QueueEmpty` have no consumer in `src/game` or `src/coach`.
- `Scorekeeper::onEvent` reacts only to the `PenPut` the handshake emits (`src/game/scorekeeper.cpp:295`).
- Audio's default position for Handshake is (0, 1, 0) (`src/audio/audio.cpp:656`); `tests/audio_tests.cpp:198` and 866-872 use hands at (0, 1, 0). None of these are tied to the animator's pose.

**Game flow** depends only on `busy()` / `writingBusy()` and the fixed 2.60 s:
- `State::Handshake` → `startPlaying` (line 1265);
- the coach's `onHandshakeDone` (`src/game/game_scene_coach.cpp:1078`);
- the card timing (lines 1143, and 1061 online).
- No code depends on where the hands are.

**Timing that must be kept**
- `Handshake` 2.60, `HandshakeClaspAt` 0.92, `HandshakeReleaseAt` 1.96 (`src/anim/animator.h:53-56`).
- The handshake gaze phases (`src/anim/animator.cpp:989-1010`).
- The pen put-down at t0 = 0.30 s.

**Tests**
- `tests/anim_tests.cpp`:
  - 526-560: page turn cut short; clasp exactly at start + 0.92 (1e-4).
  - 565-607: `PenPut` fires once, at its instant.
  - 612-648: gaze — deepest head pitch at 0.90 s ± 1/80 s (scale 1) and within 0.15 s of the clasp; the head is back down only after T − 0.5·scale; also tested with a 1.5× duration.
  - 710-768: cut at 1.2 s, right- and left-handed — hand back to rest within 2 mm after Retract + 0.05 s, head within 0.02 rad of an idle robot, chest jerk < 5 mm.
  - 773-838: cut while the pen is in hand — `PenPut` at start + 0.30, hand back within 2 mm.
- The anim viewer self-test (above): palm gap < 50 mm, and the clasp and pen checks.
- Coach tests only check session-level ordering: `tests/coach_session_tests.cpp:480-501` and 532-708, `tests/coach_director_tests.cpp:434`, `tests/coach_lesson_tests.cpp:569`, `tests/coach_catalog_tests.cpp:524-560`.
- No test reads `GameScene` handshake geometry.

**Other things a new pose touches**
- The online `setLean` on the remote robot can bring its chest forward up to about 11° during the handshake (`src/game/game_scene_online.cpp:832`), which changes its reach. `driveRemoteHead` can override its head.
- The left-handed hand's path from the scoresheet side passes over the board's corner pieces.
- At game end, any piece may stand under C.

## 5. Screenshot commands

Built binary: `build/scacelith`. The first frame bakes light probes (~20 s), and llvmpipe is slow under load. Rendered time = `--warp` + frames/60.

1. **First-person opening clasp (verified, ~3 min).** The output reproduces the bug view.
   ```
   tools/shot.sh game out.png 3 1280x720 --start --human white --warp 2.10
   ```
   This captures t ≈ 2.15 s, just after the clasp at ≈2.12 s. Other phases: `--warp 1.85` (approach), `2.5` (mid-pump), `3.15` (release).
   Result: `scratchpad/fp_clasp.png`.

2. **Detached side view of the same instant (verified).** The human's head is drawn (`headNearCamera`, `src/game/game_scene.cpp:2528-2535`):
   ```
   … --start --human white --warp 2.10 --cam 0.75,1.12,0.0 --look 0,1.0,0 --fov 30
   ```
   Result: `scratchpad/side_clasp.png`, showing the crossed thumbs and overlapping hands.

3. **End-of-game clasp (verified, 640×360, ~15 min).**
   ```
   tools/shot.sh game out.png 44 640x360 --start --human white --no-intro --moves f2f3,e7e5,g2g4,d8h4 --warp 1.0
   ```
   - Outside coach mode, `runWarp` stops warping 1.0 s into `GameOver` (`src/game/game_scene.cpp:1212-1214`), so extra frames are needed: the handshake starts at about 0.817 s and clasps at about 1.737 s.
   - The card is on screen from 1.2 s.
   - Result: `scratchpad/gamearea/end_clasp.png`. Same geometry as the opening handshake.

4. **Not run:**
   - Coach game, same timing: `--start --coach --coach-level 1 --coach-colour white --warp 2.10`. Same geometry; the coach is left-handed.
   - Viewer side view: `--viewer --viewpoint 7 --warp 2.10`.
   - Anim viewer close-up: `tools/shot.sh anim out.png 2 1280x720 --demo lefty --time 1.45 --view shake --robot`. The shake camera is at (0.42, 1.08, 0.02) aimed at (0, 1.0, 0), 34° (`src/anim/anim_viewer.cpp:1500`).

Probe sources: `scratchpad/gamearea/hsprobe.cpp` (poses at each phase; pass `lefty` to mirror Black) and `scratchpad/gamearea/hsrange.cpp` (pump range and palm gap). Build with:
```
g++ -std=c++17 -O1 -Isrc … src/anim/animator*.cpp src/character/skeleton.cpp src/math/math.cpp src/core/log.cpp
```