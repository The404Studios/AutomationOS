# AutomationOS

> A from-scratch x86_64 operating system that boots to a graphical desktop — on a real 2010 ThinkPad *and* in QEMU — with its own compiler, its own web browser, its own WiFi driver, and a gated AI agent that can drive the machine.

![Platform](https://img.shields.io/badge/platform-x86__64-blue)
![Boot](https://img.shields.io/badge/boot-GRUB%20Multiboot-orange)
![Hardware](https://img.shields.io/badge/runs%20on-ThinkPad%20T410-purple)
![Smoke tests](https://img.shields.io/badge/boot%20smoke-43%2F43-brightgreen)
![License](https://img.shields.io/badge/license-MIT-green)

## What is this?

AutomationOS is a hobby operating system written from the ground up — **no Linux, no BSD, no
existing kernel underneath**. It boots via GRUB (Multiboot, legacy BIOS) into a 64-bit
higher-half kernel, brings up its own memory management, scheduler, drivers, filesystems and
network stack, then hands off to a **from-scratch compositor desktop** with real applications.

It runs in QEMU, and it boots from a USB stick on actual hardware: a **2010 Lenovo ThinkPad
T410** (Intel Core i5-M520 "Westmere", NVIDIA NVS 3100M). The system is RAM-rooted — it boots
into a ramfs root and runs entirely from memory.

Every layer below was built by hand: the bootstrap into long mode, the allocators, the
scheduler, the drivers, the TCP/IP stack, the TLS stack, the compositor, the C compiler, the web
browser, and the WiFi driver. There is no libc underneath the userland — userspace is freestanding
and talks to the kernel through a hand-written syscall surface.

![desktop](screenshots/desktop.png)

---

## 🏆 The most incredible feats

These are the things that make AutomationOS unusual for a from-scratch hobby OS:

- **It compiles its own software, on itself.** AutomationOS ships **`cc`**, an on-device C
  compiler that lexes → parses → type-checks → generates x86-64 → assembles → writes a runnable
  **ELF**, entirely on the running machine. Write C in the IDE, press **Ctrl+B**, and run the
  result with **Ctrl+R**. No host toolchain involved — the OS is **self-hosting** for the subset
  of C it supports.

- **Its own web browser reaches the real internet over its own TLS.** `browser2` has a
  hand-written **DOM, HTML parser, CSS engine, layout engine and an ES5-subset JavaScript
  interpreter** — sitting on a **from-scratch TLS 1.2 *and* TLS 1.3** stack (RFC 8448
  known-answer-proven; X25519 + P-256 + P-384 ECDHE, RSA-PSS, AES-GCM, ChaCha20-Poly1305, full
  X.509 chain verification). It has fetched live `cloudflare.com` with a trusted certificate.

- **A from-scratch Intel WiFi driver.** A hand-written **iwlwifi DVM** driver (the kind of
  driver that is thousands of lines of register-level firmware choreography) for the T410's Intel
  1000/5000/6000-series radios: APM power-up, firmware DMA load → ALIVE → calibration, EEPROM/OTP
  NVM read, RXON, and a passive scan that harvests beacons into the network list. It even
  **auto-selects the right firmware** for whichever card is present, and surfaces its bring-up
  state **into the Network Manager GUI** so the radio can be debugged on-screen — no serial cable.

- **A gated AI agent that can drive the OS.** The "agent rail" lets a host LLM automate the
  machine (shell / files / **synthetic mouse + keyboard**) through a **capability-gated** tool
  surface with a human-supervised **cockpit** (Allow / Deny / STOP), a **tamper-evident audit
  ledger** (hash-chained), and **rollback** — the model is treated as hostile text and never
  granted unmediated control.

- **A Semantic LEGO Map IDE built for aphantasia.** The IDE renders code as a navigable map of
  "blueprints" and data-flow — the visual model the developer can't form in their head — with a
  teaching dictionary, snippet library, live re-parse, and on-device build/run.

- **WPA2 *and* WPA3.** A complete supplicant: PBKDF2, the WPA2 4-way handshake, CCMP/GCMP, and
  the **WPA3-SAE** dragonfly handshake (PWE → commit → confirm → PMK), all KAT-proven.

- **It boots on 14-year-old hardware** and **scales across cores** — a gated SMP build does real
  AP bring-up (INIT-SIPI-SIPI), per-CPU state, LAPIC timers, IPIs and TLB shootdown, and runs
  ring-3 work on a second core.

- **Real persistence + recovery.** A durable disk filesystem survives reboot, and a desktop
  **self-heal watchdog** restores the session if the compositor dies.

---

## 🖥️ Feature tour

### Desktop & compositor
A from-scratch **compositor** with window management (maximize + edge-snap, Alt+Tab overlay,
dirty-rectangle redraw), a right-side **dock** with hover-magnify, folders, eased open/close
animations, and a fluid **circular-iris boot transition**. On top of it: a **Windows-11-style
Start menu**, a **file manager**, a **Control Center**, a **Photos** viewer, a tabbed VT/ANSI
**terminal**, **Settings**, and a **task manager**.

### The IDE & self-hosting compiler
![ide](screenshots/ide.png)

`cc` (`userspace/apps/cc`) is a real C front end + x86-64 back end + assembler + ELF writer that
runs on the OS. The **IDE** (`userspace/apps/ide`) is the "Semantic LEGO Map" — file tree,
syntax-highlighting editor, integrated terminal, a navigable code/blueprint map, teaching
dictionaries, a snippet library, and **Ctrl+B build / Ctrl+R run** wired to `cc`. The editor and
compiler share one verified front end (lexer/parser/codegen/assembler/ELF writer).

### Browser & the modern web
![browser](screenshots/browser.png)

`browser2` is a from-scratch browser: its own **DOM**, **HTML parser**, **CSS engine**, **layout
engine**, an **ES5-subset JavaScript engine**, and web APIs (timers, fetch, localStorage,
console, URL), rendering over the hand-rolled **TLS 1.2/1.3 + HTTPS** stack.

### Games & apps
![game](screenshots/game.png)

25+ userspace apps under `userspace/apps/`, all proven to spawn and survive by the `GAMETEST`
harness: **snake, tetris, 2048, minesweeper, breakout, pong, space invaders, solitaire, chess,
asteroids, sudoku, pac-man**, a **zombie tower-defense** (`zombietd`), a **3D demolition derby**
(`derby`), a spinning-**cube3d** and a **raycaster** (`ray`) on the from-scratch `g3d` software
3D, plus **calculator, clock, paint, notes, a spreadsheet, photos, settings** and the **task
manager**.

### The AI agent rail
![cockpit](screenshots/cockpit.png)

`agentd` is the OS-side gated agentic loop; a host LLM drives it over the slirp seam through a
typed tool surface (shell/files/mouse/keyboard). Every dangerous action is **gated** (allow /
confirm / deny), **audited** into a tamper-evident hash-chained ledger, and **rollback-able**;
the **cockpit** GUI shows the live goal + steps with **Allow / Deny / STOP**. Proven 10/10 on the
confirm gate, with a policy-driven gate (`/etc/ai/policy.json`) and a verified live ledger.

### WiFi, networking & the Network Manager
![network](screenshots/netman_diag.png)

A from-scratch network stack: **ARP / IPv4 / ICMP / UDP / TCP** with **DNS**, BSD-style sockets,
and tools (`ping`, `nc`, `wget`, `dhcpc`). The **e1000** NIC works in QEMU; the **iwlwifi DVM**
driver brings up the real Intel radio on the T410. The animated **Network Manager** shows scanned
SSIDs with signal bars + lock icons, a masked-passphrase connect flow, and a live **"Radio:"
diagnostics line** that reports exactly where radio bring-up is (detected → firmware ALIVE →
RF-kill → scanned → or the failing step) — so WiFi can be diagnosed on-screen.

### Sound
HDA (Intel High-Definition Audio) output — codec enumeration via the Immediate Command Interface,
DMA stream playback — plus a **Sound Manager** (volume / mute / test-tone / live status) and a
`SYS_AUDIO_*` mixer surface.

### Filesystems & persistence
A **ramfs** root (RAM-rooted from initrd), **ext2** + **FAT32** read support behind a VFS with a
filesystem registry, and a durable **diskfs** superblock over AHCI so writes survive a reboot.

---

## 🧩 The API surface (from-scratch, all hand-written)

Userspace talks to the kernel through a `SYSCALL`/`SYSRET` fast path and a broad, numbered
syscall table (`kernel/include/syscall.h`). Beyond POSIX-ish basics (read/write/open/fork/execve/
mmap/waitpid) it includes:

| Area | Syscalls / surface |
|------|--------------------|
| Concurrency | `futex`, `epoll`, `poll`/`select`, threads, `io_uring`-style **batch** submit |
| Fast I/O | `sendfile`, zero-copy paths, perf counters |
| WiFi control | `SYS_WLAN_SCAN/CONNECT/STATUS/DISCONNECT/SET_KEY` + **`SYS_WLAN_DIAG`** (GUI bring-up diagnostics) |
| Audio | `SYS_AUDIO_*` mixer (volume/mute/tone/status) |
| Networking | BSD sockets, `SYS_NET_*` config, per-interface bring-up |
| Agent rail | gated typed tool channels, synthetic-input SHM, recovery/overlay |
| GUI | shared-memory framebuffer + compositor IPC, clipboard, notifications |

Each major syscall is exercised by a **boot-time probe** so regressions surface immediately. Full
details: **[docs/API_REFERENCE.md](docs/API_REFERENCE.md)** and the **[Wiki](docs/wiki/Home.md)**.

---

## 🔬 From-scratch analysis (what "from scratch" means here)

Nothing in the boot-to-desktop path depends on a third-party OS, kernel, libc, or GUI toolkit.
Built by hand, from zero:

- **Boot**: 32-bit Multiboot stub → long mode → higher-half 64-bit kernel.
- **Memory**: page-frame allocator, 4-level paging, slab + heap allocators, CoW fork, VMAs.
- **Scheduling**: cooperative core; gated **preemptive** + gated **SMP** (multi-core) builds.
- **Drivers**: serial, PIT, RTC, PS/2 kbd+mouse, PCI, framebuffer, AHCI/SATA, e1000, **iwlwifi**, HDA audio.
- **Net**: Ethernet/ARP/IPv4/ICMP/UDP/TCP/DNS + sockets.
- **Crypto/TLS**: SHA-1/256/384/512, MD5, HMAC, HKDF, AES (CCM/GCM), ChaCha20-Poly1305, RSA(+PSS),
  X25519, P-256, P-384, ASN.1/X.509 chain verification, **TLS 1.2 + TLS 1.3**, WPA2 + **WPA3-SAE**.
- **Toolchain**: C compiler (`cc`) + assembler + ELF writer, on-device.
- **GUI**: compositor, window manager, dock, an integer-only (no-float) animation/widget toolkit.
- **Apps**: browser engine, IDE, 25+ apps, the AI agent rail.

The build itself is the only thing that uses host tools (`gcc`/`nasm`/`ld`/`grub-mkrescue` to
*produce* the image); the **running system** uses none of them.

---

## 🛠️ Building & running

Builds under **WSL (Arch Linux)** with a stock host toolchain (`gcc`, `nasm`, `ld`,
`grub-mkrescue`, `qemu`). No cross-compiler needed — everything is freestanding
(`-ffreestanding -nostdlib -mno-red-zone -fno-pic`).

```bash
# Build just the kernel (fast iteration)
bash scripts/quick_build.sh

# Build the compositor + the full userspace app suite and package the bootable ISO
bash scripts/build_all.sh

# Boot it in QEMU
qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev user,id=n0 -device e1000,netdev=n0

# Run the 43-check boot smoke test
bash scripts/smoke_boot.sh
```

**Useful build flags** (env vars to `quick_build.sh` / `build_all.sh`):

| Flag | Effect |
|------|--------|
| `IWLWIFI=1` | compile the real Intel WiFi driver (T410); QEMU shows a graceful "no card" |
| `WIFI_SIM=1` | simulated WiFi backend (scan/connect/DHCP work in QEMU) |
| `HDA_ENABLE=1` | enable HDA audio (off by default; keeps the T410 boot safe) |
| `SMP=1` / `PREEMPT=1` | gated multi-core / preemptive scheduler |
| `IDE=1` | auto-open the IDE (for iteration/screenshots) |
| `GAMETEST=1` | spawn every game/app and prove each survives |
| `SHOWCASE=1` | auto-open the headline apps for documentation screenshots |
| `TLS13=1` | offer TLS 1.3 in the client |

To run on real hardware, write the ISO to a USB stick and boot it (legacy/BIOS or CSM mode).
For the T410 WiFi bring-up procedure (firmware + `iwlup` + the GUI diagnostics loop), see
**[docs/T410_IWLWIFI.md](docs/T410_IWLWIFI.md)**.

---

## ✅ Verified

- **`scripts/smoke_boot.sh`** boots the ISO under QEMU and runs **43 invariant checks** — kernel
  start, no panics/faults, fork+CoW isolation, the on-device compiler, crypto/TLS known-answer
  tests, the networking + socket path, the whole browser pipeline, and more. **43/43.**
- **`GAMETEST=1`** spawns every game + app and asserts each survives its init + render loop.
- **Boot-time KATs** (under `IWLWIFI=1`): `IWL-RXON`, `IWL-SCAN`, `IWL-FWSEL`, `IWL-FW` all PASS —
  the software-provable half of the WiFi driver (the RF tail is hardware-iterated on the T410).
- **RFC 8448** known-answer vectors prove the TLS 1.3 key schedule / record / handshake.

---

## 📖 Documentation

- **[Wiki](docs/wiki/Home.md)** — [Architecture](docs/wiki/Architecture.md) ·
  [Kernel Internals](docs/wiki/Kernel-Internals.md) · [Drivers & I/O](docs/wiki/Drivers-and-IO.md) ·
  [Desktop & Apps](docs/wiki/Desktop-and-Apps.md) ·
  [Networking & Security](docs/wiki/Networking-and-Security.md) ·
  [Browser & Web Engine](docs/wiki/Browser-and-Web-Engine.md) ·
  [Self-Hosting Compiler](docs/wiki/Self-Hosting-Compiler.md) ·
  [Building & Running](docs/wiki/Building-and-Running.md)
- **[API Reference](docs/API_REFERENCE.md)** · **[Architecture](docs/ARCHITECTURE.md)** ·
  **[Roadmap](docs/ROADMAP.md)** · **[T410 WiFi guide](docs/T410_IWLWIFI.md)**

---

## ⚠️ Status & honest limitations

This is an actively-built hobby OS, and it's honest about where it stands:

- **Cooperative + single-core by default.** Preemptive (`PREEMPT=1`) and SMP (`SMP=1`) are
  implemented and validated but remain opt-in until single-core preemption is the default.
- **GRUB Multiboot, legacy BIOS** — not UEFI.
- **The T410 WiFi *radio* tail has no emulator.** The driver's logic is KAT-proven in QEMU, but
  the actual firmware ALIVE → scan on real silicon must be iterated on the physical T410 (now made
  much easier by the on-screen "Radio:" diagnostics). WiFi association + the WiFi *data* plane
  (DHCP/traffic over `wlan0`) are the next milestones after SSIDs are confirmed on hardware.
- **`cc` compiles a careful subset of C** (no floats/`switch`, single-file) — enough to be
  self-hosting for that subset, not a full C11 compiler.
- **Some smaller apps are experiments** at varying maturity; the desktop, terminal, file manager,
  IDE, compiler, browser, WiFi and agent rail are the focus.

## Credits

Created from scratch by **fourzerofour** & **Claude**. Built with the OSDev community's collective
knowledge and tooling from GCC, NASM, GRUB and QEMU.

## License

MIT — see [LICENSE](LICENSE).
