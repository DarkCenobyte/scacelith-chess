# Handshake clipping in the anim viewer: what the current visuals show (read-only research)

The close-ups have been blurred because of depth of field: the anim viewer never sets a focus distance. With depth of field turned off, the clasp shows three main faults:
- **Fingers go through the partner's wrist.** Each hand's fingers wrap around the partner's wrist cuff and forearm, not the partner's palm, and most of them pass through it.
- **Thumbs cross in an X.** Both thumbs stand up in the air above the partner's wrist and their bases pass through each other.
- **Hands are stacked in line.** The two hands are almost exactly anti-parallel and centred on each other, so each hand's fingers reach past the partner's wrist.

No tracked file was edited. The images are in `/tmp/claude-0/-home-user-scacelith-chess/ea3bb550-e871-579e-a29d-78cac27b4b97/scratchpad/shake/understand/` (written as `<dir>/` below).

World frame used throughout: White sits at +Z facing −Z, so its right is +X. Black sits at −Z, so its right is −X. The planned clasp centre is (0, BOARD_TOP_Y+0.225 = 1.007, 0), set at `animator.cpp:453-459`.

## 1. Camera code, depth of field, and how to get sharp close-ups

**Where the cameras are defined:**
- View names: `src/anim/anim_viewer.cpp:512-514`. `kOrbitView = 11` is hard-coded at :516, so new views must be appended at the end.
- `camera()` is at `anim_viewer.cpp:1462-1575`.
  - `shake` (case 10, :1500): `look({0.42,1.08,0.02},{0,1.0,0},34°)`. It is fixed, does not follow the hands, and sits 0.43 m from the target on the +X side (White's back of hand).
  - `top` (:1476): `({0,2.3,0.02}→{0,0.8,0},40°)`. Far away; the hands cover about 100×130 px at 720p.
  - `side` (:1472): from +X at 1.5 m.
  - `front` (:1474): from behind Black, 1.75 m.
  - `white` / `black` (:1477-1484): eye cameras at 55°. The clasp falls at the bottom edge and is partly cut off.
  - `hand` / `handb` / `handl` (:1486-1498): aimed at the midpoint of the playing hand's index tip and thumb tip, plus 5 cm up. During the handshake the thumb tip is 63 mm above the clasp, so the hands sit low in the frame and the little-finger edge is cut off.
  - `orbit` takes mouse input only, so it can't be aimed in headless shots.

**Why close-ups are blurred:**
- `render()` (:574-575) never sets `r.post().settings.dofFocusDistance`, so it stays at the default 0.8 m with f/2.8 (`src/render/post/postfx.h:45-46`).
- Depth of field is on by default: quality High (`src/game/settings.h:29-31`, `renderer.cpp:54-55`).
- In `shake` the hands are at 0.43 m while the board at about 0.8 m is sharp. A rough thin-lens estimate is a blur radius of about 9 px at 720p. Compare `<dir>/shake_1.42_dof.png` (blurred) with `<dir>/shake_1.42.png` (sharp).

**Sharp shots without a code change:**
- Add `--ini <dir>/sharp.ini`. The file contains `[graphics] depth_of_field = false`, `motion_blur = false`.
- Screenshot mode sets `settings.readOnly`, so the file is never written to.

**Proper fix:**
- Before `r.beginFrame` (:575), set `r.post().settings.dofFocusDistance = length(cam.position - target)`. This is what `robot_viewer.cpp:269` and `coach_viewer.cpp:216` already do.
- Setting `post().settings.dof = false` would not stick: `renderer.cpp:593` overwrites it from the render settings in `endFrame`.

**Private debug binary (no tracked change):**
- Patched copy at `/tmp/claude-0/-home-user-scacelith-chess/ea3bb550-e871-579e-a29d-78cac27b4b97/scratchpad/shake/priv/scacelith`, diff in `priv/anim_viewer_debugcam.diff`.
- `--cam dx,dy,dz,fov` places the camera relative to the midpoint of both right hands (HandR and MiddleR1 of each), so it follows the pumps.
- `--only 0|1` draws one robot only.
- `dumpShake()` logs the joints, plane distances and segment-to-segment distances (`SHAKEDUMP` lines in `<dir>/log_*.txt`).
- To use it: `SCACELITH_BUILD=<priv> tools/shot.sh ...`.

## 2. Measured geometry at the clasp (t = 1.42)

**Symmetry:** the two hands are exactly point-symmetric about the vertical axis through the clasp point (0, 1.0096, 0): White (x, y, z) ↔ Black (−x, y, −z). Every fault below therefore appears identically on both sides.

**White's hand frame:**

| Item | Value |
|---|---|
| Wrist | (0.0296, 1.0082, 0.0454) |
| Palm normal | (−0.956, 0, 0.293) |
| Finger direction | (−0.292, −0.080, −0.953) |
| Thumb side | (−0.023, 0.997, −0.076) |

So the palms are vertical, the palm plane runs along the line between the two shoulders, and the hands point down only 4.6°, not the planned 0.28 rad (16°).

**Hands almost anti-parallel:**
- Seen face-on to the palms, the two hand axes cross at only about 9.2°. A real handshake has the fingers clearly pointing down, so the hands cross at a much larger angle.
- Both palm centres are on the clasp point, so the hands are centred on each other along their length.

**Palm contact:**
- The two hand mid-planes are parallel and 30.0 mm apart.
- With palm surfaces at ±13.5 mm (`kPalmHalf`), that leaves a 3 mm gap at the palm centre (about 0.8 mm at the little-finger-side pad).
- The palms themselves do not pass through each other.

**Depth of the hands along the arm:**
- White's knuckle row is about 86 mm from its wrist, which places it 14 mm past Black's wrist, on the base of Black's palm.
- White's fingers therefore extend beyond Black's wrist and curl around Black's wrist cuff.

**Fingertip positions (White's, in Black's hand frame):**
- Index tip: 14.5 mm toward Black's elbow from Black's wrist, 28.6 mm behind Black's mid-plane, 20 mm up.
- Middle tip: 14 mm toward the elbow, 36.7 mm behind the mid-plane.
- Both are around or inside the forearm cuff, not on the back of the hand.

**Fingers inside the partner's forearm:** the cuff is a 25 mm-radius sphere around the wrist (`kWristDome` + 4.5 mm), the forearm radius is 26–33 mm, and finger half-thickness is about 7 mm. Distance from the partner's forearm axis:

| Finger segment | Distance from axis | Approx. depth inside |
|---|---|---|
| Middle, middle segment | 8.2 mm | 24 mm |
| Ring, middle segment | 12.1 mm | 20 mm |
| Middle, first segment | 14.0 mm | 18 mm |
| Ring, tip segment | 16.2 mm | 15 mm |
| Middle, tip segment | 17.2 mm | 15 mm |
| Ring, first segment | 19.0 mm | 13 mm |
| Index, middle segment | 26.6 mm | about 5 mm |
| Index, tip segment | 28.6 mm | about 3 mm |

The little finger passes under the partner's wrist (29–33 mm from the partner's little-finger metacarpal).

