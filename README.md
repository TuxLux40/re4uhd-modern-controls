# RE4 OG Modern Controls

Modern controls for the original **Resident Evil 4** (2005, Steam). No more tank controls:

- **Camera-relative movement.** Push the left stick toward any direction on screen and Leon goes there. He runs by
  default; hold the game's run button to walk.
- **Free camera.** The right stick turns the camera freely around Leon, and it stays where you leave it. It also sits
  a bit higher than the vanilla camera.
- **Aim where you look.** When you aim, Leon turns to face the camera's direction.
- **Walk while aiming.** Move with the left stick while you aim with the right stick. RE4 has no strafe animations,
  so Leon glides.
- **Crosshair.** A small crosshair marks where the laser points, so you can still see your aim when the laser is hard
  to make out.
- **Walls stop the camera.** The free camera stays in front of walls instead of looking through them.

Made for a controller. **Requires the Steam version** of Resident Evil 4 (2005), the current update (1.1.0). On any
other version the mod detects the different game code, logs it and changes nothing. Tested on Linux (Proton); Windows
uses the same DLL and should work the same way, but has not been tested yet.

> **Early version.** The basics work. See [what's planned](#planned) and
> [known issues](#known-issues).

## Install

1. Download the latest `RE4ModernControls-vX.Y.Z.zip` from
   [**Releases**](https://github.com/TuxLux40/re4og-modern-controls/releases).
2. In Steam, right-click **Resident Evil 4** → **Manage** → **Browse local files**.
3. Extract the zip into that folder and let it merge with the existing `Bin32` folder.
4. **Windows:** done. Start the game.

   **Linux / Steam Deck:** right-click **Resident Evil 4** → **Properties** → **Launch Options** and paste:

   ```
   WINEDLLOVERRIDES="xinput1_3=n,b" %command%
   ```

   If you also use [re4_tweaks](https://github.com/nipkownix/re4_tweaks), paste this instead:

   ```
   WINEDLLOVERRIDES="dinput8,xinput1_3=n,b" %command%
   ```

**Uninstall:** delete `Bin32/xinput1_3.dll` (and the `RE4ModernControls.ini` / `.log` next to it). On Linux, also
remove the launch option.

## Controls

| Input | Outside aiming | While aiming |
|---|---|---|
| Left stick | Move toward that direction on screen | Walk (Leon glides) |
| Right stick | Free camera | Aim |
| Run button | Hold to walk instead of run | |
| Aim | Leon turns to face where the camera looks | |

## Settings

The defaults are meant to just work. An in-game settings page is planned. Until then, the mod creates
`Bin32/RE4ModernControls.ini` on the first start for anyone who wants to fine-tune it (camera height and angle,
turn speeds, run behaviour, switching walk-while-aiming off). Changes apply while the game runs.

## Planned

- In-game settings page: run by default on/off, free or classic camera, modern or tank controls
- Smoother camera turn when you start aiming
- Reload on the move and quick turn in the RE3 (2020) style

## Known issues

- When you start aiming with the camera turned far away from Leon, the aim camera swings around quickly.
- Walking while aiming checks walls at knee height; stairs and slopes are not tested yet.

Found a bug? [Open an issue](https://github.com/TuxLux40/re4og-modern-controls/issues), and attach
`Bin32/RE4ModernControls.log` if you can.

## Credits

- **RE3Controls** for Resident Evil 5 by palecvzhope (MIT): the idea, the design and the XInput proxy this mod started
  from.
- [**re4_tweaks**](https://github.com/nipkownix/re4_tweaks) by nipkow and contributors: the reverse-engineered RE4
  structures and code locations this mod builds on.
- [**Ghidra**](https://github.com/NationalSecurityAgency/ghidra) (NSA): disassembly and decompilation of the game.
- [**REA – Reverse Engineer Anything**](https://github.com/morluto/rea) by morluto: reverse-engineering tooling for
  AI agents, used to analyse the game binary.
- [**MinHook**](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu (BSD 2-clause): function hooking.

Resident Evil is a trademark of Capcom. This project contains no game code or assets.

Developers: see [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for building and
[docs/RE-NOTES.md](docs/RE-NOTES.md) for the reverse-engineering notes.
