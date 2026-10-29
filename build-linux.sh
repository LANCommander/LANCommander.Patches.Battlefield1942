#!/bin/sh
# Builds, for the Linux dedicated server (bf1942_lnxded 1.61):
#   build/linux/libbf1942patch.so            the voice server, preloaded into the server
#   build/linux/start-with-bf1942patch.sh    the script that starts the server with it
# Needs a 32-bit gcc (gcc-multilib) on Linux; on Debian 12, as the LANCommander server images, the result runs on their
# glibc. From Windows: docker run --rm -v "$PWD:/src" -w /src debian:12 sh -c \
#   'apt-get update -qq && apt-get install -y -qq gcc-multilib libc6-dev-i386 > /dev/null && sh build-linux.sh'
# BF_VERSION=v1.2.3 sets the version the patch logs (default "dev").
set -e
cd "$(dirname "$0")"
OUT=build/linux
mkdir -p $OUT
gcc -m32 -O2 -Wall -Wextra -shared -fPIC -fvisibility=hidden -DBF_VERSION="\"${BF_VERSION:-dev}\"" \
    -o $OUT/libbf1942patch.so src/linux/server_linux.c src/voice_server.c src/settings.c \
    -Wl,--version-script=src/linux/exports.map -Wl,-z,defs -ldl -lpthread
cp src/linux/start-with-bf1942patch.sh $OUT/
chmod +x $OUT/start-with-bf1942patch.sh
