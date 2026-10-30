#!/usr/bin/env bash
# Finds the color combiners the game uses and adds them to res/switch/shader_combiners.txt,
# the list the build prebuilds into romfs:/shadercache.
#
# Runs the Switch build in yuzu with tools/switch/shader-gather, which warps through every
# level area with each cap power. The build records every combiner it has to compile at
# runtime (one that isn't in the prebuilt list yet) in <user dir>/shadercache/combiners.txt.
# The user shader cache is moved aside for the run and restored afterwards.
#
# Usage: tools/switch/gather_combiners.sh ROM [EXTRA_COMBINERS_TXT...]
#   ROM                  a vanilla US Super Mario 64 ROM
#   EXTRA_COMBINERS_TXT  combiners.txt files from other runs (e.g. from hardware) to merge in
# Environment: YUZU (default yuzu-cmd), YUZU_SDMC (default ~/.local/share/yuzu/sdmc)
set -euo pipefail

ROM=$1
shift
YUZU=${YUZU:-yuzu-cmd}
YUZU_SDMC=${YUZU_SDMC:-$HOME/.local/share/yuzu/sdmc}

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MOD="$REPO_ROOT/tools/switch/shader-gather/shader-gather.lua"
NRO="$REPO_ROOT/build/us_nx/sm64coopnx.nro"
LIST="$REPO_ROOT/res/switch/shader_combiners.txt"
ARGS="--server 7777 --skip-intro --skip-update-check --playername test --disable-mods --enable-mod shader-gather.lua"

for dir in sm64coopnx sm64coopdx; do
    if [ -d "$YUZU_SDMC/switch/$dir" ]; then SD_DIR="$YUZU_SDMC/switch/$dir"; break; fi
done
SD_DIR=${SD_DIR:-$YUZU_SDMC/switch/sm64coopnx}
CACHE="$SD_DIR/shadercache"
BACKUP="$SD_DIR/shadercache.gather-backup"
if [ -e "$BACKUP" ]; then
    echo "$BACKUP exists (an earlier run was interrupted): restore it to $CACHE first" >&2
    exit 1
fi

mkdir -p "$SD_DIR/mods"
cp "$ROM" "$SD_DIR/"
cp "$MOD" "$SD_DIR/mods/"
[ -d "$CACHE" ] && mv "$CACHE" "$BACKUP"
restore() {
    kill -9 "${YUZU_PID:-}" 2>/dev/null || true
    rm -f "$SD_DIR/args.txt" "$SD_DIR/mods/shader-gather.lua"
    rm -rf "$CACHE"
    [ -d "$BACKUP" ] && mv "$BACKUP" "$CACHE"
}
trap restore EXIT

rm -f "$SD_DIR/log.txt"
echo "$ARGS" > "$SD_DIR/args.txt" # yuzu doesn't pass -p arguments to NROs
"$YUZU" "$NRO" >/dev/null 2>&1 &
YUZU_PID=$!

echo "gathering in yuzu (about 15 minutes)"
for _ in $(seq 1 3600); do
    grep -q "SHADERGATHER done" "$SD_DIR/log.txt" 2>/dev/null && break
    if ! kill -0 $YUZU_PID 2>/dev/null; then
        echo "yuzu exited early, see $SD_DIR/log.txt" >&2
        exit 1
    fi
    sleep 1
done
grep -q "SHADERGATHER done" "$SD_DIR/log.txt" || { echo "timed out, see $SD_DIR/log.txt" >&2; exit 1; }
grep -c "SHADERGATHER scene" "$SD_DIR/log.txt" | sed 's/^/scenes visited: /'

NEW=$(mktemp)
cat "$CACHE/combiners.txt" "$@" 2>/dev/null | grep -v '^#' | grep -v '^\s*$' > "$NEW" || true
before=$(grep -cv -e '^#' -e '^\s*$' "$LIST" || true)
{
    grep -e '^#' "$LIST"
    { grep -v -e '^#' -e '^\s*$' "$LIST" || true; cat "$NEW"; } | sort -u
} > "$LIST.tmp"
mv "$LIST.tmp" "$LIST"
rm -f "$NEW"
after=$(grep -cv -e '^#' -e '^\s*$' "$LIST")
echo "$LIST: $before -> $after combiners"
