#!/usr/bin/env bash
# Many-terminals scenario: rail miniatures + summaries + switcher.
set -u
OUT="${1:-/tmp/canopy-smoke3}"; mkdir -p "$OUT"
export DISPLAY="${DISPLAY:-:0}" XDG_DATA_HOME="$OUT/data" XDG_CONFIG_HOME="$OUT/config"
mkdir -p "$XDG_DATA_HOME" "$XDG_CONFIG_HOME"
BIN="$(dirname "$0")/../build/canopy"
"$BIN" >"$OUT/canopy.log" 2>&1 & PID=$!
WID=""; for i in $(seq 1 30); do WID=$(xdotool search --pid $PID --name Canopy 2>/dev/null | head -1); [ -n "$WID" ] && break; sleep 0.3; done
[ -z "$WID" ] && { echo "no window"; cat "$OUT/canopy.log"; kill $PID; exit 1; }
xdotool windowactivate --sync "$WID" 2>/dev/null; sleep 1
shot() { sleep "${2:-0.7}"; import -window "$WID" "$OUT/$1.png"; echo "shot $1"; }
type_cmd() { xdotool type --delay 15 -- "$1"; xdotool key Return; }
type_cmd "cd /home/benoit/terminal && git log --oneline | head -5"
xdotool key ctrl+shift+r; sleep 0.4; xdotool type --delay 20 "repo"; xdotool key Return; sleep 0.5
xdotool key ctrl+shift+n; sleep 1.2
type_cmd "cd /tmp && mkdir -p brokenproj && cd brokenproj && printf 'int main(){ return x; }\n' > main.c && gcc main.c -o app"
xdotool key ctrl+shift+r; sleep 0.4; xdotool type --delay 20 "build"; xdotool key Return; sleep 0.5
xdotool key ctrl+shift+t; sleep 1.2
type_cmd "cd /etc && vim -u NONE hosts"
xdotool key ctrl+shift+t; sleep 1.2
type_cmd "echo downloading dataset; for i in 1 2 3 4 5 6; do echo chunk \$i of 6; sleep 20; done"
xdotool key ctrl+shift+t; sleep 1.2
type_cmd "cd ~ && ls -la | head -5 && df -h | head -4"
shot 01-five-terminals 1.0
echo "waiting for summaries..."; sleep 45
shot 02-rail-summaries 0.5
xdotool key ctrl+Tab
shot 03-switcher 1.0
xdotool type --delay 40 "vim"
shot 04-switcher-filtered 0.8
xdotool key Return; sleep 1.5
shot 05-jumped-to-vim 0.5
xdotool type ":q!"; xdotool key Return; sleep 0.5
xdotool key ctrl+shift+o
shot 06-overview-banners 1.5
xdotool key Escape; sleep 1
kill $PID; sleep 0.5; grep -v "slow flush" "$OUT/canopy.log" | tail -6
