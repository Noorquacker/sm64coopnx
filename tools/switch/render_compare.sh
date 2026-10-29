#!/usr/bin/env bash
# Captures the same scenes on the PC OpenGL build and the Switch build (in yuzu) and
# writes side-by-side comparisons. The scenes come from tools/switch/render-test.
#
# The PC build runs on a virtual X display (Xvfb), so nothing appears on the desktop,
# with a temporary --savepath so the normal PC config and mods are untouched.
#
# Usage: tools/switch/render_compare.sh ROM OUT_DIR
#   ROM      a vanilla US Super Mario 64 ROM
#   OUT_DIR  receives pc_<scene>.png, switch_<scene>.png and compare_<scene>.png
# Environment: YUZU (default yuzu-cmd), YUZU_SDMC (default ~/.local/share/yuzu/sdmc)
set -euo pipefail

ROM=$1
OUT_DIR=$2
YUZU=${YUZU:-yuzu-cmd}
YUZU_SDMC=${YUZU_SDMC:-$HOME/.local/share/yuzu/sdmc}

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MOD="$REPO_ROOT/tools/switch/render-test/render-test.lua"
PC_EXE="$REPO_ROOT/build/us_pc/sm64coopdx"
NRO="$REPO_ROOT/build/us_nx/sm64coopnx.nro"
ARGS="--server 7777 --skip-intro --skip-update-check --playername test --disable-mods --enable-mod render-test.lua"
SCENES="castle_grounds bob ccm_slide bitdw"

mkdir -p "$OUT_DIR"
OUT_DIR=$(cd "$OUT_DIR" && pwd)
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"; kill $(jobs -p) 2>/dev/null || true' EXIT

# waits for "RENDERTEST <marker>" in a log, up to 3 minutes
wait_for() {
    local log=$1 marker=$2
    for _ in $(seq 1 1800); do
        grep -q "RENDERTEST $marker" "$log" 2>/dev/null && return 0
        sleep 0.1
    done
    echo "timed out waiting for '$marker' in $log" >&2
    return 1
}

# --- PC (OpenGL) ---
echo "PC: capturing on a virtual display"
PC_DIR="$WORK_DIR/pc"
mkdir -p "$PC_DIR/mods"
cp "$ROM" "$PC_DIR/"
cp "$MOD" "$PC_DIR/mods/"
echo "language English" > "$PC_DIR/sm64config.txt" # an empty language opens the first launch language picker
DISPLAY_NUM=97
Xvfb ":$DISPLAY_NUM" -screen 0 1920x1080x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1
# force X11: with WAYLAND_DISPLAY set, SDL would open the window on the real desktop
(cd "$(dirname "$PC_EXE")" && env -u WAYLAND_DISPLAY SDL_VIDEODRIVER=x11 DISPLAY=":$DISPLAY_NUM" stdbuf -oL "$PC_EXE" --savepath "$PC_DIR" --windowed --width 1920 --height 1080 $ARGS > "$PC_DIR/stdout.txt" 2>&1) &
PC_PID=$!
for scene in $SCENES; do
    wait_for "$PC_DIR/stdout.txt" "ready $scene"
    import -display ":$DISPLAY_NUM" -window root "$OUT_DIR/pc_$scene.png"
    echo "  $scene"
done
kill -9 $PC_PID $XVFB_PID 2>/dev/null || true

# --- Switch (deko3d, in yuzu) ---
echo "Switch: capturing in yuzu"
for dir in sm64coopnx sm64coopdx; do
    if [ -d "$YUZU_SDMC/switch/$dir" ]; then SD_DIR="$YUZU_SDMC/switch/$dir"; break; fi
done
SD_DIR=${SD_DIR:-$YUZU_SDMC/switch/sm64coopnx}
mkdir -p "$SD_DIR/mods"
cp "$ROM" "$SD_DIR/"
cp "$MOD" "$SD_DIR/mods/"
rm -f "$SD_DIR"/capture_*.ppm "$SD_DIR/log.txt"
count_captures() { find "$SD_DIR" -maxdepth 1 -name 'capture_*.ppm' | wc -l; }
echo "$ARGS" > "$SD_DIR/args.txt" # yuzu doesn't pass -p arguments to NROs
"$YUZU" "$NRO" >/dev/null 2>&1 &
YUZU_PID=$!
for scene in $SCENES; do
    wait_for "$SD_DIR/log.txt" "ready $scene"
    before=$(count_captures)
    touch "$SD_DIR/capture"
    for _ in $(seq 1 150); do
        [ "$(count_captures)" -gt "$before" ] && break
        sleep 0.1
    done
    sleep 1 # the file is written on a background thread
    latest=$(find "$SD_DIR" -maxdepth 1 -name 'capture_*.ppm' -printf '%T@ %p\n' | sort -n | tail -1 | cut -d' ' -f2-)
    magick "$latest" "$OUT_DIR/switch_$scene.png"
    echo "  $scene"
done
kill -9 $YUZU_PID 2>/dev/null || true
rm -f "$SD_DIR/args.txt"

for scene in $SCENES; do
    magick "$OUT_DIR/pc_$scene.png" "$OUT_DIR/switch_$scene.png" -resize 960x540 +append "$OUT_DIR/compare_$scene.png"
done
echo "done: $OUT_DIR/compare_*.png (left PC, right Switch)"
