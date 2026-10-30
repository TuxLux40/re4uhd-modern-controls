# RE4 (2005, UHD Steam) — Modern Controls log

Read this, `docs/mechanics/` and `dead_ends.md` before changing gameplay code.

## Setup
- Game: Steam appid 254700, `Bin32/bio4.exe`. Original SHA-256 `19aed4af…ee4a` (copy in `~/Projects/re4og-ghidra/`).
  The installed exe (`dd227815…d9e0`) differs only in two PE header bytes: the LARGE_ADDRESS_AWARE flag and the
  checksum (4 GB patch). The code is identical, so all addresses apply.
- Install path: `/mnt/ssd1tb/SteamLibrary/steamapps/common/Resident Evil 4/Bin32/`
- Build: `PATH=$HOME/.local/bin:$PATH ZIG="uv tool run --from ziglang python -m ziglang" bash build.sh`
  (without uv on PATH the build fails silently and an old DLL gets installed: check the timestamp).
- Install: stop the game (`pkill -x bio4.exe`, wait), `cp build/xinput1_3.dll <Bin32>/`, then `cmp`.
- Log: `Bin32/RE4ModernControls.log` (truncated on start, build line first).
- Decompile: `~/Projects/re4og-ghidra/out/` (`bio4_decomp.c`, `bio4.asm`, `functions.tsv`); REA evidence bundle
  `rea-evidence-2026-10-29.json`. Never commit game code or decompiler output.
- Reference mods (reference only, port code in with credit): `~/Projects/third-party/re4_tweaks` (check it FIRST),
  RE3Controls (RE5 design), boshjerns RE4 SDK (legs).

## What the player asked for (oliver, in priority order)
1. No tank controls: camera-relative movement, run by default.
2. Reload and shoot while moving.
3. A crosshair that always stays in the centre of the screen, like every modern game; bullets land in it, the laser
   is only looks. A reticle that follows the laser was rejected (that was the state of 2026-10-17).
4. Free over-the-shoulder camera like RE5 (nice to have; may be dropped if it blocks 1-3).
5. QoL: quick pickup, pickup log, d-pad weapons, rearm, Ashley safe from Leon's shots, fade near the camera.
Recurring complaints: any view motion he didn't cause, smoothing on the reticle, the gun visibly not pointing at the
reticle, re-deriving what re4_tweaks already has, asking him to repeat things instead of reading the log.

## Status per feature (2026-10-29)
| Feature | State | Evidence |
|---|---|---|
| Free camera, camera-relative move, run default | works | oliver 10-17, many sessions since |
| Walk while aiming, wall collision, Ashley ground | works | oliver 10-17 |
| Reload on the move (no aim zoom), hip fire | works | oliver 10-18 |
| RE5 button layout, hidden prompts, auto QTE | works | oliver 10-18 |
| Centred reticle, bullets to the centre, spread ticks | works | oliver 10-18 ("could fight well now") |
| Red target dot | works | oliver 10-18 |
| Fade near camera | works; Ashley only when blocking | oliver 10-29 |
| Quick pickup, pickup log with item pictures | works | oliver 10-28/29 |
| Rearm after last grenade, d-pad weapon select | works | log 10-28, oliver |
| Gun pose: no tilt at aim start, no shake, no snap (handgun family) | works (milestone) | oliver 10-30 |
| Laser at the extremes (far up/down) | open: lands above the reticle | oliver 10-30 |
| Legs while moving and reloading/aiming | **open** — twitch, right leg clips | oliver 10-26, 10-27 |
| Camera jumps on stairs, wall pull-in on edges | open | oliver 10-27 |
| Ada / Mercenaries characters (legs, slacks) | open | issue raised 10-27 |

## Confirmed facts
See `docs/RE-NOTES.md` (globals, camera, trace, laser) and `docs/mechanics/aim.md` (aim, arm pose, frame order).

## Sessions
### 2026-10-17 — M0–M3
- Proxy, free camera, camera-relative movement, walk while aiming, installer, crosshair at the laser end.
- Crosshair following the laser was rejected; centred reticle and laser/bullets steered to it started.
### 2026-10-18 — centred aim, RE5 camera, buttons
- Camera locked to the gun line shook with recoil and sway → view follows aim values instead.
- Sway removed, spread ticks; RE5 shoulder camera after "aim at where Leon looks" swung 180°.
- Buttons, prompts, QTE, reload without aiming. Lesson: check re4_tweaks first.
- Red 9 snapping down at aim start; oliver concluded over-the-shoulder and "aim exactly where I look" conflict.
### 2026-10-25/26 — play-test monitoring (docs/PLAYTEST-2026-10-25.md), legs, sprint removed as a cheat
### 2026-10-27/28 — quick pickup, pickup log, rearm, auto-centre, camera fixes; quiet log watcher
### 2026-10-29 — item pictures in the DLL; gun tilt and laser
- Gun tilt at aim start traced to the raise layer; laser bent onto the reticle; jitter; shotgun off-screen.
- Circuit breaker: redesign with the game's own levers (heading + arm value + feedback from the laser line).
- Whole-project review with the native-game-modding skill and REA: results in `docs/mechanics/aim.md`,
  `dead_ends.md` and the "Next" list below.

### 2026-10-30 — aim milestone
- Redesign test: sway shake back (sideways anchor dropped by mistake), floor aim swinging, sideways jump at aim start
  (feedback loop), first raise snapping down. Fixed: sway anchor restored, feedback loop removed, handgun raise from
  the weapon table (the game's modern-mouse-aim motions). oliver: "looks good", saved as a milestone.

## Next
1. Laser above the reticle when aiming far up/down (aim.md "Open questions").
2. Static raise motions for the other weapon families; then drop the learnt table and the unused convergence code
   (`g_convYaw`/`g_convV`, AimConvYaw, gun-point easing).
3. Trim the diag logging (`diag pose`, `diag gun`).
4. Legs: write a mechanism note before any more fixes (dead_ends.md "Legs").
