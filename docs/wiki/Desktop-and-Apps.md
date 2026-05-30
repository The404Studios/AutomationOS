# Desktop & Apps

This page documents the **userspace** of AutomationOS: the compositor that paints
the Windows-like desktop, the small "Wayland-lite" protocol its apps speak, the
shared client libraries, the bundled application suite, and the on-device C
compiler that lets the machine build its own programs. Everything here runs in
**ring 3** (no libc; most apps use integer / fixed-point math by convention,
since the on-device `cc` compiler is integer-only — but SSE/FPU **is** enabled
and context-switched for user tasks, so gcc-built code uses floats fine, proven
by `sbin/floattest`) and is compiled fresh into the
initrd by [`scripts/build_all.sh`](../../scripts/build_all.sh).

The compositor is launched by `init` as `sbin/compositor`; apps are spawned from
the dock, the Start menu, or desktop icons via `SYS_SPAWN`. The whole desktop is
cooperative: every component yields with `SYS_YIELD` (see
[Kernel Internals](Kernel-Internals.md) for the scheduler model).

---

## The compositor

The live compositor is a single freestanding translation unit,
[`userspace/compositor/compositor_m8.c`](../../userspace/compositor/compositor_m8.c)
(~3650 lines). The build script compiles exactly this file (build_all.sh:27);
the file's banner still reads "compositor_m6" because m8 was copied forward from
the m6 source, but `sbin/compositor` is m8. It links only the bitmap font
(`bitfont.o`) and the procedural icon library (`icon.o`).

### Frame loop & buffers

`main()` (compositor_m8.c:3490+) acquires the framebuffer with `SYS_FB_ACQUIRE`,
then `SYS_MMAP`s up to three off-screen buffers:

- **`back`** — the back buffer every frame is composited into.
- **`prev`** — the previously presented frame, used for dirty-rectangle present.
- **`splash`** — a one-time copy of the kernel's boot framebuffer, captured so
  the desktop can dissolve in over the "Welcome to AutomationOS" splash.

If the back-buffer `mmap` fails the compositor degrades to drawing straight to
the hardware framebuffer (and skips the iris/dirty-rect optimizations). The loop
(compositor_m8.c:3594) drains client requests, pumps input, runs the shell mouse
logic, ticks animations, reaps dead windows, composites, and presents — then
always yields at least once and paces itself to a ~16 ms budget.

### Circular iris boot transition

For the first `BOOT_FADE_MS` (900 ms) the desktop is revealed through a growing
soft-edged circle centered on screen, over the captured splash —
`present_circle_iris()` (compositor_m8.c:2526). Progress `t` (0..256) is eased
with a fixed-point smoothstep (`3t² − 2t³`); the radius grows to the
center-to-corner distance (`isqrt32` of half-width² + half-height²) so the circle
covers the whole screen by the end. A 28-px feather band cross-fades back→splash
per pixel. After the window elapses, the splash buffer is dropped and the normal
present path takes over.

### Dirty-rectangle present

Outside the boot window the compositor presents with `present_diff()`
(compositor_m8.c:2466): it scans `back` vs `prev`, computes the bounding box of
changed pixels, and copies **only that box** to the slow hardware framebuffer
(then updates `prev`). If nothing changed it writes nothing. This is the main
per-frame performance win on a real screen. `present_crossfade()` and a plain
full-copy `present()` exist as fallbacks.

### Window management

The window registry holds up to **16** windows (`MAX_WINDOWS`), each a `window_t`
slot carrying its surface, frame geometry, z-order, animation phase, and snap
state. Three small index lists drive behavior: a **z-order** list (back→front,
focus = topmost), and a **most-recently-used (MRU)** ring used by Alt-Tab.

- **Drag-move** — grab a titlebar to focus + raise and drag (compositor_m8.c:3432).
- **Close / minimize** — red and amber titlebar boxes start the close / minimize
  animations; minimized windows stay parked with a taskbar button.
- **Maximize button** — a third titlebar box (a square-outline glyph drawn left
  of minimize, font-independent) toggles `SNAP_MAX`: first click tweens the
  window to fill the work area, second click tweens it back to the saved
  pre-maximize geometry (compositor_m8.c:3416).
