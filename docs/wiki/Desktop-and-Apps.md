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
cooperative by default: every component yields with `SYS_YIELD` (gated
preemptive `PREEMPT=1` and SMP `SMP=1` builds also exist — see
[Kernel Internals](Kernel-Internals.md) for the scheduler model).

![The AutomationOS desktop](../../screenshots/showcase.png)

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

`render_panel()` draws the 28-px top bar: the focused window's title (or
"AutomationOS") on the left, and on the right an `HH:MM:SS` clock derived from
`SYS_GET_TICKS_MS`. Just left of the clock is a **live network indicator** —
`refresh_net_status()` (compositor_m8.c:2454) queries `SYS_NET_INFO` once per
second (piggybacked on the clock refresh) and renders the interface IP (green
signal bars) or "No Net" (gray); clicking the indicator spawns `sbin/netman`.
A **battery indicator** sits alongside it: `SYS_BATTERY` is polled once/sec and
shown as "BAT/CHG: N%" (colored by charge), present only on real laptops (absent
on QEMU). The bottom **dock** (`render_dock`) hosts a launcher button (which
spawns `sbin/terminal`) plus one taskbar button per window; minimized windows
keep a button with a small accent dot.

### Right-side dock (hover-magnify + folders)

The signature macOS-style strip is a 44-px vertical dock on the right edge
(`RDOCK_W`), `render_right_dock()`. It lists **19** app tiles (`RDOCK_NICONS`)
plus 2 folders (`rdock_apps[]` / `rdock_folders[]`, compositor_m8.c:901). The tile
roster includes Terminal, Files, IDE, Browser, NetManager, Settings, Calculator,
Clock, Editor, Snake, Tetris, 2048, Pac-Man, Clock+, **Derby 3D**, **Claude**,
**Anthropic**, the **Agent Cockpit** (`Ck` → `sbin/cockpit`), and the **Sound
Manager** (`Au` → `sbin/soundman`). Each tile's size follows a fixed-point
**magnification field**: a tile near the cursor grows (scale up via a Q8 factor
that falls off with distance), smoothing toward its target a few units per frame.
Tiles draw procedural icons from the icon library; hovering lightens the tile,
shows a tooltip to the left, and a launch click triggers a leftward **bounce**
animation. The two folders (**Games**, **Tools**) open a rainbow **fan-out** —
member icons sweep out along an arc into the workspace, bobbing with sparkles,
using a fixed-point sine table (`sin_q`). Any tile whose magnified rect would
overflow the strip is simply not drawn (it stays reachable from the Start menu).

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
| **Editor / Notes / Calculator / Clock / Paint / Calendar / Sheet** | `apps/*` | desktop accessories (sheet = a small spreadsheet) |
| **NetManager** | `apps/netman` | Wi-Fi / network manager UI with a live **Radio** diagnostics line (links the DNS lib) — see below |
| **Sound Manager** | `apps/soundman` | system audio panel over the Intel HDA driver (volume / mute / test tone) — see below |
| **Agent Cockpit** | `apps/cockpit` | the human-in-the-loop console for the on-device AI agent (`sbin/agentd`) — see below |

### The AI agent rail — cockpit, agentd, gated tools

![Agent cockpit](../../screenshots/cockpit.png)

AutomationOS can drive *itself* under human supervision. The model lives **off
device** (a host LLM reached over the QEMU slirp seam through a broker in
[`scripts/`](../../scripts)); the OS side is the gated hands. Every byte the model
emits is treated as **hostile text** and never reaches a syscall without passing
the gates.

- **`sbin/agentd`** ([`apps/agentd/agentd.c`](../../userspace/apps/agentd/agentd.c))
  is the OS-side multi-step ReAct loop. It opens one persistent connection to the
  broker, sends a `GOAL`, and the broker replies with `TOOL {...}` lines (call a
  tool) or `DONE` (finished). agentd executes the tool, captures its stdout, and
  feeds it back as a `RESULT` — bounded to `MAX_STEPS` iterations.
- **Capability-gated typed tools.** agentd never spawns a free shell. A strict
  whitelist (`resolve_tool`) maps tool *names* to a fixed set of tiny `sbin/tool_*`
  programs: read-only (`tool_read` / `tool_ls` / `tool_stat` / `tool_ps`),
  run-open-code (`tool_write` / `tool_cc` / `tool_exec`), files
  (`tool_mkdir` / `tool_mv` / `tool_rm`), apps/system (`tool_spawn` / `tool_kill`),
  rollback (`tool_rollback`), and synthetic input (`tool_mouse` / `tool_key`). Each
  call also passes a path policy (`bad_path` rejects `..` traversal). A `shell`
  tool is **deliberately omitted** — forwarding un-gated argv to `/bin` coreutils
  would defeat the write allowlist.
- **A tighten-only policy file.** `/etc/ai/policy.json` (`policy_load`) can add
  tools to the deny set or the require-approval (CONFIRM) set, but the **allow bin
  is ignored** — policy can never add a runnable tool, so `resolve_tool` stays the
  sole whitelist.
- **The cockpit GUI** ([`apps/cockpit/cockpit.c`](../../userspace/apps/cockpit/cockpit.c))
  is the human in the loop. You type a **GOAL**, click **RUN** (which spawns
  agentd), and watch the agent's steps stream into a scrolling log. The cockpit
  owns a single shared-memory contract page (`agentcockpit.h`): it writes goal /
  stop / grant-full / confirm; agentd writes state / step / tool / args / result.
  Dangerous (CONFIRM-class) tools park agentd until you click **Allow** or
  **Deny**; **STOP** halts the run; a *grant full (auto-allow)* checkbox lets a
  trusted run proceed unattended.