**Thumbs:**
- White's thumb rises 25° above horizontal and is 31° from its own finger axis.
- Its tip is at (−0.025, 1.073, −0.051): 64 mm above the axis of Black's wrist, 9 mm toward Black's elbow, and between Black's palm and back (6 mm from Black's mid-plane on the palm side). It sticks up in the air above the wrist and does not lie on the back of the hand.
- The two thumb metacarpals are 4.6 mm apart axis-to-axis, which is about 15 mm of overlap. That overlap is the visible X.
- Thumb segment 2 is 7.1 mm from the partner's thumb metacarpal.

**Over time:**

| Time | State |
|---|---|
| 1.20 | Hands 34–45 mm apart, no contact. |
| 1.30 | Thumbs already overlapping (12.7 mm apart); straight fingers slide past the partner's wrist. |
| 1.42 → 2.46 | The pumps (32 mm) are identical for both hands, so the clipping stays the same throughout. |
| 2.55 | Release: the middle-finger middle segment is still 11.5 mm from the partner's forearm axis, so the opening fingers drag out through the cuff. |
| 2.75 | Hands separated. |

## 3. Images (all `--robot` with sharp.ini unless noted, 1280×720)

**Existing views:**
- `shake_1.0.png`: approach, hands open. Fingers straight and overlapping in projection, not yet touching. Both thumbs point straight up at about 90° like a high-five.
- `shake_1.42.png`, `shake_1.8.png` (bottom of the pump), `shake_2.2.png`: identical grip at every time. The centre is White's back of hand.
  - Two thumbs make a tall X above the hands.
  - Left of centre: Black's fingertips appear on White's wrist cuff. The index runs across the top of the wrist, the middle and ring fingertips stick out of the forearm surface like buttons, and the little finger is under the wrist.
  - Right: White's knuckles and fingers enter Black's forearm.