- **Edge snap** — while dragging, the cursor near a screen edge **arms** a snap
  zone and draws a translucent preview; on release the window snaps and animates
  into place. Zones are `SNAP_LEFT` / `SNAP_RIGHT` (left/right half of the work
  area) and `SNAP_MAX` (top edge → maximize) — `snap_zone_for_cursor()`
  (compositor_m8.c:2989). The right-snap edge and the work area both account for
  the right-side dock width (`RDOCK_W`). Each window saves its pre-snap geometry
  so snapping is reversible.
- **Alt-Tab** — the compositor tracks Left-Alt and intercepts `Tab` to cycle the
  MRU ring (`wm_handle_key`, compositor_m8.c:2834). The newly focused window is
  raised but the MRU order isn't committed until Alt is released, so repeated
  taps cycle predictably. Also intercepted while Alt is held: **Alt+Q / Alt+F4**
  (close), **Alt+M** (minimize), **Alt+K** (force-quit via `SIGKILL` + a liveness
  sweep). Every non-chord key is forwarded to the focused client untouched.

### Eased window animations

Open / close / minimize / restore / snap are all driven off `SYS_GET_TICKS_MS`
in **fixed point** — there is no float, so GCC never emits libgcc soft-float
helpers that wouldn't link under `-nostdlib`. Linear progress comes from
`anim_linear_t`; `anim_eased_t()` (compositor_m8.c:1043) applies an ease-out-cubic
(`1 − (1−t)³`). Phases:

| Phase | Duration | Effect |
|-------|----------|--------|
| `PH_OPENING` | 180 ms | scale 0.90 → 1.00, alpha 0 → 256 |
| `PH_CLOSING` | 150 ms | scale → 0.90, alpha → 0, then slot is torn down |
| `PH_MINIMIZING` | 220 ms | shrink + slide toward the taskbar button |
| `PH_RESTORING` | 220 ms | reverse of minimize |
| `PH_SNAPPING` | 170 ms | geometry tween (snap / maximize / restore) |

Additionally every new window gets a cheap 150 ms compositor-side **fade-in**
(`advance_fade_in`), independent of the scale animation. Animated windows are
drawn with a nearest-neighbor scale-about-center + alpha blit
(`blit_surface_scaled_alpha`); settled windows use the fast opaque blit.

### Top panel / taskbar

`render_panel()` (compositor_m8.c:1656) draws the 28-px top bar: the focused
window's title (or "AutomationOS") on the left, and on the right an `HH:MM:SS`
clock derived from `SYS_GET_TICKS_MS`. Just left of the clock is a 4-bar
**network indicator** — note this is currently **static decoration** in a neutral
color; the source comments that the compositor has no network-status syscall
wired in yet. The bottom **dock** (`render_dock`) hosts a launcher button (which
spawns `sbin/terminal`) plus one taskbar button per window; minimized windows
keep a button with a small accent dot.

### Right-side dock (hover-magnify + folders)

The signature macOS-style strip is a 64-px vertical dock on the right edge,
`render_right_dock()` (compositor_m8.c:2140). It lists 21 app tiles plus 2
folders (`rdock_apps[]` / `rdock_folders[]`, compositor_m8.c:592). Each tile's
size follows a fixed-point **magnification field**: a tile near the cursor grows
(scale up to ~1.9× via a Q8 factor that falls off over ~110 px), smoothing toward
its target a few units per frame. Tiles draw procedural icons from the icon
library; hovering lightens the tile, shows a tooltip to the left, and a launch
click triggers a leftward **bounce** animation. The two folders (**Games**,
**Tools**) open a rainbow **fan-out** — member icons sweep out along an arc into
the workspace, bobbing with sparkles, using a fixed-point sine table (`sin_q`).
Any tile whose magnified rect would overflow the strip is simply not drawn (it
stays reachable from the Start menu).

### Desktop icons, Start menu, context menu

The compositor enumerates `/Desktop` via `SYS_OPENDIR`/`READDIR`/`CLOSEDIR` and
lays the entries out as labeled wallpaper icons (`desk_scan`, rescanned
periodically so freshly compiled IDE output appears). Double-click runs them. The
launcher opens a **Start menu** popup; right-clicking the desktop opens a
**context menu** (Minimize All / Close All / About / New Folder). A transient
top-right **toast** (`toast_show`) fades in at boot.

---

## The window protocol

