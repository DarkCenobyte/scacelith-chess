# Offline multiplayer (hot-seat) plan

Two people play on one PC, in turn, each from their own robot's eyes. After a move is completed
(the clock pressed), the camera leaves the mover's eyes and flies over the table into the
opponent's eyes. During the flight the clock is frozen. Nothing here is networked.

**Status: implemented.** New Game > Opponent > "Human, same PC". This document keeps the design,
says where the implementation differs from it (the paragraphs marked *Implemented* and
*Deviation*), and lists what is left. The code:

- `src/game/hotseat.h/.cpp` (engine-free, in the core library, unit-tested by
  `tests/hotseat_tests.cpp`): who has the controls, which hand plays, the rematch swap, the clock
  stepping with the freeze, the handover (`hotseat::Handover`: flight or cut) and the latch on
  buttons still held (`hotseat::InputGate`).
- `src/game/game_scene_hotseat.cpp`: the scene side (`GameMode::HotSeat`), the handover, the
  per-seat look, the writing, draw offers, the ratings and the game over card.
- `src/ui/ui_hotseat.cpp` (players panel, caption, draw card) and the New Game page
  (`hotSeatColumn()` in `src/ui/ui_screens.cpp`).
- `elo::applyPair()`, `Settings::localPlayers` (`[local_player_N]`), `Scorekeeper::setHold()`.

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

*Implemented* as above, with these details: `GameScene::startHandover()` runs at the end of
`onClockPressed()` (after `beginTurn()`, so the seat to move is already O), and
`hotseat::Handover` drives the flight (`updateHandover()` in `simulate()`, after the robots are
posed, so the end pose follows O's head). `updatePlaying()` returns early while the clock is
frozen, and `hotseat::advanceClock()` never counts a frozen clock either (no time, no delay
window, no flag). The input is gated during the handover and until the buttons held at the
press are released (`inputBlocked_`), and the scripted moves of `--play` wait. The seat with the
controls is `inputSeat()` (the seat to move), the seat whose eyes are the view is `viewSeat()` (-1
during a flight), and the first-person look is `firstPersonSeat()`'s. The mover's head goes back to its
robot's gaze at the press, while O's head turns to O's own look during the flight.

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

*Implemented:* Options > Gameplay > "Hot-seat handover" offers an instant cut or a flight of 0.8
to 2 s (`[gameplay] handover_seconds`, 1.6 by default, 0 = cut; `--handover <s>` for one run).
The cut fades to black in 0.18 s, changes seats at black and fades in over 0.32 s; the clock is
frozen for the whole 0.5 s.

*Deviation (the Arc):* with symmetric control points only, the view at mid-flight (a quarter turn,
looking across the table and 28 degrees down) aimed at the table about 0.33 m beside the board.
`handoverShape()` now adds the same lateral offset to both inner control points, chosen so that
the mid-flight view ray hits the board centre: the arc orbits the board on the side the camera
looks away from, and the board stays in the middle of the view for the whole flight (within about
6 cm of the centre, unit test `hotseat_flight_keeps_the_board_in_view`). The viewer's handover
(key 0, `--handover-preview`) uses the same shape.

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

*Implemented* as described (`hotseat::advanceClock()`, unit tests
`hotseat_clock_frozen_during_the_flight` and `hotseat_delay_window_starts_on_landing`). The
stricter mode is not.

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

*Implemented* with `Scorekeeper::setHold(seat)`: a held sheet keeps the moves in order and writes
them all once released. At the press the mover's sheet is released (it records the opponent's
previous move if it had not yet, then its own) and the next player's is held. When the view lands
the next player has 0.5 s (`kWriteGrace`): if they touch a piece first, their hold stays until
their own clock press (the move is written after it, FIDE 8.1.2); otherwise the robot writes it at
once. The writing hand is the one opposite the clock, the playing hand is free, so the writing
never delays a piece click. `finishGame()` releases both holds before the result is written. Each
sheet is in its player's handwriting (chosen on the New Game page).

*Deviation:* FIDE 8.4 is not applied. Writing never costs clock time here (the other hand plays),
so skipping it in time trouble brings nothing; the sheets stay complete.

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