- **Tamper-evident audit + rollback.** agentd appends every decision to an
  append-only **hash-chained ledger** (`/var/log/ai/agent.log`, FNV-1a chain,
  verifiable with `sbin/ledgerver`), and snapshots a file before mutating it so a
  later `rollback` can restore it.

### Games

![A game running on the desktop](../../screenshots/game.png)

Spawned from the dock's **Games** folder and the Start menu — all 15 are proven
to spawn-and-survive by the `GAMETEST=1` harness ([`apps/gametest`](../../userspace/apps/gametest),
25/25 covering 15 games + 10 apps):

- **Arcade / action** — Snake, Tetris, 2048, Breakout, Pong, Invaders (space
  invaders), Asteroids, Pac-Man.
- **Puzzle / board** — Mines (minesweeper), Solitaire, Chess, Sudoku.
- **Tower defense** — **BubbleTD** and **ZombieTD** (each also doubles as an IDE
  sample project).
- **Software 3D (the from-scratch `g3d` renderer, [`lib/g3d`](../../userspace/lib/g3d))** —
  **cube3d** (spinning textured cube), **ray** (raycaster), and **Derby** (a 3D
  demolition-derby game with an arena + AI). These link `wl + bitfont + g3d`;
  there is no GPU — `g3d` rasterizes into the window's ARGB32 buffer.

The classic arcade titles (breakout / pong / invaders / solitaire / chess) link
the game-framework bundle; the rest are wl-direct.

### NetManager — Wi-Fi with on-screen radio diagnostics

![Network Manager radio diagnostics](../../screenshots/netman_diag.png)

[`apps/netman`](../../userspace/apps/netman) is the network UI: a scrollable list
of scanned Wi-Fi networks (`SYS_WLAN_SCAN` every ~3 s), a connect flow
(`SYS_WLAN_CONNECT` + spawn `sbin/wpasupp`, with a spinner while
`SYS_WLAN_STATUS` reports ASSOCIATING / 4-WAY), and — distinctively — a live
**"Radio:"** diagnostics line driven by `SYS_WLAN_DIAG` (`poll_diag`). That line
surfaces the kernel's `iwlwifi` bring-up state *on screen* (e.g. "firmware
missing", "RF-kill ON", "wlan0 LIVE", scan results), so the real Intel radio on a
physical ThinkPad T410 can be diagnosed without a serial cable. When no Wi-Fi
backend is present it reads `Radio: no Wi-Fi backend (run iwlup on the T410)`. See
[Drivers & I/O](Drivers-and-IO.md) for the driver and `sbin/iwlup`.

### Sound Manager — the Intel HDA audio panel

[`apps/soundman`](../../userspace/apps/soundman) is a small system-audio panel
over the Intel HDA driver (built with `HDA_ENABLE=1`). It drives the `SYS_AUDIO_*`
mixer syscalls: a Volume slider, an animated Mute toggle, a Test-Tone button
(440 Hz), and a status line refreshed each tick from `SYS_AUDIO_STATUS`
("Codec: present | Vol: N% | Mute: on/off | vendor 0x…", or "No audio device" on
an audioless boot). It opens already in sync with the kernel's reported volume
and mute state.

### Browser2 — a from-scratch web stack

![Browser2 rendering a page](../../screenshots/browser.png)

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
  SHA/HMAC/AES/RSA/ChaCha20-Poly1305/X25519/P-256/P-384, ASN.1/X.509, DNS, and
  HTTP(S). The stack speaks both **TLS 1.2 and TLS 1.3** (RFC 8446; TLS 1.3 is
  offered with `TLS13=1`, known-answer-proven against RFC 8448 in
  [`lib/tls/tls13_*.c`](../../userspace/lib/tls)) with ECDHE key exchange and
  RSA-PSS / ECDSA CertificateVerify. **Certificate trust is real** — the chain is
  validated against a CA bundle (`x509_verify_chain`), so `browser2` can fetch
  `https://` URLs with authenticated certificates.

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
programs are self-contained (they don't need `crt0`). This is a deliberate,
**careful subset — not full C11**: there are no floating-point types, no
`switch`, and compilation is single-file. Programs that need floats (or any of the
omitted constructs) are built with the WSL host toolchain instead.

### The IDE — an on-device forge

![The IDE Semantic LEGO Map](../../screenshots/ide.png)

[`userspace/apps/ide/ide.c`](../../userspace/apps/ide) is an editor plus the
"Semantic LEGO Map" code visualizer (the code rendered as a navigable blueprint
map — built for thinking about a program spatially), backed by ~30 toolchain
objects
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

![The IDE editor view](../../screenshots/ide_editor.png)

The build/run chain is wired to the on-device C compiler ([`apps/cc`](../../userspace/apps/cc),
the same toolchain objects the IDE links), so the machine compiles its own
programs. Because the compositor periodically rescans `/Desktop`, a program
compiled in the IDE can appear as a desktop icon and be launched without a
restart.

---

## See also

- [Home](Home.md)
- [Architecture](Architecture.md)
- [Kernel Internals](Kernel-Internals.md)
- [Drivers & I/O](Drivers-and-IO.md)
- [Building & Running](Building-and-Running.md)
- [Roadmap](../ROADMAP.md)
