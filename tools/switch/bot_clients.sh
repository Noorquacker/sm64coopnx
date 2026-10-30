#!/usr/bin/env bash
# Joins COUNT unmodified upstream sm64coopdx PC clients to a host, for many-player tests.
# Each client runs on a virtual X display (Xvfb) at a small resolution, with its own
# temporary --savepath. Enable tools/switch/player-bots on the host so they move around.
# Runs until interrupted (Ctrl+C) or DURATION seconds have passed.
#
# Usage: tools/switch/bot_clients.sh UPSTREAM_PC_EXE ROM HOST_IP COUNT [DURATION] [FIRST_NUMBER]
#   Clients are named Claude, Claude2, Claude3, ... starting at FIRST_NUMBER (default 1).
set -euo pipefail

PC_EXE=$(realpath "$1")
ROM=$(realpath "$2")
HOST_IP=$3
COUNT=$4
DURATION=${5:-0}
FIRST=${6:-1}
DISPLAY_NUM=${BOT_DISPLAY:-93}

WORK_DIR=$(mktemp -d)
PIDS=()
cleanup() {
    for pid in "${PIDS[@]}"; do kill -9 "$pid" 2>/dev/null || true; done
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT INT TERM

Xvfb ":$DISPLAY_NUM" -screen 0 1280x720x24 >/dev/null 2>&1 &
PIDS+=($!)
sleep 1

for i in $(seq "$FIRST" $((FIRST + COUNT - 1))); do
    name=Claude
    [ "$i" -gt 1 ] && name="Claude$i"
    dir="$WORK_DIR/$name"
    mkdir -p "$dir"
    cp "$ROM" "$dir/"
    printf 'language English\ndebug_info true\n' > "$dir/sm64config.txt"
    # force X11: with WAYLAND_DISPLAY set, SDL would open the window on the real desktop
    (cd "$(dirname "$PC_EXE")" && exec env -u WAYLAND_DISPLAY SDL_VIDEODRIVER=x11 DISPLAY=":$DISPLAY_NUM" \
        stdbuf -oL "$PC_EXE" --savepath "$dir" --windowed --width 320 --height 240 --no-discord --skip-intro \
        --skip-update-check --playername "$name" --client "$HOST_IP" 7777 > "$dir/stdout.txt" 2>&1) &
    PIDS+=($!)
    echo "started $name"
    sleep 3 # let each one join before the next
done

joined() { grep -l "received join packet" "$WORK_DIR"/*/stdout.txt 2>/dev/null | wc -l; }
start=$(date +%s)
while :; do
    sleep 10
    echo "$(date +%T) clients joined: $(joined)/$COUNT, running: $(( $(jobs -rp | wc -l) - 1 ))"
    [ "$DURATION" -gt 0 ] && [ $(( $(date +%s) - start )) -ge "$DURATION" ] && break
done
