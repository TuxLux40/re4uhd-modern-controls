# Reverse-engineering notes

Target: `Bin32/bio4.exe`, Steam appid 254700 (depot 254704), 9 139 840 bytes,
SHA-256 `19aed4af0ab06a748ff8744d45ac5580fcd6be6b6b7e944b1ab8822a00c8ee4a`.

- PE32, image base 0x400000, `DllCharacteristics` 0x8140: **ASLR on** (`DYNAMIC_BASE`). Store RVAs, never VAs.
- No SteamStub (`.bind`) section: the code is plain on disk, hooks can go in as soon as the DLL loads.
- Imports `XINPUT1_3.dll` (our proxy) and `DINPUT8.dll` (re4_tweaks' ASI loader), so both mods load side by side.

Addresses below are written as VA at the default base 0x400000. Subtract 0x400000 for the RVA.

## Known from re4_tweaks (pattern-scanned there, see `dllmain/Game.cpp`, `MouseTurning.cpp`, `SDK/`)

| What | How re4_tweaks finds it | Notes |
|---|---|---|
| `pPL` (player `cPlayer*`) | `A1 ? ? ? ? D8 CC D8 C9 D8 CA D9 5D ? D9 45 ?` +1 | `cCoord`: `pos_94`, `ang_A0` (yaw = `.y`, radians) |
| `pG` (`GLOBAL_WK*`) | `A1 ? ? ? ? B9 FF FF FF 7F 21 48 ? A1` +1 | `deltaTime_70` (frames at 30 fps) |
| `CamCtrl` (`CameraControl`) | `D9 EE B9 ? ? ? ? D9 9E A4 00 00 00 E8 ? ? ? ? D9 EE` +3 | `Camera_60` (`CAMERA`: `CamPoint_A4` = campos/target), `m_QuasiFPS_278` |
| `Key_btn_on` (u64 button bits) | `A1 ? ? ? ? 33 C9 83 E0 ? 83 3D` +1 | game-side action bits after mapping |
| `Joy[4]` | `83 E0 10 33 C9 0B C1 0F 84 ? ? ? ? 0F BE 05 ? ? ? ?` +0x10, -9 | `leftStick_X_0` / `_Y_4` (int), right stick int8 |
| `KeyOnCheck_0` | call in `E8 ? ? ? ? 83 C4 ? 84 C0 74 ? C7 86 ...` | turn left/right decision |
| player state functions | `pl_R1_Walk`, `pl_R1_Run`, `pl_R1_Back`, `pl_R1_Turn`, `pl_R1_Turn180` (patterns in `MouseTurning.cpp`) | `r_no_*` bytes in `cModel` select the routine |
| root-motion off | `cModel.Motion_1D8.Mot_attr_40 = 4` | MotionMove applies no rotation/translation (re4_tweaks' mouse turning relies on it) |

## Confirmed in this build (2026-10-17)

All re4_tweaks patterns above match exactly once. Resolved (VA at base 0x400000):

| What | VA | Notes |
|---|---|---|
| `pPL` | 0x00C57054 | `cPlayer**` |
| `pG` | 0x00C06F3C | `GLOBAL_WK**` |
| `CamCtrl` | 0x00C702D0 | the object; `m_QuasiFPS_278` at 0x00C70548 |
| `Key_btn_on` | 0x00C62F08 | |
| `Joy[4]` | 0x00C63008 | |
| `CameraControl::move` | 0x006F6EF0 | thiscall, reached via ILT thunk 0x00401B4A. Switches on `r0_34` (camera mode), the mode writes `m_camera_38` (campos/target/roll/fovy), then smoothing (0x006F2AD0, 0x006F2DA0) and the view matrix build (0x0059E660 on `Camera_60`). Mode 7 (0x006F3220) reads Leon's yaw (`pPL+0xA4`). |
| `KeyConvert` | 0x00967360 | `void(void)`, 13.5 KB: turns the pad into `Key_btn_on`/`trg` bits each frame; called from 0x5AC058 and 0x654B80 via thunk 0x407509. `pl_R1_Walk`/`pl_R1_Run` test `Key_btn_on & KEY_RUN (0x40)`. |
| `CameraQuasiFPS::move` | 0x0059E130 | thiscall, ECX = CamCtrl+0x278; mode 10 copies its CamPoint (+0xA4) into `m_camera_38` |
| `pl_R1_Walk` / `Run` / `Turn180` | 0x00763331 / 0x00763771 / 0x00763DC5 | pattern hits (inside the functions) |

| `Joy[0]` | 0x00C63008 | Filled in KeyConvert from the pad device object `0x00964670(0)` (floats: +0xC/+0x10 left stick, +0x14/+0x18 right stick), x127, clamp, dead zone 31 -> +-63. While a bound action (aim) is held the branch at 0x009679BD swaps the sticks: Joy left <- right stick. `PlWepLockCtrl` (0x00688090) aims from Joy left + on bits (0x00C6301C). `0x012224B0 == 1` takes a separate (mouse) aim path. |

| Trace | 0x00594160 | thiscall `Trace(from, to, hit, 0, maskA, maskB)`, ret 0x18, nonzero on hit. Collision sets: 0x00BF9A7C (world: Leon's movement masks 0/0x800, game camera 0x8000/0 at 0x006F69A9), 0x00BF9AA0 (laser: 0/0x400000). `FUN_005BC050` = laser trace: world first, then models (returns 2/3 on a model). |
| LaserUpdate | 0x0073EA60 | thiscall (char draw, char, char): beam start 0x00C79818, end 0x00C79828 (cut at the first hit), drawn when `draw`. |

Tooling notes: calls go through 5-byte `jmp` thunks (incremental linking), so call sites name the thunk, not the
function. Ghidra's decompiler drops the implicit `this` of thiscall callees; find methods of a global object by
searching the disassembly for `mov ecx,<object>` followed by `call`.

## Plan

| Milestone | Goal | Status |
|---|---|---|
| M0 | Proxy loads under Proton, forwards XInput, logs the stick; read-only probe on `CameraControl::move` logs the camera mode | done 2026-10-17 (loads at base 0x400000; gameplay camera = mode 10) |
| M1 | Free camera on the right stick: after `CameraQuasiFPS::move` (0x0059E130, mode 10) rotate the game's eye/target around Leon to our own yaw; aim hands back to the game camera | yaw works (oliver, 2026-10-17); free pitch + height offset work (oliver, 2026-10-17) |
| M2 | Camera-relative movement: turn Leon toward camera yaw - stick angle, feed the game "forward" | works (oliver, 2026-10-17, with run forced via Steam Input); `Run=Always` via `KEY_RUN` after KeyConvert works |
| M3 | Walk while aiming: the game already aims with the right stick (KeyConvert swaps the sticks into `Joy[0]` while aim is held); the mod hides the left stick and moves `pos_94` along it relative to the aim camera | walking confirmed; aim fix: log shows Leon turning and walking while aiming (2026-10-17); collision unknown |
| M3b | Camera wall collision (trace head -> eye), aim-walk wall collision (knee-height trace, slide), crosshair at the projected laser end (D3D9 Present + Clear rects), aim-walk 1300 | built, untested |
| M4 | Reload on the move, quick turn, button layout | |
| M5 | In-game options menu entries: run default (Always / OuterRing / Button), free / default camera, modern / tank controls (the ini keeps working as the fallback) | planned |

The camera has to come first: RE4's camera sits behind Leon's yaw, so turning Leon toward the stick without a
decoupled camera turns the camera too, and Leon would circle forever.
