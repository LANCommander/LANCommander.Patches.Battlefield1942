# Battlefield 1942 Comprehensive Modern Patch

This project is a set of patches for Battlefield 1942 (DICE / EA, 2002) that makes the game work well on modern
Windows. The patches are applied on startup without making modifications to the original executable. The following
fixes/improvements are included:

- **Online play through OpenSpy.** GameSpy is gone; the server list and server reporting go to
  [OpenSpy](https://openspy.net).
- **No registry.** The game never reads or writes the registry. The CD keys come from a `cdkey` file (in the game
  folder or the save folder) or from the settings.
- **Relocated saves.** Profiles (options, controls, server lists), campaign progress, the server set-up and
  screenshots go to `%USERPROFILE%\Saved Games\DICE\Battlefield 1942`. Existing profiles and saves
  are copied over the first time a mod is used.
- **Widescreen.** Support for widescreen resolutions has been added with correctly scaled geometry and HUD elements.
- **HUD scale.** A HUD SCALE slider in Options → General (saved with the profile), also `bf1942patch.hudScale` in
  the console.
- **Seamless menu.** The game now no longer flickers or changes resolution when joining a game or going back to the menu.
- **Borderless window.** Exclusive full screen has been removed and borderless fullscreen now works better on scaled displays.
- **No frame rate cap.** The 60 fps limit (100 with vsync off) has been removed and an optional frame limiter has been added.
- **Gamepads.** Modern controllers (Xbox, PlayStation, Switch Pro, ...) through [SDL 2](https://libsdl.org), with
  default bindings for infantry, vehicles, aircraft and the menu next to the keyboard and mouse.
- **More detail.** Buildings, vehicles and trees keep their full models three times farther out, and the terrain is
  finer and keeps its detail much farther out. View distance, fog, and object distance are untouched to respect the server's configuration.

It only supports `BF1942.exe` 1.61 (PE timestamp `0x417564c4`; the retail 1.61 exe and the community builds of it
that only changed the master server name). Any other executable is left alone and logged.

## Install

Download one of the packages from the [releases](https://github.com/LANCommander/LANCommander.Patches.Battlefield1942/releases)
and extract it into the folder that holds `BF1942.exe`.

| package | contents | for |
|---|---|---|
| `bf1942patch-<version>-dinput8.zip` | `dinput8.dll`, `bf1942patch.ini`, `bf1942patch\` | a plain install (the game loads `dinput8.dll` from its folder) |
| `bf1942patch-<version>-asi.zip` | `bf1942patch.asi`, `bf1942patch.ini`, `bf1942patch\` | an ASI loader that is already installed (such as Ultimate ASI Loader) |
| `LANCommander.Interposer.Plugin.Battlefield1942-<version>.zip` | `.interposer\Plugins\LANCommander.Interposer.Plugin.Battlefield1942.dll`, `bf1942patch\` | the [LANCommander Interposer](https://lancommander.app/Interposer) |

Put your CD key in a file named `cdkey` next to `BF1942.exe` or in the save folder (see
`bf1942patch\cdkey.example`), or set `[CDKey] Key=` in `bf1942patch.ini`.

## Settings

Every option is described in [`dist/bf1942patch.ini`](dist/bf1942patch.ini). Set a key to `0` to turn that fix off.
A `Settings.ini` in the save folder (`Saved Games\DICE\Battlefield 1942`) with the same sections and keys
overrides `bf1942patch.ini`. The LANCommander Interposer version can load the same values from `Config.yml`.

| section | key | what it does |
|---|---|---|
| `[Saves]` | `Redirect` | Profiles, campaign saves, server set-up and screenshots in the Saved Games folder |
| | `Folder` | Use this folder instead (empty = `Saved Games\DICE\Battlefield 1942`) |
| `[Registry]` | `UseFile` | The game does not use the registry |
| | `LogAccess` | Logs every registry access of the game |
| `[CDKey]` | `Key`, `XPack1`, `XPack2` | The CD keys (empty = the `cdkey` file); the expansions use the main key unless they have their own |
| `[Game]` | `Language` | The install language the game is told (default English) |
| `[Network]` | `OpenSpy` | Server list and server reporting through OpenSpy |
| | `MasterServer` | The master server (default `master.openspy.net`) |
| `[Display]` | `Borderless`, `Monitor`, `VSync`, `DpiAware` | Borderless window on a monitor, vsync, real pixels on scaled displays |
| `[FrameRate]` | `Unlock`, `MaxFps` | No 60 / 100 fps cap; the limit to use instead (0 = none) |
| | `MaxFpsOption` | A MAX FPS row in Options → Video |
| | `FixHud` | HUD timers and fades in real time at any frame rate |
| `[Restart]` | `Seamless` | The loading picture stays up while the game restarts itself |
| `[Widescreen]` | `HorPlus` | Hor+ 3-D view and edge culling fixed |
| | `FixHud`, `AnchorHud` | HUD and menus at their own shape; HUD elements at the screen edges |
| | `HudScale`, `HudScaleOption` | HUD size in percent; the slider and console command |
| `[Detail]` | `MeshLod`, `TreeLod` | Mesh LOD and tree billboard distances in percent |
| | `TerrainLod`, `TerrainDetail` | Terrain LOD distance and triangle budget in percent |
| `[Gamepad]` | `UseSDL` | Gamepads through SDL, offered to the game as its first joysticks |
| | `Deadzone`, `Sensitivity` | Stick dead zone in percent; response curve |
| | `Bindings` | Run `Settings\Gamepad.con` (default bindings, written the first time) after the profile's controls |
| | `HideDuplicates`, `StartKey`, `BackKey` | Hide a pad's own DirectInput entry; keys Start / Back press |
| `[Debug]` | `CrashLog` | Logs crashes with registers and a call chain to `bf1942patch.log` |
| | `LogHud` | Logs every HUD element and the screen edge it is anchored to |
| | `LogFps` | Logs the frame rate every 5 seconds |

## Building

Requires MSYS2 with the 32-bit MinGW toolchain and SDL 2:

```
pacman -S mingw-w64-i686-gcc mingw-w64-i686-SDL2 pkgconf
```

SDL is linked statically, so the patch stays a single DLL (SDL is under the
[zlib license](https://github.com/libsdl-org/SDL/blob/SDL2/LICENSE.txt)).

```
git clone https://github.com/LANCommander/LANCommander.Patches.Battlefield1942.git
cd LANCommander.Patches.Battlefield1942
sh build.sh
```

Output:
- `build\dinput8.dll`: the standalone patch (a DirectInput 8 proxy that forwards to the system DLL).
- `build\bf1942patch.asi`: the same patches as an ASI plugin.
- `build\LANCommander.Interposer.Plugin.Battlefield1942.dll`: the Interposer plugin.

`BF_VERSION=v1.2.3 sh build.sh` sets the version the patch writes to its log (`dev` otherwise).

GitHub Actions (`.github/workflows/build.yml`) builds every push and pull request.

## Repository layout

| path | contents |
|---|---|
| `src/` | The patch:<br>- `bf1942patch.c` (exe check, settings, guarded memory writes, the game's `std::string`) and `iat.c` (import table hooks)<br>- `paths.c`, `registry.c`, `saves.c`, `master.c`<br>- `d3d.c` (Direct3D 8 hooks), `window.c` (borderless), `restart.c` (seamless restarts, the rundll32 helper)<br>- `widescreen.c` (Hor+, HUD shape and anchoring), `menu.c` (HUD scale slider, console command)<br>- `framerate.c` (frame rate cap, HUD timing), `detail.c` (mesh, tree and terrain LOD)<br>- `gamepad.c` (SDL gamepads as DirectInput 8 joysticks, `Settings\Gamepad.con`)<br>- the loaders `dinput8_proxy.c`, `asi.c`, `interposer_plugin.c`, and `crashlog.c` |
| `dist/` | `bf1942patch.ini` and `cdkey.example`, shipped with the packages |

The game files are not included. You need your own copy of the game.

## License

[MIT](LICENSE). Battlefield 1942 itself is not included; you need your own copy of the game.
