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

## Plan

| Milestone | Goal | Status |
|---|---|---|
| M0 | Proxy loads under Proton, forwards XInput, logs the stick | built, untested |
| M1 | Free orbit camera on the right stick (hook the QFPS camera update, write our own campos/target outside aim) | |
| M2 | Camera-relative movement: turn Leon toward camera yaw + stick angle, feed the game "forward" | |
| M3 | Walk while aiming: move `pos_94` along the stick in the aim states, keep collision | |
| M4 | Reload on the move, quick turn, button layout, ini | |

The camera has to come first: RE4's camera sits behind Leon's yaw, so turning Leon toward the stick without a
decoupled camera turns the camera too, and Leon would circle forever.
