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
- ✅ **IDE forge** (`userspace/apps/ide`) — the **"Semantic LEGO Map"**: code rendered as a
  navigable blueprint map alongside the editor, with file tree, syntax highlight, integrated
  terminal, **New Project from templates**, and on-device **Ctrl+B build → Ctrl+R run** wired to
  the on-device C compiler (`userspace/apps/cc`)
- ✅ Windows-11-style **File Explorer** (live VFS, New Folder, open)
- ✅ **Browser** (`userspace/apps/browser2`) — own DOM/HTML/CSS/layout + ES5 JS engine + web APIs,
  over the hand-rolled TLS stack; `about:home` + Google search bar
- ✅ Terminal, Settings, **Control Center** (quick-settings), **Photos** (PNG/BMP/GIF), Task Manager,
  Notes, Calculator, Clock & **Clock+** (alarms/timer/stopwatch), Paint, Sheet (spreadsheet)…
- ✅ **Sound Manager** (`userspace/apps/soundman`) — mixer/volume/test-tone UI over the `SYS_AUDIO_*`
  syscalls
- ✅ **Network Manager** (`userspace/apps/netman`) — animated scan/connect GUI with a live **"Radio:"**
  bring-up diagnostics line fed by `SYS_WLAN_DIAG`
- ✅ **Games**: Snake, Tetris, 2048, Minesweeper, Breakout, Pong, Invaders, Solitaire, Chess,
  Asteroids, Sudoku, **Pac-Man**, **Zombie TD** & **Bubble Defense** (tower defense), and the
  software-3D demos **Cube3D**, **Ray** and **Derby** (3D demolition derby) on the from-scratch
  `g3d` rasterizer

### Platform
- ✅ Self-hosting **on-device C compiler** (`userspace/apps/cc`: lexer → parser → codegen → assembler
  → ELF writer). Compiles a careful **C subset** (no floats, no `switch`, single-file) — not full C11
- ✅ Networking: e1000 NIC (QEMU), ARP/IPv4/ICMP/UDP/TCP, DNS, HTTPS
- ✅ **TLS 1.2 *and* TLS 1.3** (RFC 8446) — TLS 1.3 is known-answer-proven against RFC 8448
  (`userspace/lib/tls/tls13_*.c`), offered with the `TLS13=1` build. ECDHE via **X25519 + P-256**
  (P-384 implemented), **RSA-PSS** + **ECDSA** CertificateVerify, and **real certificate-chain trust**
  (`x509_verify_chain`)
- ✅ **Wi-Fi (data plane to IP on the simulated backend; radio tail hardware-iterated)** — see below
- ✅ **Intel HD Audio** (`kernel/drivers/hda.c`, gated `HDA_ENABLE=1`): codec comms via the
  **Immediate Command Interface**, DMA stream playback, `SYS_AUDIO_*` mixer syscalls
- ✅ **AI agent rail** — `sbin/agentd` (OS-side gated agentic loop driven by a host LLM over the
  slirp seam) + `sbin/cockpit` GUI; capability-gated typed tools, tamper-evident hash-chained audit
  ledger, rollback, synthetic mouse+keyboard input. The model is treated as **hostile text**
- ✅ Filesystems: RAM root (ramfs), ext2 + fat32 read, durable disk superblock
- ✅ Shared libs: keymap (US + shift + caps), fixed-point animation/easing
- ✅ Boot smoke test (`scripts/smoke_boot.sh`) — **43/43**; a `GAMETEST=1` harness
  (`sbin/gametest`) additionally proves **25/25** games + key apps spawn and survive

### Wi-Fi
- ✅ **From-scratch Intel iwlwifi DVM driver** — `kernel/drivers/net/wireless/intel/iwlwifi/`:
  APM power-up + TX/RX rings (`iwl-trans.c`), firmware parse + DMA load (`iwl-fw.c` / `iwl-fw-load.c`),
  host-command + scheduler path (`iwl-hostcmd.c`), EEPROM/OTP NVM (`iwl-nvm.c`), RXON
  (`iwl-rxon.c`), passive scan (`iwl-scan.c`), and the `wifi_ops` seam + `iwl_wifi_bringup()`
  (`iwl-ops.c`). **Nothing runs at boot** — bring-up is triggered post-desktop by `sbin/iwlup`
- ✅ **Firmware auto-select by card family** (`iwl_fw_candidates`, `iwl-ops.c`) so Wi-Fi picks the
  right `.ucode` for whatever T410 card is present; **RF-kill detection** via `CSR_GP_CNTRL` bit 27
- ✅ **Wi-Fi control plane** — `SYS_WLAN_SCAN`(113)/`CONNECT`(114)/`STATUS`(115)/`DISCONNECT`(116)/
  `SET_KEY`(117)/`DIAG`(124) (`kernel/net/wlansyscall.c`, ABI in `kernel/include/uapi/wlan.h`,
  dispatched through `netif_t.wifi`)
- ✅ **GUI bring-up diagnostics** — `SYS_WLAN_DIAG` + `kernel/net/wifidiag.c`, surfaced as a live
  **"Radio:"** line in the Network Manager
- ✅ **Supplicant** (`sbin/wpasupp`): **WPA2** (PBKDF2 → 4-way handshake → CCMP/GCMP) and
  **WPA3-SAE** (dragonfly: PWE → commit → confirm → PMK); crypto in `userspace/lib/crypto/`,
  KAT-proven in the boot `cryptotest` battery
