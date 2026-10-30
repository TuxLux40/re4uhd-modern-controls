# Dead ends

Read before proposing a design. Append whenever the circuit breaker trips. Collected from all sessions
2026-10-17 to 2026-10-29 (transcripts, commits, play-test notes).

## The pattern behind most of these
The expensive failures share one shape: a mod-side model of something the game already does per weapon and per
state (gun line, arm pose, leg animation), then a correction layer for the model's errors, then another. Each
layer fixed one weapon or one case and broke another. The cheap successes did the opposite: they drove a value the
game already uses for that job (KEY_RUN, PlReloadDirect, the camera's CamPoint, the Bullet call).

## Aim — crosshair at the laser's end (2026-10-17)
- **Tried:** the reticle drawn where the laser hits.
- **Failed because:** it jumps with sway and with depth edges, and it isn't centred. oliver wants an always-centred
  reticle; this is the one design he has rejected outright.
- **Better approach:** centred reticle; bullets steered to the centre ray (Bullet 0x686C40 hook).

## Aim — camera locked onto the gun line (2026-10-17/18)
- **Tried:** the aim camera looks along the laser, without smoothing.
- **Failed because:** recoil, breathing and sway move the gun, so the whole view shook.
- **Better approach:** the view follows the aim *values* (heading, side, sway anchor), never the gun.

## Aim — keeping the sway and showing it as a dot inside the ticks (2026-10-18)
- **Tried:** the laser dot wandering inside fixed ticks, with smoothing and edge delays.
- **Failed because:** smoothing on the reticle reads as lag; the dot still left the ticks.
- **Better approach (taken):** sway off, remake-style spread ticks.

## Camera — swinging to where Leon looks at aim start (2026-10-18)
- **Tried:** aim start turns the view toward Leon's facing; also circling the game's own aim eye.
- **Failed because:** at large angles the view swung 180°; any view motion the player didn't cause disorients.
- **Better approach (taken):** RE5 model: keep the view direction, move in over the shoulder, Leon turns instead.

## Aim — steering the gun through the sway value with a learnt model (2026-10-29)
- **Tried:** the gun turned onto the aim point through the arm value and `cPlayer+0x448`, with:
  1. a learnt shoulder-pivot model;
  2. a mapping constant;
  3. a closed-loop trim on the model's leftover error.
  The laser was also bent to the reticle and its start patched back onto the gun.
- **What broke, one fix after another:**
  - the laser detached from the gun;
  - wrong pitch mapping (2/π implies 90° per unit; 60° measured);
  - jitter at depth edges;
  - the trim overshot while turning;
  - the pivot was learnt for one weapon only;
  - the shotgun hung high at the edge of the view.
- **Failed because:** `+0x448` is the gun's free play inside a cone of 0.4°–12° (PlWepLockCtrl, REA). Past the cone
  the game turns the body. 30° through it are poses the game never makes. The pose is also set by 52 per-weapon
  init routines, so a single learnt pivot can't fit them all.
- **Missed step:** no mechanism note first. The decompile was there and would have shown both facts.
- **Better approach:** heading for sideways convergence, arm value for pitch, feedback from the laser line
  (the 10-29 redesign). Fallback agreed with oliver: original aim camera with the centred reticle.

## Aim — closed-loop convergence from the laser line (2026-10-29/30)
- **Tried:** the 10-29 redesign: each frame half the gun's error, measured from the laser line, went into Leon's
  heading and the arm value.
- **What broke:** a visible sideways jump at every aim start once live aim began; aiming at the floor (point ~1000
  away) Leon swung 3-20 deg back and forth every second. Turning Leon moves the laser sight with him, so near points
  make the loop gain far above 1. The same build dropped the sideways sway anchor by mistake (shaking view).
- **Failed because:** a feedback layer on a measurement that its own output moves; second failure of the
  "steer the gun onto the reticle" family.
- **Taken instead (milestone 2026-10-30):** no gun convergence at all. Gun parallel to the view, bullets to the
  centre, sway anchored on both axes, raise motions from the game's own tables.

## Aim start — raise motions per weapon family, one at a time (2026-10-30)
- **Tried:** the learnt raise table, then a static lookup for the handgun family only (table +0x88 -> +0x9C/+0xA0).
- **Failed because:** every weapon kind raises from its own routine with its own table offsets (0x48 -> 0x54/0x58,
  0x7C -> 0x90/0x9C, 0x68 -> two different pairs, ...). The SMG still snapped down; patching family by family
  repeats the per-weapon trap. The skill wasn't reloaded after a context compaction, so the variants check (gate 4)
  was skipped.
- **Next:** one lever for all weapons. Every raise routine already has the PC modern-mouse-aim branch
  (`0x012224B0 == 1`) that picks the live aim's up/down motions. The flag is read in 149 functions, so it can't be
  set globally; candidates: set it only around the raise state's dispatch, or hook ArmPoseCtrl_Init.

## Aim — shotgun points ~26 deg right of the view (2026-10-30, open)
- With the game's own live pose, heading = view and side ~0, the shotgun's laser is 26 deg right and 15 deg above
  the reticle (handgun: 0-4 deg). The pre-10-29 30 deg upper-body twist was hiding this. Mechanism unknown: how the
  game's own aim camera places itself for the shotgun stance. Phase 1 needed before any fix.

## Aim start — seeding pitch from the free camera (2026-10-17 and 2026-10-18)
- **Tried:** start the aim at the camera's pitch (stairs case); later the same for the Red 9.
- **Failed because:** the raise motion ignores the arm value (see aim.md), so the gun first pointed level, then
  snapped. The camera over the shoulder and a gun that points exactly where the camera looks also disagree by the
  shoulder offset (parallax). oliver: "choose between over the shoulder and aiming exactly where I look".
- **Better approach:** raise with the live aim's up/down motions; converge with heading and arm value.

## Legs — procedural walk cycle and motion copies (2026-10-26 onward, still open)
- **Tried:** a walk cycle added to the leg bones (ported from boshjerns' SDK); for the reload glide a copy of the
  game's walk motion played on the legs only.
- **What broke:** legs still, distorted gun, legs twitching, one leg clipping, hip turning, NaN in bones,
  a garbage pointer that made the angle wrap loop forever. Each got a backstop (NaN restore, fingerprint,
  separate key cursors, angle-wrap guard).
- **Status:** circuit breaker territory (more than three failures of the same kind). Stop adding backstops.
- **Next:** write `docs/mechanics/animation.md` first: how the game itself blends a lower-body motion with an
  upper-body layer (the aim layer +0x2A8 has a partial-body flag, bit 31 of +0x44), and whether a walk motion on
  the body slot plus the aim/reload as partial layer is something the engine already supports.

## Features oliver rejected
- Sprint boost on top of the game's run (2026-10-26): reads as a cheat.
- Less sway as a silent difficulty change (2026-10-17): not shipped silently; the spread reticle replaced sway on
  his request.

## Process dead ends
- Re-deriving what re4_tweaks already had (reload without zoom, prompts, weapon hotkeys, fade). Grep it first.
- Watching the log line by line burnt the usage limit. Use the quiet watcher (`tools/logwatch.py`).
- Pickup icons: GDI drawings, then a PNG folder loader, then a download script, then compiled-in pictures — ten
  commits in one day. Ask how the pictures should ship before building a loader.