Apps don't draw to the screen — they hand the compositor a shared-memory surface
and receive input events over a SysV message queue. This "Wayland-lite" protocol
is implemented client-side in
[`userspace/lib/wl/wl_client.c`](../../userspace/lib/wl/wl_client.c)
(header: [`wl_client.h`](../../userspace/lib/wl/wl_client.h)).

**Connect & create.** `wl_connect()` opens (creating if needed) the shared
compositor inbox queue keyed `WL_COMP_INBOX_KEY` ("COMP") and a per-process reply
queue keyed `WL_REPLY_KEY(pid)`. `wl_create_window(w, h, title)` allocates a
private shm segment (`SYS_SHMGET`), maps it (`SYS_SHMAT`) as a zero-copy ARGB32
buffer, sends `WL_REQ_CREATE`, and waits for `WL_EVT_CREATED` to learn its
`win_id`. (The reply wait is a bounded spin-yield, since blocking `msgrcv` isn't
implemented kernel-side — a dead compositor cleanly returns NULL.)

**Frames & input.** The app draws directly into `win->pixels` (stride `w*4`
bytes) and calls `wl_commit()` to send `WL_REQ_COMMIT` (full-surface damage).
`wl_poll_event()` does a non-blocking `SYS_MSGRCV` and reports:

- **`WL_EVENT_POINTER`** (a=x, b=y, c=buttons)
- **`WL_EVENT_KEY`** (a=keycode, b=pressed)

The compositor forwards pointer events to the focused window and routes every key
through its WM-shortcut filter first, forwarding only un-consumed keys.

### Shared client libraries

| Library | Path | Role |
|---------|------|------|
| **wl** | [`userspace/lib/wl`](../../userspace/lib/wl) | the windowing protocol client above |
| **ui** | [`userspace/lib/ui/ui.c`](../../userspace/lib/ui/ui.c) | retained-mode widget toolkit (panels / labels / buttons); owns the event loop, layout, render, and click dispatch over wl + font |
| **keymap** | [`userspace/lib/keymap`](../../userspace/lib/keymap) | shared US-QWERTY translation + modifier state (shift / caps / ctrl / alt) so apps get real characters, not raw scancodes |
| **anim** | [`userspace/lib/anim`](../../userspace/lib/anim) | fixed-point easing + tweening (linear/quad/cubic/back/elastic/bounce, smoothstep), color lerp, and a critically-damped spring stepper — all integer/Q16.16, the only dep being `SYS_GET_TICKS_MS` |

GUI apps generally link one of three bundles (see the `build_*_app` helpers in
build_all.sh): **ui-app** (`ui + wl + font`), **wl-direct** (`wl + font`, app
draws its own pixels), or **game-framework** (`game + wl + font`).

---

## The app suite

Everything below is built and packaged by build_all.sh into `/sbin` (GUI apps,
dock-launchable) or `/bin` (CLI tools the shell can spawn).

| App | Path | What it is |
|-----|------|------------|
| **File Manager** | `apps/filemanager` | Windows-11-style explorer (ui toolkit) |
| **Terminal** | `apps/terminal` | VT terminal; links a native git-like VCS (`sh_git`) |
| **Browser2** | `apps/browser2` | from-scratch GUI browser: DOM + HTML + CSS + layout + JS engine + TLS/HTTPS (see below) |
| **Browser** | `apps/browser` | earlier wl-direct browser (HTTP/HTTPS + simplified render) |
| **IDE** | `apps/ide` | editor + Semantic LEGO Map + on-device build/run (see below) |
| **Control Center** | `apps/controlcenter` | quick-settings panel |
| **Photos** | `apps/photos` | image viewer; links the PNG/BMP/GIF codecs |
| **Settings** | `apps/settings` | settings panels (ui toolkit) |
| **Task Manager** | `apps/taskman` | process list / system info |
| **Editor / Notes / Calculator / Clock / Paint / Calendar** | `apps/*` | desktop accessories |
| **NetManager** | `apps/netman` | network manager UI (links the DNS lib) |

### Games

Spawned from the dock's **Games** folder and the Start menu. Snake, Tetris, 2048,
Mines, Breakout, Pong, Invaders (space invaders), Solitaire, Chess, Asteroids,
Sudoku, and **BubbleTD** (a tower-defense game that doubles as an IDE sample
project). The arcade titles (breakout/pong/invaders/solitaire/chess) link the
game-framework bundle; the rest are wl-direct.

