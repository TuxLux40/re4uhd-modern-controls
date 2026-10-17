# RE4 OG Modern Controls

Modern, camera-relative controls for the original **Resident Evil 4** (2005, Steam "UHD" release, appid 254700),
in the spirit of RE3Controls for Resident Evil 5 (Nexus Mods):

- free over-the-shoulder camera on the right stick that does not snap back behind Leon
- camera-relative movement: push the stick toward a direction on screen and Leon goes there (no tank controls)
- walking while aiming
- one-button reload, quick turn, and the rest of the RE3 (2020) layout

> **Status: early work in progress.** Nothing is playable yet. See [docs/RE-NOTES.md](docs/RE-NOTES.md) for the
> reverse-engineering log and the milestone plan.

## How it works

The game imports `XINPUT1_3.dll`. The mod is a proxy with that name placed next to `bio4.exe`: it forwards every
XInput call to the system XInput and hooks the game's player and camera code with
[MinHook](https://github.com/TsudaKageyu/minhook). `bio4.exe` is built with ASLR, so all game addresses are stored
relative to the image base, and every hook checks the expected code bytes before it patches anything.

It is meant to run side by side with [re4_tweaks](https://github.com/nipkownix/re4_tweaks), which loads through
`dinput8.dll`.

## Building

Requires [zig](https://ziglang.org/) 0.13 or newer (brings clang, lld and the mingw headers) and bash.

```sh
bash build.sh                                              # zig on PATH
ZIG="uv tool run --from ziglang python -m ziglang" bash build.sh   # zig from PyPI via uv
```

Output: `build/xinput1_3.dll`.

## Installing (Linux / Proton)

1. Copy `xinput1_3.dll` into `steamapps/common/Resident Evil 4/Bin32/`.
2. Steam launch options: `WINEDLLOVERRIDES="xinput1_3=n,b" %command%`
   (with re4_tweaks: `WINEDLLOVERRIDES="dinput8,xinput1_3=n,b" %command%`).
3. The mod writes `RE4ModernControls.log` next to the DLL.

On Windows, step 1 is enough.

## Credits

- RE3Controls (Nexus Mods, Resident Evil 5 Gold Edition) by palecvzhope (MIT): design, XInput proxy and
  build setup this project started from.
- [re4_tweaks](https://github.com/nipkownix/re4_tweaks) by nipkow and contributors: the reverse-engineered RE4
  structures and code patterns this mod builds on.
- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu (BSD 2-clause), in `third_party/minhook`.

Resident Evil is a trademark of Capcom. This project contains no game code or assets.
