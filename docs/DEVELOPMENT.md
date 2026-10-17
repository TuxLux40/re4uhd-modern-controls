# Development

## How it works

The game imports `XINPUT1_3.dll`. The mod is a proxy with that name placed next to `bio4.exe`: it forwards every
XInput call to the system XInput, rewrites pad 0 before the game sees it, and hooks the game's camera and input code
with [MinHook](https://github.com/TsudaKageyu/minhook). `bio4.exe` is built with ASLR, so all game addresses are
stored relative to the image base (`src/re4.h`), and every hook checks the expected code bytes before it patches
anything. It runs side by side with re4_tweaks, which loads through `dinput8.dll`.

Code map:

| File | What |
|---|---|
| `src/xinput_proxy.cpp` | XInput forwarding, import-slot redirection |
| `src/camera.cpp` | free camera, camera-relative movement, run mode, walk while aiming |
| `src/config.cpp` | `RE4ModernControls.ini`: defaults, live reload |
| `src/game.cpp` | hook setup, the `CameraControl::move` probe that logs camera mode / Leon's state |
| `src/re4.h` | addresses and struct offsets for this `bio4.exe` build |
| `src/crosshair.cpp` | laser end point -> screen, D3D9 `Present` hook drawing the reticle |
| `src/default.ini` | the settings file template: embedded into the DLL by `build.sh` and shipped to the installers |
| `installer/` | `install.sh` (Linux/Deck: Steam library search, Proton `AppDefaults\bio4.exe\DllOverrides` in the prefix's `user.reg`), `install.ps1` + `install.bat` (Windows) |

Reverse-engineering notes, addresses and the milestone plan: [RE-NOTES.md](RE-NOTES.md).

## Building

Requires [zig](https://ziglang.org/) (0.17.0 is what CI uses; it brings clang, lld and the mingw headers), bash and
python3.

```sh
bash build.sh                                                         # zig on PATH -> build/xinput1_3.dll
ZIG="uv tool run --from ziglang==0.17.0 python -m ziglang" bash build.sh  # zig from PyPI via uv
VERSION=0.1.0 bash build.sh package                                   # + dist/RE4ModernControls-v0.1.0.zip
```

## Releasing

Push a tag `vX.Y.Z`. The workflow builds the DLL with that version and packages `RE4ModernControls-vX.Y.Z.zip`
(installers, `files/xinput1_3.dll`, `files/default.ini`, README, licence). Before publishing it tests the Linux
installer against a fake Steam library, the Windows installer on a Windows runner, and loads the DLL into a 32-bit
Windows process to check it starts and stays inactive outside RE4. Only then is the GitHub release created.
Every push to `main` runs the same build and tests and uploads the zip as a workflow artifact.

## Log

`Bin32/RE4ModernControls.log` (rewritten on every start): load and hook results, config, and one line whenever the
camera mode, Leon's routine bytes or the aim keys change.