### Browser2 — a from-scratch web stack

`browser2` is the flagship app. It links the whole web pipeline as separate
freestanding libraries:

- **DOM** ([`lib/dom`](../../userspace/lib/dom)) — nodes, selectors, events,
  serialization.
- **HTML** ([`lib/html`](../../userspace/lib/html)) and **CSS**
  ([`lib/css`](../../userspace/lib/css)) parsers, plus a **layout** engine
  ([`lib/layout`](../../userspace/lib/layout)).
- **JavaScript** ([`lib/js`](../../userspace/lib/js)) — an ES5-subset
  lexer/parser/interpreter with a native-object bridge, DOM bindings, and web
  APIs (timers, `fetch`, storage, console, URL). It renders the DOM and runs
  `<script>` tags.
- **TLS / HTTPS** — a real crypto + TLS stack ([`lib/crypto`](../../userspace/lib/crypto),
  [`lib/tls`](../../userspace/lib/tls), [`lib/net`](../../userspace/lib/net)):
  SHA/HMAC/AES/RSA/ChaCha20-Poly1305/X25519/P-256, ASN.1/X.509 verification with
  a CA bundle, DNS, and HTTP(S). So `browser2` can fetch `https://` URLs.

Each web layer also ships a boot-time self-test (`domtest`, `htmltest`,
`csstest`, `layouttest`, `webtest`, `webapitest`) that prints PASS/FAIL.

---

## The self-hosting toolchain

AutomationOS compiles its own C programs **on the device**. The compiler is the
IDE's verified toolchain, exposed two ways.

### `cc` — the command-line compiler

[`userspace/apps/cc/cc.c`](../../userspace/apps/cc/cc.c) is `/bin/cc`. It is a
thin **driver** — it contains no compiler. It links the IDE's already-compiled
toolchain objects and runs exactly their pipeline:

```
src text  --(ide_lex + ide_parser)-->  AST
AST       --(cc_codegen / cc_expr)-->  Intel-subset x86-64 asm text
asm       --(as_x64 assembler)------>  machine code @ TC_ENTRY_VADDR
code      --(elf_write)------------->  a static ELF64 the loader can run
```

Usage: `cc INPUT.c -o OUTPUT` (or `./a.out` by default; bare `cc` runs a
self-test). The supported C subset is documented in the file header: 64-bit
int/pointer types (char/_Bool are 1 byte), globals + locals, functions of ≤6
params, `if`/`while`/`for`, the usual operators incl. recursion, string literals,
arrays, pointers, struct member layout, and `sys_write` / `sys_exit` builtins
that emit `syscall`. Output ELFs carry their own `_start` trampoline, so compiled
programs are self-contained (they don't need `crt0`).

### The IDE — an on-device forge

[`userspace/apps/ide/ide.c`](../../userspace/apps/ide) is an editor plus the
"Semantic LEGO Map" code visualizer, backed by ~30 toolchain objects
(`ide_lex` → `ide_parser` → `cc_codegen` → `as_x64` → `elf_write`; see the
`IDE_SRCS` list in build_all.sh:322). Key bindings (ide.c:47):

- **Ctrl+N** — New Project: pick a template from `/usr/src/templates`
  (game / app / service starters), name it, and the IDE clones the template dir
  into `/usr/src/<name>/` and opens its main `.c`.
- **Ctrl+B** — Build the open file with the native toolchain (it flushes unsaved
  edits first, then re-reads and compiles), surfacing output in the Build view.
- **Ctrl+R** — Run the last build by `SYS_SPAWN`-ing the produced ELF.
- **Ctrl+S** save, **Ctrl+E** toggle editor / LEGO-map workspace,
  **Ctrl+`** focus the embedded terminal.

Because the compositor periodically rescans `/Desktop`, a program compiled in the
IDE can appear as a desktop icon and be launched without a restart.

---

## See also

- [Home](Home.md)
- [Architecture](Architecture.md)
- [Kernel Internals](Kernel-Internals.md)
- [Drivers & I/O](Drivers-and-IO.md)
- [Building & Running](Building-and-Running.md)
- [Roadmap](../ROADMAP.md)