*Deviation (simpler profiles):* there is no profile picker. The New Game page asks for two names
and two handwritings (White defaults to Options > Player's name and hand, Black to "Player 2" and
the next hand); the last choices are remembered (`[hotseat]`). A game is **friendly by default**.
A rated game keys the ratings by name: `Settings::localPlayers`, one `[local_player_N]` section
each (`name, elo, games, wins, draws, losses, peak` and the FIDE unrated phase `rated,
unrated_games, unrated_opponents, unrated_half_points`, `elo::readRecord`), created unrated at 1500
the first time a name plays a rated game. `elo::applyPair()` rates each against the other's rating
before the game (unit test `hotseat_elo_pair`). The single-player `[player]` record (the rating
against Stockfish) is never read nor changed by a hot-seat game, and does not become a profile. A
game with fewer than two plies, or the same name on both sides, is not rated; leaving a game early
(Main menu, closing the window) rates nothing. The game over card shows both changes (or "friendly game").

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

*Implemented:* the New Game page puts the clock at White's or Black's right (`[hotseat]
clock_right_of`, `--clock-right white|black`) and names the player who will play left-handed.
`hotseat::playsLeftHanded()` gives each seat's playing hand; the look state (`GameScene::Look`:
yaw, pitch, gaze smoothing, lean) is per seat, restored when the view reaches that seat. The rematch
swaps the colours and the clock side, so each player keeps the same hand. Checked with scripted
games (`--play`) with the clock on either side: touch, carry, captures, castling and the clock
press with either hand, and the writing with the other.

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

*Implemented.* "Offer a draw" in the Esc menu is noted ("it will be offered with your move") and
delivered at the offering player's clock press; the card (mouse buttons only, Space presses the
clock) appears for the opponent once the view has reached them, and touching a piece declines it.
Resign confirms with both names ("Resign for Alice? Bob will be declared the winner."). Claims are
those of the game against Stockfish, for the player to move. The drag is cancelled at the press,
and `hotseat::InputGate` ignores every button (and Space) held since then until all are released.
Markers and the legal-move hints are hidden during the handover. A players panel (top left) shows
both names (and ratings when rated) and a caption names the player whose turn begins.

The play comfort features follow the player to move as well: the in-game pointer (Options >
Gameplay > Game pointer), the square aimed at while a piece is in hand, drag and drop, the clock
hover and the see-through playing arm (the arm of the player who carries the piece). During the
handover there is no pointer, no aiming and no hover (the system arrow stays hidden), and the
pointer stays dimmed until the buttons held by the previous player are released. The look at
one's own scoresheet (S) is one state of the view, not per seat: it reads the sheet of the player
at the table and ends at their clock press (the flight starts from the sheet's narrower view and
widens; a cut lets it end while the view darkens).

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

## Work items (all done)

1. Done: `GameMode::HotSeat` with two `Controller::Human` seats (`configureHotSeatSeats()`), and
   the setup page: two names and handwritings, the time control, the clock side, rated or friendly.
2. Done: the input path uses "the seat to move" (`inputSeat()`, `inputColor()`):
   `updateHumanInput()`, `humanTouch/Place/PressClock()`, the markers, the promotion picker
   colour, the first-person camera (`firstPersonView(seat)`), and the head override.
3. Done: per-seat look state (`look_[2]`) and first-person camera.
4. Done: the handover (flight or cut) started in `onClockPressed()`, with the freeze.
5. Done: draw offer / accept card between humans, and resign with the player's name.
6. Done, simplified: name-keyed local ratings instead of profiles (see above), `elo::applyPair()`,
   and the game over card with the winner's name and both changes. The rematch swaps colours.
7. Done: each player's handwriting, and the writing timed as described (except FIDE 8.4).
8. Done: the left hand plays when the clock is on the player's left (`hotseat::playsLeftHanded()`).

Command line, for tests and screenshots: `--start --hotseat [--white-name N] [--black-name N]
[--clock-right white|black] [--rated] [--handover <s>]`, and `--play e2e4,e7e5,...` (moves played
by hand for the side to move, one after the other, the way a player would). With `--no-intro` and
`--warp <s>`, the hand-over starts about 1.7 s after the start with `--play e2e4` and lands 1.6 s
later. The UI viewer has `--ui-screen newgame-hotseat|hotseat-hud|hotseat-confirm|hotseat-gameover`.

## Known gaps

- No profile picker: a typo in a name starts a new rating.
- FIDE 8.4 (no recording in the last 5 minutes) is not applied (see above).
- The stricter clock mode (next clock started at mid-flight) is not offered.
- The hold logic of the scoresheets needs the GL scene, so it is checked with scripted headless
  games, not unit tests.

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
  they are released after landing. (Done: `hotseat::InputGate`.)
- **Handwriting and clocks.** If the writing tasks are not interruptible, a player in time
  trouble loses seconds to the animation: they must yield to a piece click (point 7 above).
