#!/bin/sh
# Builds, with clang for the MSVC target (lld-link, the static CRT):
#   build/dinput8.dll                                         standalone patch (dinput8.dll proxy)
#   build/bf1942patch.asi                                     the same patches as an ASI plugin (for an ASI loader)
#   build/LANCommander.Interposer.Plugin.Battlefield1942.dll  LANCommander Interposer plugin (same patches)
# The dinput8.dll and the ASI have the LANCommander Interposer (voice chat, FastDL) linked in: its static library,
# LANCommander.Interposer.Static, is MSVC C++, hence the MSVC target. The plugin runs in the Interposer that loads it.
#
# Needs (from MSYS2's bash, or Git Bash):
# - clang and lld-link (LLVM; MSYS2: pacman -S mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-lld)
# - Visual Studio 2022 or later with the C++ x86 tools and a Windows 10/11 SDK (found with vswhere), unless INCLUDE
#   and LIB are set already (a Developer Command Prompt for x86)
# - vcpkg (VCPKG_ROOT): SDL2 (gamepads), from vcpkg.json
# - the Interposer (INTERPOSER_DIR, default ../LANCommander.Interposer) built with its windows-x86 preset:
#   cmake --preset windows-x86 && cmake --build out/windows-x86 --config Release --target LANCommander.Interposer.Static
# BF_VERSION=v1.2.3 sets the version the patch logs (default "dev").
set -e
cd "$(dirname "$0")"
OUT=build
mkdir -p $OUT
PATH="/c/msys64/clang64/bin:/c/Program Files/LLVM/bin:$PATH"

# MSVC and the Windows SDK, x86
if [ -z "$INCLUDE" ] || [ -z "$LIB" ]; then
    VS=$("/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products '*' \
         -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | tr -d '\r')
    MSVC="$VS/VC/Tools/MSVC/$(tr -d '\r\n' < "$VS/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt")"
    SDK="C:/Program Files (x86)/Windows Kits/10"
    SDKV=$(ls "$SDK/Include" | grep '^10\.' | sort -V | tail -1)
    export INCLUDE="$MSVC/include;$SDK/Include/$SDKV/ucrt;$SDK/Include/$SDKV/um;$SDK/Include/$SDKV/shared"
    export LIB="$MSVC/lib/x86;$SDK/Lib/$SDKV/ucrt/x86;$SDK/Lib/$SDKV/um/x86"
fi

# SDL2, linked statically: the patch stays a single DLL
"${VCPKG_ROOT:?set VCPKG_ROOT to vcpkg}/vcpkg" install --triplet x86-windows-static --overlay-triplets=triplets \
    --x-install-root=vcpkg_installed --no-print-usage > /dev/null
SDL=vcpkg_installed/x86-windows-static

# the Interposer: its static library, the libraries it was built with, and YY-Thunks (the MSVC runtime on XP)
INTERPOSER_DIR=${INTERPOSER_DIR:-../LANCommander.Interposer}
INTERPOSER_OUT=$INTERPOSER_DIR/out/windows-x86
INTERPOSER_DEPS=$INTERPOSER_OUT/vcpkg_installed/x86-windows-static/lib
INTERPOSER_LIBS="-wholearchive:$INTERPOSER_OUT/lib/Release/LANCommander.Interposer.Static.lib
    $INTERPOSER_DEPS/yaml-cpp.lib $INTERPOSER_DEPS/enet.lib $INTERPOSER_DEPS/opus.lib $INTERPOSER_DEPS/minhook.x32.lib
    winhttp.lib"
YY_THUNKS=$(ls "$INTERPOSER_DIR"/build/YY-Thunks/*/objs/x86/YY_Thunks_for_WinXP.obj | tail -1)

CFLAGS="--target=i686-pc-windows-msvc -fms-runtime-lib=static -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS
    -D_WINSOCK_DEPRECATED_NO_WARNINGS -I$SDL/include/SDL2 -DBF_VERSION=\"${BF_VERSION:-dev}\""
CORE="src/bf1942patch.c src/settings.c src/settings_game.c src/iat.c src/paths.c src/crashlog.c src/registry.c src/saves.c src/master.c src/d3d.c src/window.c src/restart.c src/widescreen.c src/menu.c src/gamepad.c src/framerate.c src/detail.c src/voice.c src/voice_net.c src/voice_hud.c src/voice_menu.c src/voice_scoreboard.c src/voice_icons.c src/voice_lips.c src/fastdl.c src/voice_server.c src/voice_server_win.c"
# what XP needs of a DLL built with the current MSVC runtime, as the Interposer's own (its build/Compatibility.cmake)
LDFLAGS="-dll -subsystem:windows,5.01 -entry:DllMainCRTStartupForYY_Thunks
    -alternatename:_YY_ThunksOriginalDllMainCRTStartup@12=__DllMainCRTStartup@12 -opt:ref -opt:icf $YY_THUNKS
    -export:bf_restart_helper=_bf_restart_helper@16"
SYSLIBS="$SDL/lib/SDL2-static.lib user32.lib gdi32.lib winmm.lib imm32.lib ole32.lib oleaut32.lib version.lib uuid.lib
    advapi32.lib setupapi.lib shell32.lib ws2_32.lib"

# build <output> <extra clang flags> <sources...> -- <extra lld-link arguments...>
build() {
    out=$1 flags=$2
    shift 2
    obj=$OUT/obj/$(basename "$out")
    rm -rf "$obj"
    mkdir -p "$obj"
    objs=
    while [ "$1" != -- ]; do
        o=$obj/$(basename "$1" .c).obj
        clang $CFLAGS $flags -c "$1" -o "$o"
        objs="$objs $o"
        shift
    done
    shift
    lld-link -nologo -out:"$out" $LDFLAGS $objs "$@" $SYSLIBS
    rm -f "${out%.*}.lib" "${out%.*}.exp"
}

build $OUT/dinput8.dll "-DBF_INTERPOSER_EMBEDDED -DINTERPOSER_STATIC" src/dinput8_proxy.c $CORE -- \
    -def:src/dinput8.def $INTERPOSER_LIBS
build $OUT/bf1942patch.asi "-DBF_INTERPOSER_EMBEDDED -DINTERPOSER_STATIC" src/asi.c $CORE -- $INTERPOSER_LIBS
build $OUT/LANCommander.Interposer.Plugin.Battlefield1942.dll "" src/interposer_plugin.c $CORE -- \
    -export:InterposerPluginInit=_InterposerPluginInit@4
