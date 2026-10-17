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

Push a tag `vX.Y.Z`. The `release` workflow builds the DLL with that version, packages
`RE4ModernControls-vX.Y.Z.zip` (`Bin32/xinput1_3.dll`, install notes, licence) and publishes a GitHub release.
Every push to `main` also builds and uploads the zip as a workflow artifact.

## Log

`Bin32/RE4ModernControls.log` (rewritten on every start): load and hook results, config, and one line whenever the
camera mode, Leon's routine bytes or the aim keys change.
