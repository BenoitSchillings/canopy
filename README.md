# Canopy

A GPU-rendered terminal for Linux built around three ideas:

* **Activity tree** — every shell session is a node in a tree. Children inherit the
  parent's directory and environment. The tree persists across restarts with each
  activity's command history, so you can resume where you left off.
* **Commands as blocks** — each prompt/command/output is an object with its exit
  status and duration. Fold it, pin it, re-run it, copy its output, diff it against
  the previous run, or fork it into a child activity.
* **A zoomable canvas** — activities are panes on an infinite canvas laid out as a
  tree. Zoom out (F1) to see every live session as a miniature; click one to fly to it.

Output is also *understood*: paths and URLs are clickable, error lines get gutter
markers and a jump key, JSON can be pretty-printed in place, and column-aligned
tables get sortable headers.

It is a complete VT terminal underneath (xterm-256color, truecolor, mouse,
bracketed paste, alternate screen), so vim, htop, tmux, less and friends work as usual.

## Build / install

```
./install.sh            # builds and installs to ~/.local/bin/canopy
./build/canopy          # or run from the build directory
```

Dependencies are already on this machine (SDL2, FreeType, HarfBuzz, fontconfig,
OpenGL 3.3). Development headers are vendored in `third_party/sysroot`.

## Configuration

`~/.config/canopy/canopy.conf` — see `docs/canopy.conf.example` for every key.

## Keys

Press **F10** inside Canopy for the full list. The essentials:

| Key | Action |
|-----|--------|
| Ctrl+Shift+N / T | new child / sibling activity |
| Ctrl+Shift+W | close activity |
| F1 | overview of all panes |
| F2 | sidebar |
| Ctrl+Shift+P | command palette (actions, jump to activity, past commands) |
| Ctrl+Shift+Up/Down | previous / next block |
| Ctrl+Shift+Left/Right | fold / unfold block |
| Ctrl+Shift+Enter | re-run selected block |
| Ctrl+Shift+E | next error line |
| Ctrl+Shift+F | search scrollback |
| Ctrl+Shift+C / V | copy / paste |
| Ctrl+wheel, Ctrl+= / − / 0 | zoom canvas |
| Ctrl+Alt+Arrows | move focus between panes |

From the shell: `canopy new <name>`, `canopy note <text>`, `canopy name <name>`, `canopy mark`.
