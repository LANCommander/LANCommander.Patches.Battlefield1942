#!/bin/sh
# Starts the Battlefield 1942 Linux dedicated server with the patch's voice server. Arguments go to the server:
#
#   ./start-with-bf1942patch.sh +statusMonitor 1
#
# Put this script, libbf1942patch.so and libLANCommander.Interposer.so (x86) in the server directory, next to
# bf1942_lnxded. The patch is preloaded into the server only, so no other program (64-bit ones in particular) sees it.
dir=$(cd "$(dirname "$0")" && pwd)
cd "$dir" || exit 1

server=./bf1942_lnxded
[ -e "$server" ] || server=./bf1942_lnxded.static

LD_PRELOAD="$dir/libbf1942patch.so${LD_PRELOAD:+:$LD_PRELOAD}" exec "$server" "$@"
