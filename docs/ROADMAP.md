# AutomationOS Roadmap

AutomationOS is a from-scratch x86_64 operating system that boots to a graphical,
Windows-like desktop on **QEMU and real hardware** (a 2010 Lenovo ThinkPad T410).
This roadmap tracks what's done, what's in progress, and where it's going. It's a
living document — the goal is an OS you can actually *build software on*, from the
OS itself.

> Status legend: ✅ done · 🚧 in progress · 🔭 planned

---

## Vision

A self-hosting, from-scratch OS where the **IDE is the forge**: write a game,
app, driver, or service in C, hit **Ctrl+B** to compile it with the on-device
compiler, and **Ctrl+R** to run it — all without leaving the machine. A desktop
that feels like Windows (start menu, taskbar, window snapping, animations) but is
100% original code: own kernel, own compositor, own browser engine, own compiler.

---

## ✅ Done

### Kernel & boot
- ✅ x86_64 long-mode kernel, GRUB Multiboot (legacy BIOS), 4-level paging
- ✅ Cooperative scheduler (`SYS_YIELD`), `fork`/`exec`/`exit`, ring 0/3 separation, ~70 syscalls
- ✅ **Ring-3 SSE/FPU**: enabled at boot (`paging.c` `cpu_enable_fpu_sse`) + `fxsave`/`fxrstor` per task; proven by `sbin/floattest`
- ✅ **Gated preemptive scheduler** (`PREEMPT=1` → `kernel-preempt.elf`): PIT-driven, non-preemptible-kernel safety model — **validated, still gated/experimental** (see below)
- ✅ Boots to the desktop on a **physical ThinkPad T410** (i5-M520 Westmere) *and* QEMU
- ✅ **Initrd-in-`.bss` rescue** in `boot.asm` (the keystone fix that unblocked real-HW boot)
- ✅ Frozen-tick safety: every early-boot hardware spin is iteration-bounded (AHCI, PS/2)
- ✅ On-screen `[NN]` boot markers (the only debug channel on the serial-less T410)

### Desktop & UX
- ✅ From-scratch compositor: windows, a right-side dock with hover-magnify, a taskbar
- ✅ **Circular iris boot transition** (the splash reveals the desktop through a growing eased circle)
- ✅ **Dirty-rectangle present** — only changed regions hit the framebuffer (the big T410 perf win)
- ✅ **Eased window animations** (open/close/minimize)
- ✅ **Windows-11-style Start menu** (search, pinned grid, power) on the launcher button
- ✅ **Window maximize** button + edge-snap (drag to top/left/right)
- ✅ Taskbar network indicator

### Apps
- ✅ **IDE forge**: file tree, editor (syntax highlight, caps/shift typing), integrated terminal,
  **New Project from templates** (game / app / service), **Ctrl+B build → Ctrl+R run**
- ✅ Windows-11-style **File Explorer** (live VFS, New Folder, open)
- ✅ **Browser** (own DOM/HTML/CSS/layout + JS engine + TLS 1.2 + HTTPS), `about:home` + Google search bar
- ✅ Terminal, Settings, **Control Center** (quick-settings), **Photos** (PNG/BMP/GIF), Task Manager, Notes, Calculator, Clock & **Clock+** (alarms/timer/stopwatch)…
- ✅ **Games**: Snake, Tetris, 2048, Minesweeper, Breakout, Pong, Invaders, Solitaire, Chess, Asteroids, Sudoku, **Pac-Man**, Bubble Defense

### Platform
- ✅ Self-hosting **on-device C compiler** (lexer → parser → codegen → assembler → ELF writer)
- ✅ Networking: e1000 NIC (QEMU), ARP/IPv4/ICMP/UDP/TCP, DNS, TLS/HTTPS
- ✅ Filesystems: RAM root (ramfs), ext2 + fat32 read, durable disk superblock
- ✅ Shared libs: keymap (US + shift + caps), fixed-point animation/easing
- ✅ Boot smoke test (`scripts/smoke_boot.sh`) — currently 33/33 (added `FLOATTEST: PASS`)

---

## 🚧 In progress / next

### Preemptive scheduler — post-validation roadmap

The gated preemptive scheduler (`PREEMPT=1` → `kernel-preempt.elf`) is **validated**
(six never-yielding burners time-slice fairly, zero faults, `FLOATTEST: PASS`, desktop
alive, default smoke 33/33) but stays **opt-in/experimental** until the items below land
and it has soaked. Stress-test any scheduler change with `bash scripts/stress_preempt.sh`.
Planned, roughly in order:

1. 🔭 **Scheduler stats** — per-process run counts / total ticks, exposed for Task Manager
2. 🔭 **Sleep/wakeup primitives** — real timer-driven `sleep`/wait (replace busy-wait)
3. 🔭 **Priority classes** — wire nice/priority into preemptive time-slicing
4. 🔭 **Threaded `matbench`** — multi-threaded SIMD matmul (the tensor-runtime payoff)
5. 🔭 **Render-worker queue** — offload compositor/rasterizer work to preemptible workers

Only once single-core preemption is **bulletproof** (and ideally the default) does **SMP**
(true multi-core) make sense — it is a separate, larger concurrency-safety project.

- 🚧 **T410 networking**: the Intel **82577LM** NIC is detected but runtime-gated OFF (its
  MMIO bring-up stalls the bus on real hardware). Needs a proper ich8lan PHY bring-up before re-enabling.
- 🚧 **Per-app context menus** (right-click currently shows a global menu; should be app-specific)
- 🚧 **New folders on the desktop** (right-click desktop → New Folder)
- 🚧 **Drag-snap preview** overlay + window-onto-window tiling ("boxes")
- 🚧 Wire Control Center sliders (volume/brightness) + radio toggles to real kernel APIs

---

## 🔭 Planned

- 🔭 **Framebuffer write-combining (PAT)** — the next big real-hardware perf multiplier
- 🔭 **SMP (multi-core)** — only after single-core preemption is bulletproof and the default; the cooperative kernel ships single-core by design (preemption itself now exists as a gated build, see above)
- 🔭 **Bluetooth** stack + a real Ethernet driver for the T410 (82577LM)
- 🔭 Audio output beyond the PC speaker; a real media player
- 🔭 More of the browser web platform (fetch real pages reliably)
- 🔭 IDE: debugger, multi-file projects, a package/build system, themes
- 🔭 A desktop theming/personalization system (wallpapers, accent colors)
- 🔭 Persistent user data across reboots (durable home directory)

---

## Contributing

See [Building & Running](wiki/Building-and-Running.md). The project builds under WSL
Arch Linux: `bash scripts/quick_build.sh` (kernel) and `bash scripts/build_all.sh`
(full ISO), boot in QEMU or flash to USB. Created from scratch by **fourzerofour** & **Claude**.
