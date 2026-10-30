# Aim — how the game does it

Sources: REA (Ghidra 12.1.2) on bio4.exe, evidence bundle `~/Projects/re4og-ghidra/out/rea-evidence-2026-10-29.json`
(PlWepLockCtrl pseudocode `ev_7d9c46f8…`, ArmPoseCtrl_Init callers `ev_4501ee2d…`), the Ghidra text dump, and the
mod's diag log. Each claim is marked **observed** (seen in code or log), **inferred** (reasoned from observed code)
or **unknown**. See also RE-NOTES.md.

## Names used here (VA at base 0x400000)
| Address | Name | Role |
|---|---|---|
| 0x688090 | PlWepLockCtrl | per-frame aim from the stick: vertical value, sideways cone, body turn, then sway and both poses |
| 0x684330 | PlAimSway | sway/breathing on the vertical value and the side value (we hook it) |
| 0x67CE00 | ArmPoseCtrl_SetV | thiscall on 0xC7A7E8: arm up/down blend from the vertical value |
| 0x67CD00 | SidePoseCtrl_Ease | thiscall on `*(cPlayer+0x7E0)`: eases its +4 toward the side value, rate 0.4 |
| 0x67E190 | ArmPoseCtrl_Init | sets body motion, base/up/down motions and starts the aim layer at weight 0 |
| 0x67DE90 | PlLaserTick | wrapper that ends in LaserUpdate |
| 0x73EA60 / 0x73DC60 | LaserUpdate / LaserLine | gun line from the bones, cut at the first hit, drawn |
| 0x6860C0 | PlAimValuesReset | rewrites kAimV/kAimVStart (aim start) |

## PlWepLockCtrl, step by step (observed, REA pseudocode)
1. Picks per-weapon constants from the weapon kind (`0x682AC0`):
   - **side cone** `local_c`: 0.209 rad (12°) default; 0.105 (6°), 0.063 (3.6°), 0.052 (3°) or 0.0065 (0.4°) for
     some kinds. The left limit is `-0.8 * local_c`.
   - vertical speed `local_14` and turn speed `local_10` (0.8–1.5).
2. **Vertical:** stick Y × ramp × speed is added to `kAimV` (0xC7A7D8), ×0.8 while the value is positive, clamped
   to -1..1. `kAimVStart` follows it, eased by `kAimVBlend` when that is non-zero.
3. **Sideways:** stick X is added to `cPlayer+0x448`. Inside the cone that is all. At the cone edge the value is
   clamped and the rest **turns Leon's heading** `cPlayer+0xA4` (= `Ang+4`).
   So `+0x448` is the gun's free play inside a small cone, and turning the body is how the game aims further.
4. Calls PlAimSway(pl, moved, &v, &side), writes the result back to kAimV.
5. `ArmPoseCtrl_SetV(kAimVStart)` and `SidePoseCtrl_Ease(side, 0.4)`.

**Consequence (inferred):** anything that pushes `+0x448` past ±`local_c` produces poses the game never makes.
That is why the 30° side twist broke (dead_ends.md). The heading is the right lever for sideways convergence.

## Arm pose controller 0xC7A7E8 (observed)
- `[0]` model, `[1]` value, `[2..4]` base/up/down motions, layer at `+0x18`, layer weight `layer+200 = |v|`
  (negative values scaled by a constant).
- SetV switches the layer motion to up or down only when the value's sign changes.
- ArmPoseCtrl_Init (0x67E190) is called from **52 functions** (REA, resolved direct calls through thunk 0x4021DA:
  0x753660…, 0x8E31A0–0x8E9100, 0x8FCAA0–0x8FF0xx, 0x9057C0–0x908650, 0x9124C0–0x925670).
  ArmPoseCtrl_SetV is called from 78 functions. These are the per-weapon, per-state routines.
- The raise routines pass the raise motion as base, up and down, so the arm value has no effect until live aim
  (observed in the log for the shotgun, 2026-10-29).
- Live aim (`r_no` 6/1/0) starts about 0.35–0.55 s after the raise begins (log).

## Frame order
- Inside a weapon's aim routine: PlWepLockCtrl, then PlLaserTick (0x8FDFB0, 0x9212C0); one routine (0x907C60) has
  the laser first. **Observed** call order in the code.
- The laser reads the gun bone matrices. The skeleton is built in cModel_MotionUpdate later in the frame.
  **Inferred:** the laser measured in frame N shows the pose from frame N-1, so a correction made from it lands one
  frame late. While Leon turns, the measured error includes that lag.
- Player task vs camera task order: **unknown** statically (both run from scheduler tables; no direct callers).
  Runtime hint: the shown view trails the camera hook by a frame while turning (commit 46ca9cb).

## The game's controls while aiming
| Value | Meant for | Range | Measured scale | Evidence |
|---|---|---|---|---|
| heading `cPlayer+0xA4` | sideways aim beyond the cone | any | — | observed (code) |
| `kAimV` / `kAimVStart` | up/down blend | -1..1 | ~60° gun pitch per unit | observed (code), measured (log) |
| side `cPlayer+0x448` | free play inside the cone | -0.8·c..c, c = 0.4°–12° by weapon | ~0.85 rad per unit | observed (code), measured (log) |
| sway anchor `(cPlayer+0x7D8)+0x28/+0x2C` | the point the sway drifts around | — | — | observed (log) |

## What the mod changes (milestone 2026-10-30, play-tested)
- **Heading:** left to the game and the stick. No convergence offset: the gun stays parallel to the view, a few
  degrees beside the reticle sideways (shoulder parallax). Bullets go to the centre through the Bullet hook.
- **Arm:** `pitch / 1.047` (about 60 deg of gun pitch per unit), no feedback.
- **Sway:** both outputs back to the anchor (`+0x28` vertical, `+0x2C` sideways). The sideways part also moves the
  view, which follows `+0x448`.
- **Laser:** not bent.
- **Raise:** handgun family (raise 0x008FCAA0, weapon motion table `*(pG+0x44)`): the raise layer gets the table's
  `+0x9C`/`+0xA0` as up/down motions, exactly what the game's own modern-mouse-aim path (`0x012224B0 == 1`) passes.
  Live aim (0x008FD390) uses `+0x98/+0x9C/+0xA0`; verified live. Other families still learn their motions on the
  first live aim.

## Open questions
- **First raise of the other weapon families:** map them from the `0x012224B0 == 1` branches of their raise routines
  (raise offset -> up/down offsets in `*(pG+0x44)`): seen so far 0x48 -> 0x54/0x58, 0x7C -> 0x90/0x9C, 0x68 ->
  0x78/0x80 or 0x7C/0x84 (two routines; the offset alone is ambiguous, key by routine or weapon kind).
- **Laser at the extremes:** aiming far up or down, the laser lands above the reticle (oliver 2026-10-30). The arm's
  60 deg per unit is a fit from mid angles; check `diag gun` pitch vs the view's pitch near +-1.
- Which weapon kind maps to which `local_c`: kinds from `0x682AC0` not yet mapped to weapon ids.
