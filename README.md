# AutomationOS

> A from-scratch x86_64 operating system that boots to a graphical desktop — on real 2010 hardware *and* in QEMU.

![Platform](https://img.shields.io/badge/platform-x86__64-blue)
![Boot](https://img.shields.io/badge/boot-GRUB%20Multiboot-orange)
![Scheduler](https://img.shields.io/badge/scheduler-cooperative-yellow)
![Smoke tests](https://img.shields.io/badge/boot%20smoke-32%2F32-brightgreen)
![License](https://img.shields.io/badge/license-MIT-green)

## What is this?

AutomationOS is a hobby operating system written from the ground up — no Linux, no BSD, no
existing kernel underneath. It boots via GRUB (Multiboot, legacy BIOS) into a 64-bit
higher-half kernel, brings up its own memory management, scheduler, drivers, filesystems and
network stack, then hands off to a **from-scratch compositor desktop** with real applications.

It runs in QEMU, and it boots from a USB stick on actual hardware: a **2010 Lenovo ThinkPad
T410** (Intel Core i5-M520 "Westmere", NVIDIA NVS 3100M). The system is RAM-rooted — it boots
into a ramfs root and runs entirely from memory.

Everything below was built by hand. The headline feat: AutomationOS is **self-hosting** — it
ships an on-device C compiler that turns C source into runnable ELF executables on the machine
itself, wired into an IDE with a Ctrl+B build button.

![desktop](screenshots/desktop.png)

## 📖 Documentation

- **[Wiki](docs/wiki/Home.md)** — [Architecture](docs/wiki/Architecture.md) · [Kernel Internals](docs/wiki/Kernel-Internals.md) · [Drivers & I/O](docs/wiki/Drivers-and-IO.md) · [Desktop & Apps](docs/wiki/Desktop-and-Apps.md) · [Building & Running](docs/wiki/Building-and-Running.md)
- **[Roadmap](docs/ROADMAP.md)** — what's done, in progress, and planned

## Highlights

### Kernel
- 64-bit higher-half kernel, entered from GRUB **Multiboot (legacy BIOS)** via a 32-bit stub that sets up long mode
- Physical + virtual memory management: page-frame allocator, 4-level paging, slab/heap allocators, on-demand heap growth
- **Cooperative scheduler** (`SYS_YIELD`-driven) with full context switching, `fork()` + copy-on-write address-space isolation, and `execve` ELF loading
- Ring 0/3 privilege separation, `SYSCALL`/`SYSRET` fast path, and a broad syscall surface (futex, epoll, sendfile, io_uring-style batch submission, and more — each verified by a boot probe)
- Drivers: serial, PIT, RTC, PS/2 keyboard + mouse, PCI, framebuffer, AHCI/SATA block I/O, and an NVIDIA GPU detection foundation

### Desktop & apps
- A **from-scratch compositor**: window management with **maximize + edge-snap**, dirty-rectangle redraw, a right-side **dock with hover-magnify**, folders, and **eased** open/close animations
- A fluid **circular iris boot transition** — the welcome splash reveals the desktop through a growing, eased circle
- A **Windows-11 Start menu**, a Win-11-style **file manager**, a **Control Center** (quick settings), a **Photos** viewer, a tabbed terminal (VT/ANSI), settings, a task manager, and more
- **Games**: snake, tetris, 2048, minesweeper, breakout, pong, invaders, solitaire, connect-4, **chess**, **asteroids**, **sudoku**, and others under `userspace/apps/`

### Self-hosting toolchain
- **`cc`** — an on-device C compiler (`userspace/apps/cc`) that lexes, parses, type-checks, generates x86-64, assembles, and writes ELF, entirely on the running OS
- An **IDE** (`userspace/apps/ide`) with a file tree, a syntax-highlighting editor, an integrated terminal, and **Ctrl+B** to build the open project with `cc`
- The compiler reuses the IDE's verified toolchain objects (lexer/parser/codegen/assembler/ELF writer), so the editor and the compiler share one front end

### Browser & networking
- A **from-scratch web browser** (`userspace/apps/browser2`) with its own **DOM**, **HTML parser**, **CSS engine**, **layout engine**, and a **JavaScript engine** (ES5-subset interpreter), plus web APIs (timers, fetch, localStorage, console, URL)
- A from-scratch **TLS 1.2 / 1.3 + HTTPS** stack: SHA-1/256/512, MD5, HMAC, AES, ChaCha20-Poly1305, RSA, X25519, P-256, HKDF, ASN.1/X.509 parsing + certificate verification — all hand-rolled
- Networking from scratch: an **e1000 NIC driver** (QEMU), an in-progress **Intel 82577LM** driver for the T410, and an **ARP / IPv4 / ICMP / UDP / TCP** stack with **DNS**, BSD-style sockets, and tools (`ping`, `nc`, `wget`, `dhcpc`)

### Filesystems
- A **ramfs** root (the system boots RAM-rooted from initrd)
- **ext2** and **FAT32** read support via a VFS layer with a filesystem registry
- A durable disk **superblock** (`diskfs`) over AHCI so writes can survive a reboot

### Verified by a boot smoke test
`scripts/smoke_boot.sh` boots the ISO under QEMU and runs **32 invariant checks** — kernel start,
no panics/faults, fork+CoW isolation, the on-device compiler, crypto/TLS known-answer tests, the
networking + socket path, the whole browser pipeline, and more. It currently passes **32/32**.

## Building & running

AutomationOS builds under **WSL (Arch Linux)** with a stock host toolchain (`gcc`, `nasm`, `ld`,
`grub-mkrescue`, `qemu`). No special cross-compiler is required — the kernel and userspace are
compiled freestanding (`-ffreestanding -nostdlib -mno-red-zone -fno-pic`).

```bash
# Build just the kernel (fast iteration)
bash scripts/quick_build.sh

# Build the compositor + the full userspace app suite and package the bootable ISO
bash scripts/build_all.sh

# Boot it in QEMU
qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev user,id=n0 -device e1000,netdev=n0

# Run the 32-check boot smoke test (optionally with --build to rebuild first)
bash scripts/smoke_boot.sh
```

Toolchain notes, lightly: userspace ELFs are linked with `-mstackrealign` (the kernel enters
`_start` with a 16-aligned stack, off-by-8 from the post-call ABI GCC assumes), and the kernel's
`.bss` is kept lean so it doesn't collide with the GRUB-placed initrd. The build's canary check
exists to confirm no `fs:0x28` stack-protector references slip in.

To run on real hardware, write the ISO to a USB stick and boot it (legacy/BIOS or CSM mode).

## Status & limitations

This is an actively-built hobby OS, and it's honest about where it stands:

- **Cooperative, single-core.** The scheduler is cooperative (`SYS_YIELD`) — there is **no timer
  preemption**, and the system runs **single-core** (the SMP code exists in-tree but is
  intentionally not compiled yet).
- **GRUB Multiboot, legacy BIOS** — not UEFI.
- **T410 networking is in progress.** The e1000 path works under QEMU; the Intel **82577LM**
  driver for the physical ThinkPad is still being brought up.
- **Some apps are works-in-progress.** The core desktop, terminal, file manager, IDE, compiler
  and browser are the focus; a number of the smaller apps are experiments at varying maturity.

## Credits

Created from scratch by **fourzerofour** & **Claude**.

Built with the help of the OSDev community's collective knowledge, and tooling from GCC, NASM,
GRUB, and QEMU.

## License

MIT — see [LICENSE](LICENSE).
