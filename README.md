# Battlefield 1942 Comprehensive Modern Patch

This project is a set of patches for Battlefield 1942 (DICE / EA, 2002) that makes the game work well on modern
Windows. The patches are applied on startup without making modifications to the original executable. The following
fixes/improvements are included:

- **Online play through OpenSpy.** GameSpy is gone; the server list and server reporting go to
  [OpenSpy](https://openspy.net).
- **No registry.** The game never reads or writes the registry. The CD keys come from a `cdkey` file (in the game
  folder or the save folder) or from `game.cdKey`.
- **Relocated saves.** Profiles (options, controls, server lists), campaign progress, the server set-up and
  screenshots go to `%USERPROFILE%\Saved Games\DICE\Battlefield 1942`. Existing profiles and saves
  are copied over the first time a mod is used.
- **Widescreen.** Support for widescreen resolutions has been added with correctly scaled geometry and HUD elements.
- **HUD scale.** A HUD SCALE slider in Options → General (saved with the profile), also `game.hudScale` in
  the console.
- **Seamless menu.** The game now no longer flickers or changes resolution when joining a game or going back to the menu.
- **Borderless window.** Exclusive full screen has been removed and borderless fullscreen now works better on scaled displays.
- **No frame rate cap.** The 60 fps limit (100 with vsync off) has been removed and an optional frame limiter has been added.
- **Gamepads.** Modern controllers (Xbox, PlayStation, Switch Pro, ...) through [SDL 2](https://libsdl.org), with
  default bindings for infantry, vehicles, aircraft and the menu next to the keyboard and mouse.
- **More detail.** Buildings, vehicles and trees keep their full models three times farther out, and the terrain is
  finer and keeps its detail much farther out. View distance, fog, and object distance are untouched to respect the server's configuration.
- **Voice chat.** Talk to the players near you and to your team over the radio on servers that run the
  [LANCommander Comms](#voice-chat) server plugin.
- **FastDL.** Support for downloading missing maps over HTTP is supported when the server is enabled with FastDL support.

It only supports `BF1942.exe` 1.61 (PE timestamp `0x417564c4`; the retail 1.61 exe and the community builds of it
that only changed the master server name). Any other executable is left alone and logged. The dedicated servers
(`BF1942_w32ded.exe` 1.61, and `bf1942_lnxded` 1.61 on Linux) get only the voice server and the FastDL
advertisement; see [Dedicated server](#dedicated-server).

## Install

Download one of the packages from the [releases](https://github.com/LANCommander/LANCommander.Patches.Battlefield1942/releases)
and extract it into the folder that holds `BF1942.exe`.

| package | contents | for |
|---|---|---|
| `bf1942patch-<version>-dinput8.zip` | `dinput8.dll`, `bf1942patch.con`, `bf1942patch\` | a plain install (the game loads `dinput8.dll` from its folder) |
| `bf1942patch-<version>-asi.zip` | `bf1942patch.asi`, `bf1942patch.con`, `bf1942patch\` | an ASI loader that is already installed (such as Ultimate ASI Loader) |
| `LANCommander.Interposer.Plugin.Battlefield1942-<version>.zip` | `.interposer\Plugins\LANCommander.Interposer.Plugin.Battlefield1942.dll`, `bf1942patch\` | the [LANCommander Interposer](https://lancommander.app/Interposer) |

Put your CD key in a file named `cdkey` next to `BF1942.exe` or in the save folder (see
`bf1942patch\cdkey.example`), or set `game.cdKey "XXXX-XXXX-XXXX-XXXX-XXXX"` in `bf1942patch.con` (or type
it in the game's console).

## Voice chat

Voice chat runs on the voice library built into the [LANCommander Interposer](https://github.com/LANCommander/LANCommander.Interposer), which the `dinput8.dll` and the ASI have built in. In order for voice chat to work, the server will need a patch as well (see [Dedicated server](#dedicated-server)): it hosts the voice server, keeps its players, teams and positions in step with the game and tells clients where to find it.

- **Connecting.** When the game joins a server (from the server browser, `+joinServer`, or in game), the patch asks that server for its voice port and connects as the local player with automatic discovery.
- **Talking.** Communication with other players can be done either through proximity or radio. Proximity voice is heard by anyone within range, while radio is broadcast to your team.
- **On the HUD.** When someone talks, they will show up in your HUD underneath the minimap.
- **Lip sync.** The soldier of everyone talking moves their mouth with their voice, wider the louder they speak,
  using the face animations the game has for its radio messages.
- **Keys.** Options → Controls → Common (page 3) has PUSH TO TALK: NEARBY and PUSH TO TALK: RADIO, each with a
  primary and an alternate key, as the game's own actions.
- **Microphone.** The microphone's sensitivity for voice activation and selected device can be changed within Options → Sound.
- **Muting.** The scoreboard has a MUTE VOICE button. Pick a player in the list and click it to stop hearing them. Like buddies, mutes are kept by player name with the profile (`Settings\Profiles\<profile>\VoiceMuteList.con`), so they last across maps and sessions. The console has `voiceChat.mute "<name>"` and `voiceChat.unmute "<name>"` too.

The voice options are the `voiceChat.*` ones; [`dist/bf1942patch.con`](dist/bf1942patch.con) describes each. A server
whose voice server is somewhere else can be set with `voiceChat.server` and `voiceChat.port`.

## FastDL

The [LANCommander Interposer's FastDL](https://github.com/LANCommander/LANCommander.Interposer) downloads a file the game opens and does not have from an HTTP server.

The FastDL server serves the game's `Mods` folder (at least the maps, `Mods/<mod>/Archives/bf1942/levels/<map>.rfa`)
under its base URL, over plain `http://`. The client will read the `X-Checksum-CRC32` header for caching purposes.

## Dedicated server

The patch also runs in the dedicated server, where it does two things: it hosts the voice server for the players, and it tells them where its FastDL server is.

- Every player on the game server is a voice player, under game id engine id + 1. Clients log in by claiming their own game player, so who is in voice always matches who is on the server.
- Each team is a voice group, "Axis" or "Allies", and radio only broadcasts to the player's team.
- The positions of the live players drive nearby voice about 20 times a second (`voiceChat.proximityRange`,
  `voiceChat.proximityFullVolumeRange`). The dead drop out of nearby voice but keep the radio.
- The voice port is added as `\commsport\` to the game's GameSpy query replies, so the clients find it on their own.
- With `game.fastDlPort` set, the FastDL server is added too: `\fastdlport\`, and `\fastdlhost\` /
  `\fastdlpath\` when `game.fastDlHost` / `game.fastDlPath` are set (else the clients use the game server's address and `/`).
  The patch does not serve the files: run any HTTP server for that. This needs no voice server (`voiceChat.enabled 0`
  still advertises it).
- Windows: the server's error dialogs are logged and answered OK (`game.suppressDialogs`), so an unattended
  server never hangs on one.

**Windows (`BF1942_w32ded.exe` 1.61).** The server does not load DirectInput, so `dinput8.dll` does nothing there.
Use the Interposer plugin package (with the Interposer in the server, as LANCommander deploys it), or the ASI
package with an ASI loader that the server loads (such as Ultimate ASI Loader named `winmm.dll`). The voice library
is the server's Interposer, or the one built into the ASI.
The options and the log are those of the package (`bf1942patch.con` / `bf1942patch.log`, or the Interposer's). The
server reads the options from that file only: it has no player's file and no console commands for them.

**Linux (`bf1942_lnxded` 1.61, `.static` or `.dynamic`).** Extract `bf1942patch-<version>-linux-server.tar.gz` into
the server folder, next to `bf1942_lnxded`: `libbf1942patch.so`, `start-with-bf1942patch.sh`, `bf1942patch.con` and
`libLANCommander.Interposer.so` (32-bit, from the Interposer's `linux-x86` build). Start the server with the script
(arguments go to the server):

```sh
./start-with-bf1942patch.sh +statusMonitor 1
```

The script preloads the patch into the server only (`LD_PRELOAD`); to start it another way, preload
`libbf1942patch.so` yourself. The options are read from `bf1942patch.con` next to it (or the file
`BF1942PATCH_CONFIG` names), and each can be set with an environment variable `BF1942PATCH_<OPTION>` instead (by the
option's Config.yml name in the table below), for example `BF1942PATCH_VOICEPORT=27800` or `BF1942PATCH_VOICEADMINKEY=secret`. The log is `bf1942patch.log` next to the
server. It needs glibc 2.36 or later (Debian 12, as in LANCommander's server images).

In [`lancommander/bf1942`](https://github.com/LANCommander/LANCommander.Servers.Battlefield1942), put the four files
in `/config/Overlay/`, make the script executable and set `START_EXE` to its absolute path
(`/config/Merged/start-with-bf1942patch.sh`); a relative one is not found, because the image starts it from
`/config`. Open the voice port (UDP, default 27800) next to the game and query ports.

## Settings

Every option is a console variable named next to the game's own: `game.*` (like `game.lockFps`), `renderer.*` (like
`renderer.fieldOfView`), and `voiceChat.*` for the voice chat. Every one is described in
[`dist/bf1942patch.con`](dist/bf1942patch.con). Set one to `0` to turn that fix off.

- **`bf1942patch.con` next to the game** (shipped with the packages): the install's options. The patch reads it as it
  loads, before the game runs, so even the options that act at start-up (borderless, the save folder, the registry,
  ...) can be set there.
- **The console and the game's `.con` files.** In the game, `game.maxFps` prints the option and
  `game.maxFps 144` sets it; any `.con` file the game runs can set options the same way. Text with spaces goes
  in quotes (`voiceChat.password "two words"`). Options that a module can apply at once do so (the HUD scale,
  the frame rate limit, the gamepad's dead zone, sensitivity and keys, the voice keys, volumes, activation and
  microphone, the logging); the rest take effect when the game next starts, which it does for every map, and the
  console says so.
- **The player's `bf1942patch.con`** in the save folder (`Saved Games\DICE\Battlefield 1942`, created on the first
  start): wins over the one next to the game. Whatever is set in the game (the console, the game's `.con` files, the
  options the patch adds to the menus) is kept here, so it lasts across the game's restarts. Only `game.redirectSaves`
  and `game.saveFolder` cannot be set here (they decide where this file is), and `game.hudScale` is saved with the
  profile instead.
- **The LANCommander Interposer version** also reads `Plugins.Battlefield1942` in `.interposer\Config.yml`, keyed by
  the option's Config.yml name in the table below (`borderless: 1`), between the two files.

Coming from an older version: a `bf1942patch.ini` next to the game is still read when there is no `bf1942patch.con`,
and a `Settings.ini` in the save folder is turned into the player's `bf1942patch.con` the first time. Lines from
builds that named every option `bf1942patch.<option>` are still read.

| option | Config.yml / environment | what it does |
|---|---|---|
| `game.patchEnabled` | `enabled` | 0 = load, but patch nothing |
| `game.redirectSaves`, `game.saveFolder` | `redirectSaves`, `saveFolder` | Profiles, campaign saves, server set-up and screenshots in the Saved Games folder; another folder instead |
| `game.logFiles` | `logFiles` | Logs the redirected paths (1) or every file the game opens (2) |
| `game.noRegistry`, `game.logRegistry` | `noRegistry`, `logRegistry` | The game does not use the registry; log every registry access |
| `game.cdKey`, `game.cdKeyXPack1`, `game.cdKeyXPack2` | `cdKey`, `cdKeyXPack1`, `cdKeyXPack2` | The CD keys (empty = the `cdkey` file); the expansions use the main key unless they have their own |
| `game.installLanguage` | `language` | The install language the game is told (default English) |
| `game.openSpy`, `game.masterServer` | `openSpy`, `masterServer` | Server list and server reporting through OpenSpy; the master server (default `master.openspy.net`) |
| `renderer.borderless`, `renderer.monitor`, `renderer.vSync`, `renderer.dpiAware` | `borderless`, `monitor`, `vsync`, `dpiAware` | Borderless window on a monitor, vsync, real pixels on scaled displays |
| `renderer.modeFallback`, `renderer.nativeMenus` | `modeFallback`, `nativeMenus` | Another video mode when the profile's is not available; menus at the monitor's resolution |
| `game.unlockFps`, `game.maxFps` | `unlockFps`, `maxFps` | No 60 / 100 fps cap; the limit to use instead (0 = none) |
| `game.maxFpsOption` | `maxFpsOption` | A MAX FPS row in Options → Video |
| `game.realTimeHud` | `realTimeHud` | HUD timers and fades in real time at any frame rate |
| `game.seamlessRestart` | `seamlessRestart` | The loading picture stays up while the game restarts itself |
| `renderer.horPlus` | `horPlus` | Hor+ 3-D view and edge culling fixed |
| `game.widescreenHud`, `game.anchorHud` | `widescreenHud`, `anchorHud` | HUD and menus at their own shape; HUD elements at the screen edges |
| `game.hudScale`, `game.hudScaleOption` | `hudScale`, `hudScaleOption` | HUD size in percent; the slider in Options → General |
| `renderer.meshLod`, `renderer.treeLod` | `meshLod`, `treeLod` | Mesh LOD and tree billboard distances in percent |
| `renderer.terrainLod`, `renderer.terrainDetail` | `terrainLod`, `terrainDetail` | Terrain LOD distance and triangle budget in percent |
| `game.gamepad` | `gamepad` | Gamepads through SDL, offered to the game as its first joysticks |
| `game.gamepadDeadzone`, `game.gamepadSensitivity` | `gamepadDeadzone`, `gamepadSensitivity` | Stick dead zone in percent; response curve |
| `game.gamepadBindings` | `gamepadBindings` | Run `Settings\Gamepad.con` (default bindings, written the first time) after the profile's controls |
| `game.gamepadHideDuplicates`, `game.gamepadStartKey`, `game.gamepadBackKey` | `gamepadHideDuplicates`, `gamepadStartKey`, `gamepadBackKey` | Hide a pad's own DirectInput entry; keys Start / Back press |
| `voiceChat.enabled` | `voiceChat` | Voice chat |
| `voiceChat.port`, `voiceChat.password`, `voiceChat.server` | `voicePort`, `voicePassword`, `voiceServer` | The voice server's port if the game server does not say, its password, its host if it is not the game server's |
| `game.queryPorts` | `voiceQueryPorts` | The game server's query ports, asked for its voice port and FastDL server |
| `voiceChat.voiceActivation`, `voiceChat.radioActivation` | `voiceActivation`, `radioActivation` | Voice activation instead of push to talk, nearby and on the radio (nearby also as VOICE INPUT MODE in Options → Sound) |
| `voiceChat.proximityKey`, `voiceChat.radioKey` | `proximityKey`, `radioKey` | The push-to-talk keys (also in Options → Controls → Common) |
| `voiceChat.proximityKeyAlternate`, `voiceChat.radioKeyAlternate` | `proximityKeyAlternate`, `radioKeyAlternate` | The alternate push-to-talk keys; `none` by default (also in Options → Controls → Common) |
| `voiceChat.volume`, `voiceChat.micSensitivity` | `voiceVolume`, `micSensitivity` | Voice volume and microphone sensitivity in percent (also in Options → Sound) |
| `voiceChat.inputDevice` | `inputDevice` | The microphone's device id; empty = the system's default (also in Options → Sound) |
| `voiceChat.hud`, `voiceChat.options` | `voiceHud`, `voiceOptions` | Who is talking on the HUD; the rows in the Controls and Sound options |
| `voiceChat.muteButton` | `voiceMuteButton` | The MUTE VOICE button on the scoreboard (and the single ADD / DROP BUDDY button) |
| `voiceChat.library` | `voiceLibrary` | Linux server: the voice library when the server has no Interposer |
| `game.fastDl`, `game.fastDlUrl` | `fastDl`, `fastDlUrl` | Missing maps downloaded through the Interposer's FastDL; a FastDL server for game servers that name none |
| `game.fastDlPort`, `game.fastDlHost`, `game.fastDlPath` | `fastDlPort`, `fastDlHost`, `fastDlPath` | Dedicated server: the FastDL server its query replies name (port 0 = none; host "" = the server's) |
| `voiceChat.bindAddress`, `voiceChat.adminKey` | `voiceBindAddress`, `voiceAdminKey` | Dedicated server: the address the voice server listens on; its remote admin key |
| `voiceChat.maxPlayers`, `voiceChat.maxBitrate` | `voiceMaxPlayers`, `voiceMaxBitrate` | Dedicated server: the most players in voice; the voice bitrate |
| `voiceChat.proximityRange`, `voiceChat.proximityFullVolumeRange` | `proximityRange`, `proximityFullVolumeRange` | Dedicated server: how far nearby voice carries, in metres |
| `voiceChat.includeBots` | `voiceIncludeBots` | Dedicated server: the bots are voice players too (testing) |
| `game.suppressDialogs` | `suppressDialogs` | Dedicated server: error dialogs are logged and answered OK |
| `game.logToStderr` | `logToStderr` | Linux server: the log goes to the console too |
| `game.crashLog` | `crashLog` | Logs crashes with registers and a call chain to `bf1942patch.log` |
| `game.logHud` | `logHud` | Logs every HUD element and the screen edge it is anchored to |
| `game.logFps` | `logFps` | Logs the frame rate every 5 seconds |

## Building

The patch builds with clang for the MSVC target, because the `dinput8.dll` and the ASI link in the
[LANCommander Interposer](https://github.com/LANCommander/LANCommander.Interposer)'s static library
(`LANCommander.Interposer.Static`, MSVC C++). It requires:

- LLVM's `clang` and `lld-link` (the LLVM installer, or MSYS2:
  `pacman -S mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-lld`), run from Git Bash or MSYS2's bash
- Visual Studio 2022 or later with the C++ x86 tools and a Windows SDK (only its headers and libraries are used)
- [vcpkg](https://vcpkg.io) in `VCPKG_ROOT`, for SDL 2 (`vcpkg.json`)
- the Interposer next to this repository (or in `INTERPOSER_DIR`), its static library built for x86:

```
git clone https://github.com/LANCommander/LANCommander.Interposer.git
cd LANCommander.Interposer
powershell -File build\Get-YYThunks.ps1
cmake --preset windows-x86
cmake --build out/windows-x86 --config Release --target LANCommander.Interposer.Static
cd ..
```

SDL and the Interposer are linked statically, so the patch stays a single DLL (SDL is under the
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

The Linux server build needs a 32-bit gcc on Linux (`gcc-multilib`); build it on Debian 12 so it runs on the
LANCommander server images. From Windows, with Docker:

```
docker run --rm -v "$PWD:/src" -w /src debian:12 sh -c \
  'apt-get update -qq && apt-get install -y -qq gcc-multilib libc6-dev-i386 > /dev/null && sh build-linux.sh'
```

Output: `build/linux/libbf1942patch.so` and `build/linux/start-with-bf1942patch.sh`.

The `dinput8.dll` and the ASI export the Interposer's API (`Interposer*`, `interposer_comms_*`) as
`LANCommander.Interposer.dll` does, and the patch calls it through those exports, as the Interposer plugin does with
its host's (`src/interposer/` holds the headers of the Interposer SDK).

GitHub Actions (`.github/workflows/build.yml`) builds every push and pull request, with the Interposer's `main` (or
the branch or tag in the repository variable `INTERPOSER_REF`). With `INTERPOSER_SO_URL` (a 32-bit
`libLANCommander.Interposer.so`), the Linux server package includes it.

## Repository layout

| path | contents |
|---|---|
| `src/` | The patch:<br>- `bf1942patch.c` (exe check, guarded memory writes, the game's `std::string`) and `iat.c` (import table hooks)<br>- `settings.c` (the options and their `.con` parser, shared by Windows and Linux), `settings_game.c` (where they are read from, the player's file, the console commands)<br>- `paths.c`, `registry.c`, `saves.c`, `master.c`<br>- `d3d.c` (Direct3D 8 hooks), `window.c` (borderless), `restart.c` (seamless restarts, the rundll32 helper)<br>- `widescreen.c` (Hor+, HUD shape and anchoring), `menu.c` (HUD scale slider, console command)<br>- `framerate.c` (frame rate cap, HUD timing), `detail.c` (mesh, tree and terrain LOD)<br>- `gamepad.c` (SDL gamepads as DirectInput 8 joysticks, `Settings\Gamepad.con`)<br>- `voice.c` (voice chat through the Interposer), `voice_net.c` (finding the voice server), `voice_hud.c` (who is talking: HUD rows grafted by `menu.c`, the icons' archive), `voice_icons.c` (the icons, from `icons/`), `voice_menu.c` (the keys and volumes in the options; rows grafted by `menu.c`), `voice_scoreboard.c` (the scoreboard's MUTE VOICE and ADD / DROP BUDDY buttons), `interposer/` (the Interposer's headers)<br>- `fastdl.c` (the game server's FastDL server, handed to the Interposer as the game joins)<br>- `voice_server.c` (the dedicated server's voice server, shared by Windows and Linux), `voice_server_win.c` (in `BF1942_w32ded.exe`), `linux/` (`libbf1942patch.so` for `bf1942_lnxded`, its start script)<br>- the loaders `dinput8_proxy.c`, `asi.c`, `interposer_plugin.c`, and `crashlog.c` |
| `dist/` | `bf1942patch.con` and `cdkey.example`, shipped with the packages |

The game files are not included. You need your own copy of the game.

## License

[MIT](LICENSE). Battlefield 1942 itself is not included; you need your own copy of the game.