- `shake_1.42_dof.png`: the same shot with depth of field on, blurred.
- `top_1.42.png` and `top_1.42_crop.png` (crop ×4): forearms and hands lie in one straight diagonal. The hands are stacked side by side along their whole length and each set of fingers hooks around the partner's wrist. Too far away to judge detail.
- `side_1.42.png`: whole-body view. Arm posture is plausible (elbows low); the thumb X is visible even from 1.5 m.
- `front_1.42.png` and `front_1.42_crop.png`: too far away.
- `white_1.42.png` and `white_1.42_crop.png`: this is the first-person view the player sees. The clasp is cut off at the bottom of the frame. Black's thumb sticks up toward the camera, and Black's index wraps White's wrist while its other fingers disappear into it.
- `black_1.42.png`: the mirror image of the above.
- `hand_1.42.png`, `handb_1.42.png`, `handl_1.42.png` (also in `montage_hand_handb_handl_black.png`): the thumb X dominates and the little-finger edge is cropped. Not good for checking a fix.

**Custom close-ups (private binary, camera relative to the clasp, field of view 30°):**
- `c_px_1.42.png` (+0.30 m in X, White's back): the best overall view.
  - `c_px_1.42_crop_Bfingers_in_Wforearm.png`: Black's middle and ring fingernails sunk into White's forearm about 15–25 mm from the wrist seam; the index over the top of the wrist; the little finger under it.
  - `c_px_1.42_crop_Wfingers_into_Bforearm.png`: the first segments of White's four fingers cut cleanly into Black's forearm surface, with no contact shading.
- `c_nx_1.42.png` (−0.30 m in X): a pixel-for-pixel mirror of c_px.
- `c_up_1.42.png` (0.30 m above): both thumbs lie roughly along the forearm line, crossing in projection at the centre. Each set of fingers hooks over the partner's wrist with the dark joint caps beyond it.
- `c_dn_1.42.png` (0.20 m below, field of view 45°; the image is mirrored, White's hand on the left) and `c_dn_1.42_crop_Wpinky_around_Bforearm.png`:
  - The clearest proof. The two little-finger edges run parallel with a dark gap between the palms.
  - Each little finger wraps around the partner's forearm right next to the wrist.
  - The index tip sticks out of the far side of the partner's forearm, and a dark joint cap shows through the forearm surface.
- `c_w_1.42.png` (looking along White's forearm, offset (0.10, 0.08, 0.30)): Black's four knuckles sit on the −X side of White's wrist. Its index goes over the top of the cuff, the other fingers go through it, and the tips come out on the +X side. Black's thumb stands up in front of the camera.
- `c_b_1.42.png`: the mirror image.
- `c_q_1.42.png` (three-quarter view from above, (0.22, 0.18, 0.12)): a good summary picture. White's knuckles hook into Black's cuff, Black's fingers hook over and under White's wrist with the tips sticking out near the camera, and the thumbs are crossed.
- `c_nx_onlyW_1.42.png` (White alone, from the palm side): the grip shape is an open C-hook. The thumb is straight and points up and toward the partner at about 31° from the fingers, staying in the palm plane instead of wrapping over the partner's hand.
- `c_px_onlyB_1.42.png`: the same for Black.

**Timeline:**
- `montage_c_px_1.20_1.30.png`: open hands, then straight fingers sliding past the partner's wrist while the thumbs start to cross.
- `montage_c_dn_1.30_c_px_2.55_2.75.png`: slide-in seen from below (grey speckled shading where the two thumb bases pass through each other), release (fingers still in the cuff, X still there), then separated.

**Capsule robots:**
- `cap_shake_1.42.png`: the same faults.
- `cap_c_dn_1.42.png`: the clearest view of the bones. The little-finger capsule chains loop through the partner's forearm cylinder and the hand boxes are parallel.
- `cap_c_up_1.42.png`.

## 4. Proposed verification views (add to `anim_viewer.cpp`)

**Where to add them:**
- Append the names to `kViews` after `"coachhandt"` (:512-514) so `kOrbitView = 11` stays valid, and document them in the header comment (:30-37).
- Add a block before `default:` (:1572) in `camera()`.
- Aim every view at P, the midpoint of `(g[HandR] + g[MiddleR1]) / 2` over both animators. P follows the pumps and is about (0, 1.010, 0) at the clasp. The `claspPoint()` code in the private diff can be copied as is.
- Use field of view 30°, `nearZ` 0.01.
- Set the focus distance to |camera − P| (see section 1).

| Proposed view | Camera position | What a correct handshake shows |
|---|---|---|
| `shakex` | P + (0.30, 0.02, 0) | White's back of hand. Black's 4 fingertips on White's back near the little-finger side (bottom), not on the forearm. Black's thumb lying over White's index knuckle (top). No X. |
| `shakexl` | P + (−0.30, 0.02, 0) | Mirror for Black. |
| `shakeu` | P + (0, 0.30, 0.001), up hint (0, 0, −1) | Thumb side. Thumb-index webs locked; each thumb flat on the partner's back of hand. Forearms meet at an angle, not in one straight line. |
| `shaked` | P + (0, −0.20, 0), up hint (0, 0, −1), field of view 45° | Little-finger side. Each set of fingers wraps the partner's little-finger edge, with the tips on the partner's back of hand short of the wrist cuff. |
| `shakew` | P + (0.10, 0.08, 0.30) | Down White's forearm: nothing of Black passes through White's forearm or cuff. |
| `shakeb` | P + (−0.10, 0.08, −0.30) | Mirror for Black. |
| `shakeq` | P + (0.22, 0.18, 0.12) | Overall three-quarter check. |

For `shaked`, clamp the camera height to `max(P.y − 0.20, BOARD_TOP_Y + 0.03)`. Without the clamp, the bottom of the pump (P.y ≈ 0.978) puts the camera at 0.778, below the board top at 0.782.

**Also worth adding:**
- A `--only 0|1` switch (in `render()` :600-606), as in the private diff, to inspect one hand's grip shape alone.
- A numeric check in `selfTest` (the private `dumpShake()` already has the closest-segment logic):
  - minimum distance from each finger segment to the partner's forearm axis should be at least 32 mm (25 mm cuff + 7 mm finger);
  - thumb metacarpal to thumb metacarpal should be at least 20 mm;
  - each fingertip should be behind the partner's mid-plane by at least 13.5 mm and between 0.02 and 0.09 m along the partner's hand from its wrist.

**Suggested shot list for checking a fix** (one at a time, with `--robot --ini sharp.ini`): `shakex`, `shaked`, `shakeu`, `shakew` at t = 1.30, 1.42, 1.80 and 2.55.