# Handshake measuring and fitting tools

Research tools written while fixing the robots' handshake (see `docs/handshake/README.md`); they
are not part of the CMake build. Each `.cpp` is compiled by `mk.sh` together with the animation
sources (it `#include`s the `src/anim` .cpp files, as `tests/anim_tests.cpp` does) and the robot's
mesh code, against a configured build's `libscacelith_core.a`.

`env.sh` (sourced by every script) sets `HS_SRC` (the source tree measured: this repository by
default, or any worktree with a changed handshake), `HS_BUILD` (its build directory, default
`$HS_SRC/build`) and `HS_WORK` (binaries, the SDF grid cache and outputs, default
`$HS_BUILD/handshake`). The first run of a tool that uses `geo.h` builds the exact SDF grids of the
arm (about 40 MB, `$HS_WORK/grids.bin`); later runs load them.

| File | What it does |
|---|---|
| `mk.sh <tool> [out] [flags]` | builds `<tool>.cpp` into `$HS_WORK/bin/<out>` |
| `geo.h`, `handsdf.h` | the robot arm's exact SDFs (palm, thenar, phalanges, forearm, upper arm, elbow and shoulder balls) copied from the render code, their meshes, and SDF grids for speed |
| `pen.cpp` | runs the real two-robot handshake and measures every 1/120 s how deep each arm's mesh vertices go into the other arm (both directions), per phase, plus the clasp metrics (pads, thumb, palms, crossing), strain, IK clamps, bone speeds, hand speeds and the lowest point. `pen rr\|rl\|lr\|rlpen\|cutrr\|cutrl\|cutlr [exact] [verbose]`; `exact` uses the SDFs instead of the grids; `PHASES=t1,t2,tc,tp,to,tw` names the phase boundaries, `CUTAT=<s>` the cut |
| `measure.sh <tag>` | builds `pen` against `$HS_SRC` and runs rr, rl, lr, rlpen and the cut cases into `$HS_WORK/<tag>/` |
| `cutscan.sh` | the cut cases at several instants (`MODES`, `CUTS`, `PEN`, `SAVE`) |
| `opt9.cpp`, `cma.inc` | the offline fit (CMA-ES, 30 parameters) of the clasp placement and the grip/open poses; its cost terms and their environment weights are listed at the top of `opt9.cpp` (`W_CUFF`: the grip against the partner's forearm cuff where the pumps put it and turned ±0.13 rad, as a left-handed partner's is; `W_SLIDEV`: the slide's length, i.e. the speed into the contact). `opt9 fit <out.txt> [in.txt] [seed] [gens] [sigma]`, `opt9 show <in.txt>`; `BOUNDS="name:lo:hi,..."` narrows a parameter's range (e.g. `lift:0:0.70` for a lower elbow) |
| `fit.sh` | `opt9` with the weights of the last refit, g7 (each overridable from the environment) |
| `bake.py <fit.txt> <fit.log>` | writes a fit into `src/anim/animator_impl.h` (constants, `poseShakeOpen` thumb, `poseShakeGrip` from the log's "grip pose:" line) |
| `fits/` | the parameters of the contact refits g1 – g7 (g7 is in the code; `g7.out`, its `opt9` output, holds the grip pose `bake.py` reads), of l70 (the best fit with the elbow lift at most 0.70 rad, with its output `l70.out`: bake it to try the lower elbow) and of o3 (the open thumb of the arrival) |
| `ballprobe.sh` | how much of the thumb's dark CMC ball shows for a few thumb poses (`P=<fit.txt>`) |
| `relscan.sh` | release timing scan; needs a `pen_tune` built against a tree whose `planHandshake` reads `SH_*` timing overrides (not kept) |
| `capcheck.cpp` | calibrates the capsule model of `anim_handshake_hands_clasp_without_going_through` on the real handshake (smallest margins per phase); `capsules.h` is that model |
| `mirrorcmp.cpp` | a right-handed vs a left-handed robot's shaking hand, bone by bone, per phase (`FROMB` / `TOB`: another range of bone indices, e.g. the arm). Build: `mk.sh mirrorcmp mirrorcmp src/character/robot_hand.cpp` |
| `chest.cpp` | the cancel test's chest movement, frame by frame |
| `geom.cpp`, `palmsec.cpp` | plan geometry (clasp, pre-contact, rest) and palm cross-sections |
| `shots.sh`, `montage.sh`, `sharp.ini` | anim viewer renders of the close-up views (`shakex shakexl shakeu shaked shakew shakeb shakeq`, `side`, `front`) without depth of field, and montages. Each shot plays the 0.5 s before its instant (`--play`), so the temporal filters carry the motion's history as in the game (`FROZEN=1`: frozen 8-frame shots, whose ambient occlusion has not converged) |
| `logsec.py` | the armtrace lines of the anim viewer self-test's handshake sections |
