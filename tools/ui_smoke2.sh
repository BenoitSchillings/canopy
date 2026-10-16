#!/usr/bin/env bash
# Mouse-driven feature checks: table sort, context menu, ctrl+click path, search, shell 'canopy new', rename.
set -u
OUT="${1:-/tmp/canopy-smoke2}"; mkdir -p "$OUT"
export DISPLAY="${DISPLAY:-:0}" XDG_DATA_HOME="$OUT/data" XDG_CONFIG_HOME="$OUT/config"
mkdir -p "$XDG_DATA_HOME" "$XDG_CONFIG_HOME"
BIN="$(dirname "$0")/../build/canopy"
"$BIN" >"$OUT/canopy.log" 2>&1 & PID=$!
WID=""; for i in $(seq 1 30); do WID=$(xdotool search --pid $PID --name Canopy 2>/dev/null | head -1); [ -n "$WID" ] && break; sleep 0.3; done
[ -z "$WID" ] && { echo "no window"; cat "$OUT/canopy.log"; kill $PID; exit 1; }
xdotool windowactivate --sync "$WID" 2>/dev/null; sleep 1
shot() { sleep "${2:-0.7}"; import -window "$WID" "$OUT/$1.png"; echo "shot $1"; }
type_cmd() { xdotool type --delay 15 -- "$1"; xdotool key Return; }
# geometry (1600x1000, sidebar 240): term origin ~ (434,212), cell ~ (9,19)
TX=434; TY=212; CW=9; CH=19
cell() { echo $((TX + CW * $2 + CW/2)) $((TY + CH * $1 + CH/2)); }
type_cmd "ps aux | head -12"
shot 01-table 1.2
xdotool mousemove $(cell 1 12); sleep 0.3; xdotool click 1      # header row, PID column
shot 02-sorted-pid 0.8
xdotool click 1
shot 03-sorted-desc 0.8
xdotool mousemove $((TX - 12)) $((TY + CH/2)); sleep 0.3; xdotool click 3   # gutter of block row 0
shot 04-context-menu 0.8
xdotool key Escape; sleep 0.3
type_cmd "clear; echo see /etc/hostname and https://example.com/x"
shot 05-spans 1.0
xdotool mousemove $(cell 1 8); sleep 0.5
shot 06-hover-path 0.5
xdotool keydown ctrl; xdotool click 1; xdotool keyup ctrl
shot 07-ctrl-click-opens-editor 2.0
xdotool type ":q"; xdotool key Return; sleep 0.5
xdotool key ctrl+alt+Left; sleep 1.2
xdotool key ctrl+shift+f; sleep 0.3; xdotool type --delay 30 "root"
shot 08-search 0.8
xdotool key Escape; sleep 0.3
type_cmd "canopy new experiments"
shot 09-shell-new 1.5
type_cmd "canopy note 'try the new flag'"
xdotool key ctrl+shift+r; sleep 0.4; xdotool type --delay 30 "Build"; xdotool key Return
shot 10-renamed 1.0
xdotool key F1
shot 11-overview 1.5
kill $PID; sleep 0.5; grep -v "slow flush" "$OUT/canopy.log" | tail -5
