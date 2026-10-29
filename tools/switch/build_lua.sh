#!/usr/bin/env bash
# Rebuilds lib/lua/switch/liblua53.a for aarch64 Nintendo Switch (libnx) from
# the official Lua 5.3.5 release, using the repo's vendored luaconf.h so the
# result matches the prebuilt libs used on the other platforms.
#
# Usage: tools/switch/build_lua.sh   (requires devkitPro with devkitA64)
set -euo pipefail

LUA_VERSION=5.3.5
LUA_SHA256=0c2eed3f960446e1a3e4b9a1ca2f3ff893b6ce41942cf54d5dd59ab4b3b058ac

DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
CC="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc"
AR="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc-ar"

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT_DIR="$REPO_ROOT/lib/lua/switch"
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

curl -sSL -o "$WORK_DIR/lua.tar.gz" "https://www.lua.org/ftp/lua-$LUA_VERSION.tar.gz"
echo "$LUA_SHA256  $WORK_DIR/lua.tar.gz" | sha256sum -c -
tar xzf "$WORK_DIR/lua.tar.gz" -C "$WORK_DIR"

SRC="$WORK_DIR/lua-$LUA_VERSION/src"
cp "$REPO_ROOT/lib/lua/include/luaconf.h" "$SRC/luaconf.h"

# same flags as libnx applications, LUA_COMPAT_5_2 as in the stock Lua makefile
CFLAGS="-O2 -Wall -ffunction-sections -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
        -D__SWITCH__ -DLUA_COMPAT_5_2 -I$DEVKITPRO/libnx/include"

OBJS=()
for f in "$SRC"/*.c; do
    case "$(basename "$f")" in lua.c|luac.c) continue ;; esac
    obj="$WORK_DIR/$(basename "${f%.c}").o"
    $CC $CFLAGS -c "$f" -o "$obj"
    OBJS+=("$obj")
done

mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR/liblua53.a"
$AR rcs "$OUT_DIR/liblua53.a" "${OBJS[@]}"
echo "built $OUT_DIR/liblua53.a"
