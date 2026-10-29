#!/usr/bin/env bash
# Direct connection test between the Switch build (in yuzu) and an UNMODIFIED upstream
# sm64coopdx PC build, in both directions:
#   switch-host: the Switch hosts, the PC joins
#   pc-host:     the PC hosts, the Switch joins
#
# Everything runs on virtual X displays (Xvfb). The PC uses a temporary --savepath.
# Success = the joining side logs "received join packet" (coopdx's debug_info logging).
# Screenshots of both sides are written to OUT_DIR.
#
# Usage: tools/switch/net_test.sh UPSTREAM_PC_EXE ROM OUT_DIR [switch-host|pc-host|both] [MOD_DIR]
#   UPSTREAM_PC_EXE  sm64coopdx built from unmodified upstream
#   MOD_DIR          optional mod folder enabled on the host (tests mod downloads)
# Environment: YUZU (default yuzu-cmd), YUZU_SDMC (default ~/.local/share/yuzu/sdmc)
set -euo pipefail

PC_EXE=$(realpath "$1")
ROM=$(realpath "$2")
OUT_DIR=$3
SCENARIOS=${4:-both}
MOD_DIR=${5:-}
YUZU=${YUZU:-yuzu-cmd}
YUZU_SDMC=${YUZU_SDMC:-$HOME/.local/share/yuzu/sdmc}

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NRO="$REPO_ROOT/build/us_nx/sm64coopnx.nro"
COMMON="--skip-intro --skip-update-check"
PC_DISPLAY=:97
SWITCH_DISPLAY=:98

