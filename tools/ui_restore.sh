#!/usr/bin/env bash
# Relaunch Canopy against an existing smoke data dir to verify restore; screenshot and quit.
set -u
OUT="${1:?outdir}"; export DISPLAY="${DISPLAY:-:0}"
export XDG_DATA_HOME="$OUT/data" XDG_CONFIG_HOME="$OUT/config"
BIN="$(dirname "$0")/../build/canopy"
"$BIN" >"$OUT/canopy-restore.log" 2>&1 & PID=$!
WID=""; for i in $(seq 1 30); do WID=$(xdotool search --pid $PID --name Canopy 2>/dev/null | head -1); [ -n "$WID" ] && break; sleep 0.3; done
[ -z "$WID" ] && { echo "no window"; cat "$OUT/canopy-restore.log"; kill $PID; exit 1; }
xdotool windowactivate --sync "$WID"; sleep 1.5
import -window "$WID" "$OUT/r1-restored.png"
xdotool key F1; sleep 1.5; import -window "$WID" "$OUT/r2-overview.png"
xdotool key Escape; sleep 1
xdotool key ctrl+alt+Left; sleep 1.5; import -window "$WID" "$OUT/r3-focus-left.png"
xdotool type --delay 20 "echo resumed"; xdotool key Return; sleep 1; import -window "$WID" "$OUT/r4-resumed.png"
kill $PID; sleep 0.5; tail -5 "$OUT/canopy-restore.log"
