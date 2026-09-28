# Offline multiplayer (hot-seat) plan

Two people play on one PC, in turn, each from their own robot's eyes. After a move is completed
(the clock pressed), the camera leaves the mover's eyes and flies over the table into the
opponent's eyes. During the flight the clock is frozen. Nothing here is networked.

This document describes the design, what already exists (seats, clock freeze, camera flights,
the viewer mode's `--handover-preview`), and the work that remains.

## Flow of a turn

1. The player to move (seat S) plays as in a game against Stockfish: touch, place, press the
   clock with the hand on the clock side.
2. `onClockPressed(S)`: the arbiter validates the move, `clock_.press()` switches the clock (the
   increment is added to S at that moment), `game_.play()` records the move.
3. `setClockFrozen(true)`, then the camera starts `CameraFlight` from S's eye pose to the eye pose
   of the other seat O, with `CameraFlight::handoverShape()` and `kHandoverDuration` (1.6 s).
4. During the flight nothing counts and nobody acts: `updatePlaying()` returns early, so the clock
   neither runs nor consumes the delay window, no flag can fall, and the input is ignored.
5. When the flight lands (`!observer.flying()`), `setClockFrozen(false)`: O's clock (already the
   running side since step 2) starts counting, and O has the controls.

In the viewer, `--viewer --handover-preview` (or key **0** during a watched game) already runs
steps 3 to 5 between two Stockfish players, with the same code.

## The camera path

`CameraFlight` interpolates a `CameraPose` (position, yaw, pitch, roll, field of view) along a
cubic Bezier curve, eased with smootherstep, so the flight starts and ends at rest.

- **Rise.** The first control point sits 0.55 m above the mover's eyes and 0.22 m towards the
  board centre: the camera leaves the head upwards and slightly forwards, never backwards through
  the skull.
- **Arc.** The two control points are symmetric, so the middle of the curve passes about 0.4 m
  above the eye line, over the board. Meanwhile the pitch dips by 28 degrees (`pitchDip`), so the
  view looks down at the position at mid-flight, and the yaw turns half a turn, always in the same
  direction (`yawTurn = +1`, counter-clockwise seen from above). Yaw, pitch and roll are
  interpolated separately, so the horizon stays level and the view never flips over. A
  quaternion slerp between two views 180 degrees apart would roll or pass through the vertical.
- **Settle.** The camera comes down into the opponent's head from above and slightly in front,
  and ends on its eye pose (roll included, the head may be tilted). The robot's head moves during
  the flight, so the end pose is retargeted every frame (`CameraFlight::retarget()`).

The robot whose head holds the camera is drawn without its head (`headNearCamera()` in
`GameScene`). While the camera flies, both robots are drawn complete.

Options to offer: the flight length (0.8 to 2 s), or an instant cut with a short fade to black for
players who dislike camera motion.

## The clock during the handover

The freeze is a convention of this game, not a FIDE rule. In an over-the-board game there is no
handover: this replaces the time the opponent would need to see the move.

- **Nothing is charged to anyone.** The running time, the delay window and the flag checks are
  skipped while frozen (`clockFrozen_`). The clock's stored times are not touched, so pausing and
  unfreezing never lose milliseconds (the fractional-millisecond accumulator is not fed either).
- **Increment** (Fischer) is added at the press, before the freeze, exactly as now.
- **Delay** (Bronstein / US delay): the delay window of the next player starts when the camera
  lands, not when the lever was pressed. The flight never eats into the delay.
- **Thinking during the flight.** The next player sees the position during 1.6 s that do not
  count: about a minute per player over a 40-move game. That is acceptable for a casual mode. Two
  options for a stricter mode: start the next clock at mid-flight, or look at the table edge
  rather than the board during the arc.
- **Pause menu** during the flight: both the freeze and the pause stop the clock. The flight goes
  on behind the menu, and the clock runs again once the flight has landed and the menu is closed.

## Writing the moves on the scoresheets

Both players record every move (FIDE 8.1). The scoresheet agent adds the writing animation with
the hand opposite the clock (`Seat::writingHand()`). Hooks in `GameScene`: right after
`game_.play()` in `onClockPressed()` (a move was completed) and in `endGame()` (result, signature).

- The mover writes their own move after pressing the clock (FIDE allows replying first, but the
  robot writes at once). This animation plays during the camera flight and afterwards, on the
  opponent's time as over the board: the mover's clock is stopped anyway.
- The player to move records the opponent's move at the start of their turn, on their own time,
  as over the board. For a human in first person, the robot does it by itself: a short writing
  task queued when the flight lands, which the player can interrupt by clicking a piece. The
  writing then happens after the move instead (allowed: FIDE 8.1.2).
- In the last 5 minutes without increment, players need not record (FIDE 8.4). Skip the writing
  tasks then, which keeps the hands free in time trouble.

## Players: names, handwriting, profiles and Elo

- **Names.** Each seat has `Seat::name`. For a human it currently comes from
  `localPlayerName()` in `game_scene.cpp`, which the integrator replaces with
  `Settings::playerName`. Hot-seat needs a second name: the setup page asks for both players, and
  each can pick a saved profile.