mkdir -p "$OUT_DIR"
OUT_DIR=$(cd "$OUT_DIR" && pwd)
WORK_DIR=$(mktemp -d)
PIDS=()
cleanup() {
    for pid in "${PIDS[@]}"; do kill -9 "$pid" 2>/dev/null || true; done
    [ -n "${SD_DIR:-}" ] && rm -f "$SD_DIR/args.txt" "$SD_DIR/capture" "$SD_DIR/quit"
    [ -n "${SD_DIR:-}" ] && [ -f "$WORK_DIR/sm64config.txt.bak" ] && cp "$WORK_DIR/sm64config.txt.bak" "$SD_DIR/sm64config.txt"
    [ -n "${SWITCH_MOD:-}" ] && rm -rf "$SWITCH_MOD"
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

Xvfb $PC_DISPLAY -screen 0 1280x720x24 >/dev/null 2>&1 & PIDS+=($!)
Xvfb $SWITCH_DISPLAY -screen 0 1920x1080x24 >/dev/null 2>&1 & PIDS+=($!)
sleep 1

# waits up to $3 seconds for a pattern in a file
wait_for() {
    local file=$1 pattern=$2 timeout=$3
    for _ in $(seq 1 $((timeout * 10))); do
        grep -q "$pattern" "$file" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

# --- Switch side (yuzu) ---
for dir in sm64coopnx sm64coopdx; do
    if [ -d "$YUZU_SDMC/switch/$dir" ]; then SD_DIR="$YUZU_SDMC/switch/$dir"; break; fi
done
SD_DIR=${SD_DIR:-$YUZU_SDMC/switch/sm64coopnx}
mkdir -p "$SD_DIR/mods"
cp "$ROM" "$SD_DIR/"
if [ -f "$SD_DIR/sm64config.txt" ]; then
    cp "$SD_DIR/sm64config.txt" "$WORK_DIR/sm64config.txt.bak"
    sed -i 's/^debug_info .*/debug_info true/' "$SD_DIR/sm64config.txt"
else
    printf "language English\ndebug_info true\n" > "$SD_DIR/sm64config.txt"
fi

start_switch() {
    rm -f "$SD_DIR/log.txt"
    echo "$1" > "$SD_DIR/args.txt"
    env -u WAYLAND_DISPLAY SDL_VIDEODRIVER=x11 DISPLAY=$SWITCH_DISPLAY "$YUZU" "$NRO" >/dev/null 2>&1 & PIDS+=($!)
    SWITCH_PID=$!
}

capture_switch() {
    local out=$1 before
    before=$(find "$SD_DIR" -maxdepth 1 -name 'capture_*.ppm' | wc -l)
    touch "$SD_DIR/capture"
    for _ in $(seq 1 300); do
        [ "$(find "$SD_DIR" -maxdepth 1 -name 'capture_*.ppm' | wc -l)" -gt "$before" ] && break
        sleep 0.1
    done
    sleep 1
    latest=$(find "$SD_DIR" -maxdepth 1 -name 'capture_*.ppm' -printf '%T@ %p\n' | sort -n | tail -1 | cut -d' ' -f2-)
    [ -n "$latest" ] && magick "$latest" -resize 1280x720 "$out" && rm -f "$SD_DIR"/capture_*.ppm
}

# --- PC side (upstream, unmodified) ---
# start_pc ARGS [with-mod]
start_pc() {
    PC_DIR="$WORK_DIR/pc"
    rm -rf "$PC_DIR"
    mkdir -p "$PC_DIR/mods"
    cp "$ROM" "$PC_DIR/"
    [ "${2:-}" = with-mod ] && cp -r "$PC_MOD" "$PC_DIR/mods/"
    printf "language English\ndebug_info true\n" > "$PC_DIR/sm64config.txt"
    (cd "$(dirname "$PC_EXE")" && env -u WAYLAND_DISPLAY SDL_VIDEODRIVER=x11 DISPLAY=$PC_DISPLAY \
        stdbuf -oL "$PC_EXE" --savepath "$PC_DIR" --windowed --width 1280 --height 720 $1 > "$PC_DIR/stdout.txt" 2>&1) & PIDS+=($!)
    PC_PID=$!
}

stop_both() {
    kill -9 "$PC_PID" "$SWITCH_PID" 2>/dev/null || true
    pkill -9 -x sm64coopdx 2>/dev/null || true
    sleep 1
}

# Copies MOD_DIR to a uniquely named mod whose files all differ from any earlier copy,
# so the joining side has to download every file instead of finding it in its mod cache.
# (Bytes appended after an .ogg's last page are ignored by decoders.)
unique_mod() {
    local token dest
    token=$(head -c 8 /dev/urandom | od -An -tx1 | tr -d ' \n')
    dest="$WORK_DIR/mod-$token/$(basename "$MOD_DIR")-$token"
    mkdir -p "$(dirname "$dest")"
    cp -r "$MOD_DIR" "$dest"
    find "$dest" -name '*.lua' -exec sh -c 'printf "\n-- %s\n" "$1" >> "$2"' _ "$token" {} \;
    find "$dest" -name '*.ogg' -exec sh -c 'printf "%s" "$1" >> "$2"' _ "$token" {} \;
    echo "$dest"
}

report() {
    local name=$1 ok=$2
    if [ "$ok" = 1 ]; then echo "PASS $name"; else echo "FAIL $name"; fi
    magick "$OUT_DIR/${name}_pc.png" "$OUT_DIR/${name}_switch.png" +append -resize 1920x "$OUT_DIR/${name}.png" 2>/dev/null || true
}

if [ "$SCENARIOS" = switch-host ] || [ "$SCENARIOS" = both ]; then
    echo "switch-host: Switch hosts on 7777, upstream PC joins"
    HOST_MODS="--disable-mods"
    if [ -n "$MOD_DIR" ]; then
        mod=$(unique_mod)
        cp -r "$mod" "$SD_DIR/mods/"
        SWITCH_MOD="$SD_DIR/mods/$(basename "$mod")"
        HOST_MODS="--disable-mods --enable-mod $(basename "$mod")"
    fi
    start_switch "--server 7777 $COMMON --playername switch $HOST_MODS"
    ok=0
    if wait_for "$SD_DIR/log.txt" "game ticks" 120; then
        sleep 20 # let the host finish starting (yuzu on Xvfb runs at about half speed)
        start_pc "--client 127.0.0.1 7777 $COMMON --playername pc"
        if wait_for "$PC_DIR/stdout.txt" "received join packet" 120; then ok=1; fi
    fi
    sleep 15
    import -display $PC_DISPLAY -window root "$OUT_DIR/switch-host_pc.png" 2>/dev/null || true
    capture_switch "$OUT_DIR/switch-host_switch.png" || true
    cp "$SD_DIR/log.txt" "$OUT_DIR/switch-host_switch_log.txt" 2>/dev/null || true
    cp "$PC_DIR/stdout.txt" "$OUT_DIR/switch-host_pc_log.txt" 2>/dev/null || true
    stop_both
    [ -n "${SWITCH_MOD:-}" ] && rm -rf "$SWITCH_MOD"
    report switch-host $ok
fi

if [ "$SCENARIOS" = pc-host ] || [ "$SCENARIOS" = both ]; then
    echo "pc-host: upstream PC hosts on 7778, Switch joins"
    if [ -n "$MOD_DIR" ]; then
        PC_MOD=$(unique_mod)
        start_pc "--server 7778 $COMMON --playername pc --disable-mods --enable-mod $(basename "$PC_MOD")" with-mod
    else
        start_pc "--server 7778 $COMMON --playername pc --disable-mods"
    fi
    ok=0
    sleep 15
    start_switch "--client 127.0.0.1 7778 $COMMON --playername switch"
    if wait_for "$SD_DIR/log.txt" "received join packet" 180; then ok=1; fi
    sleep 25
    import -display $PC_DISPLAY -window root "$OUT_DIR/pc-host_pc.png" 2>/dev/null || true
    capture_switch "$OUT_DIR/pc-host_switch.png" || true
    cp "$SD_DIR/log.txt" "$OUT_DIR/pc-host_switch_log.txt" 2>/dev/null || true
    cp "$PC_DIR/stdout.txt" "$OUT_DIR/pc-host_pc_log.txt" 2>/dev/null || true
    stop_both
    report pc-host $ok
fi
