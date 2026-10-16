#!/usr/bin/env bash
# Launch Canopy on DISPLAY with an isolated data/config dir, drive it with xdotool, screenshot.
# usage: tools/ui_smoke.sh <outdir> [script-name]
set -u
OUT="${1:-/tmp/canopy-smoke}"; mkdir -p "$OUT"
export DISPLAY="${DISPLAY:-:0}"
export XDG_DATA_HOME="$OUT/data" XDG_CONFIG_HOME="$OUT/config"
mkdir -p "$XDG_DATA_HOME" "$XDG_CONFIG_HOME"
BIN="$(dirname "$0")/../build/canopy"
"$BIN" >"$OUT/canopy.log" 2>&1 &
PID=$!
sleep 1.5
WID=""
for i in $(seq 1 20); do WID=$(xdotool search --pid $PID --name Canopy 2>/dev/null | head -1); [ -n "$WID" ] && break; sleep 0.3; done
if [ -z "$WID" ]; then echo "no window; log:"; cat "$OUT/canopy.log"; kill $PID 2>/dev/null; exit 1; fi
xdotool windowactivate --sync "$WID" 2>/dev/null; sleep 0.3
shot() { sleep "${2:-0.6}"; import -window "$WID" "$OUT/$1.png" 2>/dev/null || import -window root "$OUT/$1.png"; echo "shot $1"; }
type_cmd() { xdotool type --delay 20 -- "$1"; xdotool key Return; }

shot 01-start 1.5
type_cmd "echo hello from canopy; ls -la /etc | head -20"
shot 02-ls 1.5
type_cmd "printf '{\"name\":\"canopy\",\"features\":[\"blocks\",\"tree\",\"canvas\"],\"n\":3}\n'"
shot 03-json 1.0
type_cmd "gcc -c /nonexistent.c; false"
shot 04-errors 1.0
type_cmd "ps aux | head -12"
shot 05-table 1.2
xdotool key ctrl+shift+n; sleep 1.2
type_cmd "cd /tmp && pwd && seq 1 30"
shot 06-child 1.5
xdotool key ctrl+shift+n; sleep 1.0
type_cmd "top -b -n1 | head -8"
shot 07-grandchild 1.5
xdotool key F1
shot 08-overview 1.5
xdotool key Escape; sleep 1.0
xdotool key ctrl+shift+p; sleep 0.5
xdotool type --delay 30 "seq"
shot 09-palette 0.8
xdotool key Escape; sleep 0.3
xdotool key ctrl+shift+Up; sleep 0.3; xdotool key ctrl+shift+Left
shot 10-folded 0.8
xdotool key F10
shot 11-help 0.8
xdotool key Escape
type_cmd "vim -u NONE /etc/hostname"
shot 12-vim 1.5
xdotool type ":q!"; xdotool key Return; sleep 0.5
kill $PID 2>/dev/null; sleep 0.5
echo "--- log ---"; tail -20 "$OUT/canopy.log"
echo "--- tree.json ---"; python3 -c "import json,sys; t=json.load(open('$XDG_DATA_HOME/canopy/tree.json')); print(json.dumps({k:v for k,v in t.items() if k!='activities'}), len(t.get('activities',t.get('nodes',[]))), 'activities')" 2>/dev/null || ls -la "$XDG_DATA_HOME/canopy/" 2>/dev/null
