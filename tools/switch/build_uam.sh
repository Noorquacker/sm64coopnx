#!/usr/bin/env bash
# Rebuilds lib/uam/switch/libuam.a: uam, the deko3d GLSL compiler, as a static library
# for Nintendo Switch, so the game can compile color combiner shaders at runtime.
#
# Uses uam's own meson build with a devkitA64 cross file, adds a small C interface
# (tools/switch/uam/uam_c_api.cpp, lib/uam/include/uam.h) and prelinks every object
# except the command line tool's main.cpp into one object that only exports that interface.
#
# Usage: tools/switch/build_uam.sh
# Requires devkitPro (devkitA64, libnx), meson, ninja, bison, flex and python3 with mako.
set -euo pipefail

UAM_TAG=v1.1.0
UAM_COMMIT=5a5afc2

DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
BIN="$DEVKITPRO/devkitA64/bin"

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT_DIR="$REPO_ROOT/lib/uam/switch"
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

git clone --quiet --depth 1 --branch "$UAM_TAG" https://github.com/devkitPro/uam "$WORK_DIR/uam"
actual=$(git -C "$WORK_DIR/uam" rev-parse --short=7 HEAD)
if [ "$actual" != "$UAM_COMMIT" ]; then
    echo "uam $UAM_TAG is $actual, expected $UAM_COMMIT" >&2
    exit 1
fi

cp "$REPO_ROOT/tools/switch/uam/uam_c_api.cpp" "$REPO_ROOT/lib/uam/include/uam.h" "$WORK_DIR/uam/source/"
echo "uam_files += files('uam_c_api.cpp')" >> "$WORK_DIR/uam/source/meson.build"

ARCH="'-march=armv8-a+crc+crypto', '-mtune=cortex-a57', '-mtp=soft', '-fPIE'"
cat > "$WORK_DIR/cross.txt" <<CROSS
[binaries]
c = '$BIN/aarch64-none-elf-gcc'
cpp = '$BIN/aarch64-none-elf-g++'
ar = '$BIN/aarch64-none-elf-gcc-ar'
strip = '$BIN/aarch64-none-elf-strip'

[host_machine]
system = 'horizon'
cpu_family = 'aarch64'
cpu = 'cortex-a57'
endian = 'little'

[built-in options]
c_args = [$ARCH, '-ffunction-sections', '-D__SWITCH__', '-isystem', '$DEVKITPRO/libnx/include']
cpp_args = [$ARCH, '-ffunction-sections', '-D__SWITCH__', '-isystem', '$DEVKITPRO/libnx/include']
c_link_args = [$ARCH, '-specs=$DEVKITPRO/libnx/switch.specs', '-L$DEVKITPRO/libnx/lib', '-lnx']
cpp_link_args = [$ARCH, '-specs=$DEVKITPRO/libnx/switch.specs', '-L$DEVKITPRO/libnx/lib', '-lnx']
CROSS

meson setup "$WORK_DIR/build" "$WORK_DIR/uam" --cross-file "$WORK_DIR/cross.txt" --buildtype=release
# only the objects are needed; the uam executable itself is not
ninja -C "$WORK_DIR/build" $(ninja -C "$WORK_DIR/build" -t targets all | grep -o '^[^:]*\.o' | grep -v 'main\.cpp\.o$')

# Prelink everything into one object whose only visible symbol is the C interface.
# uam contains its own copy of Mesa's GLSL compiler, which would otherwise clash with the
# Mesa that the static SDL2 pulls in (glsl_type, linker_error, ...). Hiding the symbols is
# not enough: C++ vtables and inline functions live in COMDAT groups named after their
# symbol, and a group from uam would replace libEGL's. So every symbol uam defines is
# renamed with a __uam_ prefix (which renames the groups too), then made local.
find "$WORK_DIR/build" -name '*.o' ! -name 'main.cpp.o' -print0 | xargs -0 "$BIN/aarch64-none-elf-ld" -r -o "$WORK_DIR/uam_all.o"
"$BIN/aarch64-none-elf-nm" --defined-only "$WORK_DIR/uam_all.o" | awk 'NF == 3 && $3 != "uam_compile_dksh" { print $3 " __uam_" $3 }' | sort -u > "$WORK_DIR/rename.txt"
"$BIN/aarch64-none-elf-objcopy" --redefine-syms="$WORK_DIR/rename.txt" "$WORK_DIR/uam_all.o" "$WORK_DIR/uam_renamed.o"
"$BIN/aarch64-none-elf-objcopy" --keep-global-symbol=uam_compile_dksh "$WORK_DIR/uam_renamed.o" "$WORK_DIR/uam.o"

mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR/libuam.a"
"$BIN/aarch64-none-elf-gcc-ar" rcs "$OUT_DIR/libuam.a" "$WORK_DIR/uam.o"
echo "built $OUT_DIR/libuam.a"