- **Handwriting fonts.** Each name is written in a font that covers its script, so the two
  players may use two different fonts on the same scoresheet (for example a Japanese name and
  an Arabic name). The scoresheet takes the font per player, not per sheet.
- **Profiles.** Local profiles in the settings file, one section each (`[profile.1]`: name,
  language / handwriting font, preferred hand, and the Elo record `elo, games, wins, draws,
  losses, peak`). The single-player `[player]` record becomes profile 1.
- **Elo.** `elo::applyResult()` already handles one player. A rated hot-seat game updates both
  profiles, each against the other's rating before the game, with each one's own K factor. The
  two changes cancel out (up to rounding) only when both K factors are equal. Games against Stockfish keep
  using the preset ratings (`ai::presetElo()`). Offer "rated / friendly" on the setup page.

## The left-handed player in first person

The clock stands on one side of the table for the whole game (FIDE 6.5: the arbiter decides; here
the setup page does). The seat whose clock is on its left plays and presses the clock with its
left hand (`Seat::playHand`, passed to `Animator::init()`), and writes with its right hand. In
hot-seat one of the two players is therefore always "left-handed" in first person:

- The animator plays with the left hand (the animation agent adds it), and the resting play hand
  goes on the left of the board (`initAnimators()` already asks the animator which hand it uses).
- The first-person head override, the lean and the mouse look are per seat (today there is one
  set, for the only human).
- A preference could put the clock on the side that suits the hand of the player with White (or
  of profile 1).

## Input

One mouse and keyboard, used by the player to move. Everything is ignored during the flight.
Each seat keeps its own look offsets (yaw, pitch, lean), restored when the camera lands in its
eyes.

- **Esc**: pause menu. It offers "Offer a draw" to the player to move, "Claim a draw", and "Resign"
  (for the player to move, with a confirmation naming them), plus Options and Main menu.
- **Draw offers.** A draw offered with a move (FIDE 9.1.2: move, offer, press) is shown to the
  opponent when the camera lands: an accept / decline card. It is declined when the opponent
  touches a piece.
- The promotion picker, the touch-move rules and the arbiter's penalties apply to whichever seat
  is moving.

## What the seat refactor already provides

- `Seat` (`game_scene.h`): colour, `Controller` (Human / Stockfish), name, Elo, provisional flag,
  preset and engine settings, playing hand and `writingHand()`. `GameScene::seat(i)` exposes it.
- `isHumanSeat(seat)` decides who acts in `beginTurn()`; `configureSeats()` builds the seats for
  each game from the mode (`GameMode::Play` / `Watch`).
- Per-seat engine settings, and one Stockfish serving both sides with a hash clear whenever the
  settings change (`ai::Engine`, about 10 ms with 64 MB).
- `setClockFrozen()` / `clockFrozen()`, honoured by `updatePlaying()`.
- `CameraPose`, `CameraFlight` (flight, retarget, `handoverShape()`), `ObserverCamera` (flights,
  free movement kept inside the hall). These are engine-free and unit-tested
  (`tests/game_mode_tests.cpp`).
- Eye poses from the animator (`eyePose()`), the head hidden when the camera is inside it
  (`headNearCamera()`), and gaze for every seat not driven by a first-person player
  (`updateGaze()`).
- The viewer's "through the eyes" viewpoint with `--handover-preview`: the complete handover
  between two AI seats.

## Remaining work

1. `GameMode::HotSeat` with two `Controller::Human` seats, and a setup page: two names or
   profiles, the time control, the clock side, rated or friendly.
2. Replace the single human (`humanColor_`, `humanSeat()`) in the input path with "the seat to
   move": `updateHumanInput()`, `humanTouch/Place/PressClock()`, the markers, the promotion picker
   colour, `placeFirstPersonCamera(seat)`, and the head override.
3. Per-seat look state (yaw, pitch, lean, gaze smoothing) and per-seat first-person camera.
4. Handover: start the flight in `onClockPressed()`, freeze, and unfreeze when it lands. The
   viewer code (`selectViewpoint(0)`, `updateObserver()`) shows the pattern.
5. Draw offer / accept card between humans, and resign with the player's name.
6. Profiles in the settings file, two-player Elo update, and the game over card with both
   changes.
7. Scoresheets: the per-player font and the writing tasks timed as described above.
8. The left-hand play animations (animation agent) and a check of every hand task with the clock
   on the left.

## Risks

- **Motion sickness.** A 180-degree turn with a vertical arc in 1.6 s is a lot for some players.
  Keep the ease-in/ease-out, the level horizon and the look at the board, and offer the instant
  cut.
- **Fairness of the freeze.** Free thinking time during the flights (see above). This is minor in
  casual games, and a stricter mode exists if needed.
- **Hidden-head artefacts.** The camera passes close to the robots' heads when it leaves and
  lands. The head is hidden only within about 16 cm of its centre, so near-plane clipping of the
  neck or shoulders may show for a frame or two. It can be tuned with the near plane and the
  departure direction.
- **Input left over from the previous player.** A mouse button still held, or the look drag,
  when the flight starts: cancel drags and captures at the press, and ignore the buttons until
  they are released after landing.
- **Handwriting and clocks.** If the writing tasks are not interruptible, a player in time
  trouble loses seconds to the animation: they must yield to a piece click (point 7 above).