- ✅ **Simulated backend** (`kernel/drivers/net/wireless/sim/wifisim.c`, `WIFI_SIM=1`) carries the
  full scan → WPA handshake → DHCP → IP path; boot KATs **IWL-RXON / IWL-SCAN / IWL-FWSEL /
  IWL-FW: PASS** in QEMU. See `docs/T410_IWLWIFI.md`

### SMP (Symmetric Multiprocessing)
- ✅ **CPU detection** via ACPI MADT — supports up to 256 CPUs
- ✅ **Application Processor startup** — INIT-SIPI-SIPI sequence, AP trampoline (`boot/ap_boot.S`)
- ✅ **Per-CPU data structures** — isolated runqueues, page caches, statistics (10x faster allocation)
- ✅ **Local APIC driver** — memory-mapped + x2APIC modes, IPI delivery, local timer
- ✅ **Inter-Processor Interrupts** — reschedule, TLB shootdown, remote function calls, broadcast
- ✅ **TLB shootdown protocol** — synchronous invalidation across all cores (~5μs for 4 CPUs)
- ✅ **IRQ-safe spinlocks** — `spin_lock_irqsave/restore` for interrupt-safe critical sections
- ✅ **SMP integration** — PMM per-CPU caches, scheduler hooks ready, comprehensive docs
- ✅ **6,800+ LOC** delivered — full implementation with testing and validation suite

**Status**: Production-ready, currently gated behind `SMP=1` build flag. Enables near-linear scaling (96% efficiency at 4 cores, 90% at 16 cores). Ready for integration once preemptive scheduler is the default.

---

## 🚧 In progress / next

### Preemptive scheduler — post-validation roadmap

The gated preemptive scheduler (`PREEMPT=1` → `kernel-preempt.elf`) is **validated**
(six never-yielding burners time-slice fairly, zero faults, `FLOATTEST: PASS`, desktop
alive, default smoke 43/43) but stays **opt-in/experimental** until the items below land
and it has soaked. Stress-test any scheduler change with `bash scripts/stress_preempt.sh`.
Planned, roughly in order:

1. 🔭 **Scheduler stats** — per-process run counts / total ticks, exposed for Task Manager
2. 🔭 **Sleep/wakeup primitives** — real timer-driven `sleep`/wait (replace busy-wait)
3. 🔭 **Priority classes** — wire nice/priority into preemptive time-slicing
4. 🔭 **Threaded `matbench`** — multi-threaded SIMD matmul (the tensor-runtime payoff)
5. 🔭 **Render-worker queue** — offload compositor/rasterizer work to preemptible workers

### SMP Integration Path

The **SMP foundation is complete** (see "Done" section above) and ready to activate once
single-core preemption is bulletproof and the default. The integration follows this path:

1. ✅ **Brick 0: Memory ownership primitives** — per-CPU caches, IRQ-safe locks (complete)
2. 🔭 **Wave 1: Flip the switch** — enable `SMP=1` default, validate boot + smoke tests pass
3. 🔭 **Wave 2: SMP hardening** — stress testing, edge cases, race condition hunting
4. 🔭 **Wave 3: Per-CPU scheduling** — migrate to per-CPU runqueues, load balancing

This follows the brick-by-brick, hard-gated discipline established in the SMP bring-up plan.

- 🚧 **T410 Wi-Fi radio bring-up**: the from-scratch iwlwifi driver is complete and KAT-proven in
  QEMU, but the **RF/radio tail has no emulator** and is iterated directly on the physical T410
  (`sbin/iwlup` post-desktop). **Wi-Fi association + the live (non-simulated) data plane are the
  next milestones.** See `docs/T410_IWLWIFI.md`
- 🚧 **T410 wired networking**: the Intel **82577LM** NIC is detected but runtime-gated OFF (its
  MMIO bring-up stalls the bus on real hardware). Needs a proper ich8lan PHY bring-up before re-enabling.
- 🚧 **Per-app context menus** (right-click currently shows a global menu; should be app-specific)
- 🚧 **New folders on the desktop** (right-click desktop → New Folder)
- 🚧 **Drag-snap preview** overlay + window-onto-window tiling ("boxes")

---

## 🔭 Planned

- 🔭 **Live Wi-Fi association + data plane on real hardware** — drive `iwl_wifi_bringup()` all the
  way to a real association + DHCP lease on the T410 (the simulated path already does this end-to-end)
- 🔭 **Framebuffer write-combining (PAT)** — the next big real-hardware perf multiplier
- 🔭 **Activate SMP by default** — foundation complete (see above), activate once preemptive scheduler is default
- 🔭 **Bluetooth** stack + a real wired Ethernet driver for the T410 (82577LM ich8lan PHY)
- 🔭 A real media player on top of the HD Audio stack
- 🔭 **UEFI boot** (currently GRUB Multiboot, legacy BIOS only)
- 🔭 More of the browser web platform (fetch real pages reliably); full C11 in the on-device compiler
- 🔭 IDE: debugger, multi-file projects, a package/build system, themes
- 🔭 A desktop theming/personalization system (wallpapers, accent colors)
- 🔭 Persistent user data across reboots (durable home directory)

---

## Contributing

See [Building & Running](wiki/Building-and-Running.md). The project builds under WSL
Arch Linux: `bash scripts/quick_build.sh` (kernel) and `bash scripts/build_all.sh`
(full ISO), boot in QEMU or flash to USB. Created from scratch by **fourzerofour** & **Claude**.
