#!/usr/bin/env bash
# Rebuilds lib/coopnet/switch/libcoopnet.a and libjuice.a for Nintendo Switch from the
# same versions coopdx ships for the other platforms (coopdx commit 7be78962c):
#   CoopNet 9d9b3dd (https://github.com/coop-deluxe/coopnet, AGPL-3.0)
#   libjuice v1.6.2 (https://github.com/paullouisageneau/libjuice, MPL-2.0)
# with the small Switch patches in tools/switch/coopnet/:
#   libjuice: no getifaddrs() on Switch (NO_IFADDRS), random bytes from libnx's CSPRNG,
#             no pipe() on Switch (wake the poll thread with a localhost UDP socket, as on Windows)
#   CoopNet:  no <ifaddrs.h>; the device info sent to the server comes from the console's
#             Wi-Fi MAC address, combined the same way as the MAC addresses on Linux/macOS
#
# Usage: tools/switch/build_coopnet.sh   (requires devkitPro with devkitA64 and libnx)
set -euo pipefail

COOPNET_COMMIT=9d9b3dd
JUICE_TAG=v1.6.2
JUICE_COMMIT=85efaa9

DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
BIN="$DEVKITPRO/devkitA64/bin"
CC="$BIN/aarch64-none-elf-gcc"
CXX="$BIN/aarch64-none-elf-g++"
AR="$BIN/aarch64-none-elf-gcc-ar"

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PATCH_DIR="$REPO_ROOT/tools/switch/coopnet"
OUT_DIR="$REPO_ROOT/lib/coopnet/switch"
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

git clone --quiet https://github.com/coop-deluxe/coopnet "$WORK_DIR/coopnet"
git -C "$WORK_DIR/coopnet" checkout --quiet "$COOPNET_COMMIT"
git clone --quiet --depth 1 --branch "$JUICE_TAG" https://github.com/paullouisageneau/libjuice "$WORK_DIR/libjuice"
actual=$(git -C "$WORK_DIR/libjuice" rev-parse --short=7 HEAD)
if [ "$actual" != "$JUICE_COMMIT" ]; then
    echo "libjuice $JUICE_TAG is $actual, expected $JUICE_COMMIT" >&2
    exit 1
fi

patch -d "$WORK_DIR/libjuice" -p1 < "$PATCH_DIR/libjuice-switch.patch"
patch -d "$WORK_DIR/coopnet" -p1 < "$PATCH_DIR/coopnet-switch.patch"

ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections"
DEFS="-D__SWITCH__ -DJUICE_STATIC -isystem $DEVKITPRO/libnx/include"

mkdir -p "$WORK_DIR/obj/juice" "$WORK_DIR/obj/coopnet"
# picohash.h needs the byte order; newlib has no <endian.h> (the Switch is little-endian)
for f in "$WORK_DIR"/libjuice/src/*.c; do
    $CC -O2 -std=gnu11 $ARCH $DEFS -D__LITTLE_ENDIAN__=1 -I"$WORK_DIR/libjuice/include/juice" -c "$f" -o "$WORK_DIR/obj/juice/$(basename "${f%.c}").o"
done
for f in "$WORK_DIR"/coopnet/common/*.cpp; do
    $CXX -O2 -std=gnu++11 $ARCH $DEFS -Wall -Wno-unused-function -I"$WORK_DIR/coopnet/common" -I"$WORK_DIR/libjuice/include" \
        -c "$f" -o "$WORK_DIR/obj/coopnet/$(basename "${f%.cpp}").o"
done

mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR/libjuice.a" "$OUT_DIR/libcoopnet.a"
"$AR" rcs "$OUT_DIR/libjuice.a" "$WORK_DIR"/obj/juice/*.o
"$AR" rcs "$OUT_DIR/libcoopnet.a" "$WORK_DIR"/obj/coopnet/*.o
echo "built $OUT_DIR/libcoopnet.a and $OUT_DIR/libjuice.a"
