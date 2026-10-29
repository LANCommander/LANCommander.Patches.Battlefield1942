#!/bin/sh
# Builds, with MSYS2 mingw32 gcc:
#   build/dinput8.dll                                         standalone patch (dinput8.dll proxy)
#   build/bf1942patch.asi                                     the same patches as an ASI plugin (for an ASI loader)
#   build/LANCommander.Interposer.Plugin.Battlefield1942.dll  LANCommander Interposer plugin (same patches)
# BF_VERSION=v1.2.3 sets the version the patch logs (default "dev").
set -e
cd "$(dirname "$0")"
export PATH="/c/msys64/mingw32/bin:$PATH"
OUT=build
mkdir -p $OUT
CFLAGS="-m32 -O2 -Wall -Wextra -shared -static-libgcc -s"
CORE="src/bf1942patch.c src/settings.c src/settings_game.c src/iat.c src/paths.c src/crashlog.c src/registry.c src/saves.c src/master.c src/d3d.c src/window.c src/restart.c src/widescreen.c src/menu.c src/gamepad.c src/framerate.c src/detail.c src/voice.c src/voice_net.c src/voice_hud.c src/voice_menu.c src/voice_scoreboard.c src/voice_icons.c src/voice_lips.c src/fastdl.c src/voice_server.c src/voice_server_win.c"
VERSION_FLAG="-DBF_VERSION=\"${BF_VERSION:-dev}\""

# SDL2 (gamepads), linked statically: the patch stays a single DLL
SDL_PREFIX=$(sdl2-config --prefix)
CFLAGS="$CFLAGS -I$SDL_PREFIX/include/SDL2"
SDL_LIBS="$SDL_PREFIX/lib/libSDL2.a -lm -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -lws2_32 -lwsock32"

gcc $CFLAGS "$VERSION_FLAG" -o $OUT/dinput8.dll \
    src/dinput8_proxy.c $CORE src/dinput8.def \
    -Wl,--enable-stdcall-fixup -Wl,--kill-at $SDL_LIBS
objdump -p $OUT/dinput8.dll | grep -A10 "Ordinal/Name Pointer" | sed -n '2,8p'

gcc $CFLAGS "$VERSION_FLAG" -o $OUT/bf1942patch.asi \
    src/asi.c $CORE -Wl,--exclude-all-symbols -Wl,--kill-at $SDL_LIBS

gcc $CFLAGS "$VERSION_FLAG" -o $OUT/LANCommander.Interposer.Plugin.Battlefield1942.dll \
    src/interposer_plugin.c $CORE \
    -Wl,--kill-at $SDL_LIBS
objdump -p $OUT/LANCommander.Interposer.Plugin.Battlefield1942.dll | grep -A4 "Ordinal/Name Pointer" | sed -n '2,3p'
