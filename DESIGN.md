# Canopy — design document

Canopy is a GPU-rendered terminal emulator for Linux (X11/Wayland via SDL2) whose
central idea is that **terminal work has structure**: commands are objects,
sessions belong to a tree of *activities*, and the whole workspace lives on a
zoomable canvas. It must remain a fully compatible VT terminal: vim, htop, tmux,
less, ssh, mc, and anything curses-based must behave exactly as in xterm/kitty.

## 1. Concepts

### Activity tree
An *activity* is a node in a tree. It has a name, notes, a working directory,
a (possibly live) shell session, and an ordered list of *blocks*. Children
inherit cwd and environment from their parent at creation time. Activities are
created from the UI (Ctrl+Shift+N child, Ctrl+Shift+T sibling), from the shell
(`canopy new "fix tests"`), or by *forking* a block ("open this command in a
child activity"). The tree is persisted under `~/.local/share/canopy/` so that
closing Canopy and reopening it shows the same tree, with history, and a fresh
shell starts in an activity when you focus it ("resume").

### Blocks
A *block* is one prompt + command + output + exit status. Block boundaries come
from shell integration (OSC 133 A/B/C/D plus a Canopy-private OSC 7770 that
carries the command text). Blocks can be folded, pinned, re-run, copied,
forked into a child activity, searched, and their outputs compared.

### Structured output
Block output is scanned for paths, URLs, error lines, JSON documents and
column-aligned tables. These get affordances: clickable paths/URLs, error
markers in the gutter, JSON fold/pretty toggle, table column highlighting and
click-to-sort on a header.

### Spatial canvas
Every activity is a *pane* on an infinite 2D canvas. Panes are auto-laid out
as a tree (children to the right of parents) but can be dragged. Zoom with
Ctrl+wheel or toggle *overview* (F1) which smoothly zooms out to show all
panes as live miniatures; click one to fly to it. At zoom 1.0 text is
pixel-exact. Sidebar (F2) shows the tree as an outliner.

## 2. Architecture

```
src/core/   — no SDL/GL. Pure C++20. Unit-tested headlessly.
  pty.h         forkpty wrapper, non-blocking fd, resize
  cell.h        Cell / Color / Attr definitions (shared with gfx+ui)
  terminal.h    VT emulator: parser + screen grid + scrollback + modes
  keys.h        key/mouse -> escape-sequence encoding (depends on Terminal modes)
  session.h     Pty + Terminal + reader thread glue + block tracker wiring
  blocks.h      BlockTracker: turns OSC 133/7770 events into Block objects
  structure.h   structured-output scanner (paths, urls, errors, json, tables)
  activity.h    Activity tree model + JSON persistence
  json.h        minimal JSON value/parse/serialize
  config.h      ~/.config/canopy/canopy.conf loader (key = value)
src/gfx/    — SDL2 + OpenGL 3.3 core + FreeType (+ fontconfig for lookup)
  gl.h          GL function loading via SDL_GL_GetProcAddress
  font.h        glyph atlas, fallback fonts, cell metrics
  renderer.h    single-shader batched 2D renderer with camera transform + clipping
  window.h      SDL window/context/event translation into gfx::Event
src/ui/     — everything the user sees; depends on core + gfx
  app           main loop, wiring, global keybindings
  canvas        camera, pane layout, animation, overview mode
  pane          draws one Session: grid, cursor, selection, block gutter, spans
  sidebar       activity outliner
  overlay       command palette, context menu, search bar, toasts
shell/canopy.bash   bash integration (sourced via --rcfile wrapper)
tools/vtdump.cpp    headless: run a command under Session, dump screen as text
tests/              unit tests (ctest)
```

Dependencies are one-way: ui -> gfx -> (nothing of core) ; ui -> core ; core -> libc/pthread only.

## 3. Conventions
* C++20, `namespace canopy`, `-Wall -Wextra`, no RTTI tricks, no exceptions
  for control flow (parsing returns bool/optional).
* Headers in `src/core/*.h` etc. are the **contract between modules**. Add to
  them if you must, but do not change existing signatures without saying so
  in your report.
* `Terminal` and all core objects are single-threaded: only the main thread
  touches them. The PTY reader thread only moves bytes into a queue and wakes
  the main loop.
* Absolute line numbers: history and primary screen share one monotonically
  increasing `int64_t` index space. `history_base()` is the oldest retained
  line; `screen_base()` is the absolute index of primary-screen row 0. Lines
  keep their index when they scroll into history or are dropped from history.
  No reflow on resize in v1 (truncate/pad), so indices are stable.
* Colors in cells are symbolic (`Color::Default`, indexed 0-255, or RGB);
  resolution against the theme palette happens in the renderer.
* Time: `double` seconds from `std::chrono::steady_clock` (monotonic) for
  durations; `int64_t` unix seconds for persistence.

## 4. Build
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/canopy                 # the app
./build/vtdump -c 80 -r 24 -- vim -u NONE   # headless screen dump
ctest --test-dir build
```
Dev headers for SDL2/FreeType/HarfBuzz/fontconfig live in `third_party/sysroot`
(extracted .debs); the runtime .so files are the system ones. CMake links them
by absolute path.

## 5. Shell integration protocol
Sequences are `ESC ] ... BEL` (or ST).
* `OSC 133;A`  prompt start          * `OSC 133;B`  prompt end / command input start
* `OSC 133;C`  command execution start (output follows)
* `OSC 133;D;<exit>` command finished
* `OSC 7;file://host/path` cwd
* `OSC 7770;cmd;<base64 command>`   command text (sent just before 133;C)
* `OSC 7770;new;<base64 name>`      request: create child activity, start shell there
* `OSC 7770;note;<base64 text>`     append note to current activity
* `OSC 7770;name;<base64 name>`     rename current activity
* `OSC 7770;mark;<base64 label>`    named bookmark at this output line
Environment: `CANOPY=1`, `CANOPY_ACTIVITY=<id>`, `TERM=xterm-256color`,
`COLORTERM=truecolor`.

## 6. Keybindings (defaults)
Ctrl+Shift+C/V copy/paste · Shift+Insert / middle-click paste primary · Ctrl+Shift+F search ·
Ctrl+Shift+N new child activity · Ctrl+Shift+T new sibling · Ctrl+Shift+W close activity ·
Ctrl+Shift+R rename · Ctrl+Shift+Up/Down previous/next block · Ctrl+Shift+Left/Right fold/unfold ·
Ctrl+Shift+Enter re-run selected block · Ctrl+Shift+E next error line · Ctrl+Shift+J pretty JSON ·
Ctrl+Shift+P command palette · F1 overview · F2 sidebar · F10 help · Ctrl+wheel / Ctrl+= / Ctrl+- zoom ·
Ctrl+0 reset zoom · Ctrl+Shift+= / - font size · Ctrl+Alt+Arrows move between panes ·
Alt+drag or middle-drag pan · Shift+PgUp/PgDn scrollback · Ctrl+click open path/URL ·
gutter click selects a block (double-click folds, right-click menu) · table header click sorts.
