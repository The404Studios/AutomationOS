# AutomationOS

> A from-scratch x86_64 operating system with a custom kernel, compositor desktop, self-hosted C compiler, IDE, and browser — booting on QEMU **and a real 2010 ThinkPad T410**.

![Platform](https://img.shields.io/badge/platform-x86__64-blue)
![Boot](https://img.shields.io/badge/boot-GRUB%20Multiboot-orange)
![Scheduler](https://img.shields.io/badge/scheduler-cooperative%20single--core-yellow)
![Hardware](https://img.shields.io/badge/hardware-ThinkPad%20T410-success)
![Self-hosting](https://img.shields.io/badge/self--hosting-on--device%20C%20compiler-brightgreen)
![License](https://img.shields.io/badge/license-MIT-green)

![desktop](screenshots/desktop.png)

---

## What it is

AutomationOS is a hobby operating system written from the ground up — no Linux, no BSD, no
existing kernel underneath. A 32-bit GRUB Multiboot stub enters long mode and hands off to a
64-bit higher-half kernel that brings up its own memory management, scheduler, drivers,
filesystems, and TCP/IP stack, then starts a **from-scratch compositor desktop** with real
applications. It is **RAM-rooted**: it boots into a ramfs and runs entirely from memory.

It runs in QEMU and it boots from a USB stick on actual hardware — a **2010 Lenovo ThinkPad
T410** — all the way to a usable desktop.

## Why it matters — the OS is its own forge

The headline is **self-hosting**: AutomationOS ships an **on-device C compiler** that turns C
source into runnable ELF executables *on the machine itself*, wired into an **IDE** with a
Ctrl+B build button. You can open the IDE on the running OS, write a C program, build it, and
launch it as a new desktop process — no external toolchain in the loop. The long-term north
star is "the IDE is the forge": the OS can build and grow its own software from the inside.

## What works today

- **Boots to a graphical desktop** in QEMU and on the physical T410, from a single bootable ISO.
- **On-device C/ASM → ELF compiler** (`cc`) feeding an **IDE** that builds and runs your code.
- **Compositor desktop**: draggable/maximizable windows with edge-snap, a hover-magnify dock,
  a Start menu, file manager, terminal (VT/ANSI), settings, task manager, and a suite of games.
- **From-scratch networking** under QEMU: e1000 driver + ARP/IPv4/ICMP/UDP/TCP + DNS + BSD
  sockets + tools (`ping`, `nc`, `wget`, `dhcpc`), and a hand-rolled **TLS 1.2 / HTTPS** client.
- **A from-scratch web browser** with its own DOM, HTML/CSS/layout engines, and an ES5-subset
  JavaScript interpreter.
- **PS/2 keyboard + trackpad** input, validated on real T410 hardware.

See the [**status matrix**](#status-matrix) for the honest stable-vs-experimental breakdown.

## Hardware proof

The strongest claim this project makes is that it boots on real, decade-old hardware:

**Lenovo ThinkPad T410** — Intel Core i5-M520 ("Westmere"), QM57 / Ibex Peak-M PCH, NVIDIA NVS 3100M framebuffer

| Capability | State |
|---|---|
| Boot from USB → compositor desktop | ✅ working |
| PS/2 keyboard + trackpad | ✅ working |
| Framebuffer graphics | ✅ working |
| External USB mouse | 🚧 in progress — PCH is **EHCI-only**; `USB-EHCI-0` brick underway |
| Ethernet (Intel 82577LM) | ⚙️ gated off by default — PCH MMIO stall under bring-up |

Full details and the reproduction profile: [`docs/hardware.md`](docs/hardware.md).

## Building & running

AutomationOS builds under **WSL (Arch Linux)** with a stock host toolchain (`gcc`, `nasm`, `ld`,
`grub-mkrescue`, `qemu`) — no cross-compiler. Kernel and userspace are compiled freestanding
(`-ffreestanding -nostdlib -mno-red-zone -fno-pic`).

```bash
# Build just the kernel (fast iteration)
bash scripts/quick_build.sh

# Build the compositor + the full userspace app suite and package the bootable ISO
bash scripts/build_all.sh

# Boot it in QEMU
qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev user,id=n0 -device e1000,netdev=n0

# Run the boot smoke test (optionally with --build to rebuild first)
bash scripts/smoke_boot.sh
```

**Reproduce the T410 boot:** build the T410-safe profile (`T410_SAFE=1 SCHED_DEBUG=0`, which
disables modern-CPU fast paths and the on-screen scheduler markers), write the ISO to a USB
stick, and boot it in **legacy BIOS / CSM** mode. The exact recipe is in
[`docs/hardware.md`](docs/hardware.md).

Toolchain notes: userspace ELFs link with `-mstackrealign` (the kernel enters `_start` with a
16-aligned stack, off-by-8 from the post-call ABI GCC assumes), and the kernel's `.bss` is kept
lean so it doesn't collide with the GRUB-placed initrd. A build canary check confirms no
`fs:0x28` stack-protector references slip in.

## Architecture overview

```
GRUB Multiboot (32-bit stub) → long mode → 64-bit higher-half kernel
   ├─ memory:     PMM · 4-level paging · slab/heap · on-demand growth
   ├─ scheduler:  cooperative single-core (default) · fork+CoW · execve ELF
   ├─ drivers:    serial · PIT · RTC · PS/2 · PCI · framebuffer · AHCI · e1000
   ├─ fs:         ramfs root · ext2/FAT32 (read) · diskfs durable blob (VFS)
   ├─ net:        ARP · IPv4 · ICMP · UDP · TCP · DNS · sockets · TLS 1.2
   └─ syscalls:   ring0/3 split · SYSCALL/SYSRET · futex/epoll/sendfile/…
        └─ userspace (ramfs): init → compositor desktop
              ├─ cc (on-device C→ELF compiler) + IDE (Ctrl+B build/run)
              ├─ file manager · terminal · settings · task manager · games
              └─ from-scratch browser (DOM/HTML/CSS/layout/JS)
```

Deeper dives live in the [wiki](docs/wiki/Home.md) and [`docs/architecture.md`](docs/architecture.md).

## Status matrix

Honest about where it stands. "Stable" means relied on every boot; "experimental" means it
exists and runs but is gated or rough; "planned" means scoped, not built.

| Area | Status | Notes |
|---|---|---|
| Boot | **Stable** | GRUB Multiboot, legacy BIOS / CSM |
| Memory | **Stable** | PMM, 4-level paging, slab/heap, on-demand growth |
| Scheduler (single-core) | **Stable** | cooperative `SYS_YIELD`, fork+CoW, execve |
| Preemption | Experimental | gated `PREEMPT=1` |
| SMP | Experimental | AP bring-up + per-CPU data + LAPIC/IPIs + an offload path; gated `SMP=1`, **not** the default, not production-ready |
| PS/2 input | **Stable** | keyboard + trackpad (validated on T410) |
| Compositor desktop | **Stable** | windows, edge-snap, dock, animations |
| IDE + on-device `cc` | **Working** | single-file C/ASM → ELF, build + run |
| Filesystems | Partial | ramfs root; ext2/FAT32 read; `diskfs` durable blob |
| Networking (QEMU) | **Working** | e1000 + ARP/IP/ICMP/UDP/TCP/DNS/sockets |
| Networking (T410) | Gated off | Intel 82577LM PCH MMIO stall; default off |
| TLS | Partial | **TLS 1.2 client works**; TLS 1.3 *primitives* exist; 1.3 handshake planned |
| Browser | Experimental | DOM/HTML/CSS/layout + ES5-subset JS |
| USB mouse (T410) | In progress | `USB-EHCI-0` brick — routing ledger landed (E1/E2) |
| Durable projects | Planned | `persistfs` over `diskfs`, gated |

## Roadmap

**Now**
- `USB-EHCI-0` — EHCI host controller for the T410's EHCI-only PCH (routing-ledger gate, then a boot mouse)
- `IDE-PROJECT-0` — real project folders: create → build → run → desktop icon

**Next**
- Durable project folders (`persistfs`, T410-safe gated)
- USB HID mouse working end-to-end on the T410
- Framebuffer write-combining (PAT) to cut T410 paint latency

**Later**
- SMP service core (offload work to a second CPU)
- Asynchronous TCP
- Writable ext2 / a durable home
- Full TLS 1.3 handshake

The full roadmap is in [`docs/ROADMAP.md`](docs/ROADMAP.md); milestone specs live under
[`docs/bricks/`](docs/bricks/).

## How this project is built — "bricks"

Work lands as **bricks**: small, isolated, hard-gated milestones, each on its own branch with a
scoped design, explicit risks, acceptance tests, and merge criteria. New hardware support is
default-OFF behind a build flag until it's proven on real hardware. Examples:
[`USB-EHCI-0`](docs/bricks/USB-EHCI-0.md), [`IDE-PROJECT-0`](docs/bricks/IDE-PROJECT-0.md).

## Honesty notes

- **TLS:** the TLS *client* speaks **TLS 1.2** (enough for HTTPS). The crypto library is broad
  (SHA-1/256/512, MD5, HMAC, AES, ChaCha20-Poly1305, RSA, X25519, P-256, HKDF) and includes a
  TLS 1.3 HKDF-Expand-Label primitive, but a **full TLS 1.3 handshake is not implemented yet**.
- **SMP:** the SMP groundwork is real (AP startup, per-CPU data, LAPIC/IPIs, a stress-validated
  compute-offload path) but it is **gated off** — single-core cooperative is the validated
  default, and full multi-core scheduling is still experimental.
- **Smoke tests:** the boot smoke harness drives the ISO under QEMU. The **kernel → desktop path
  passes every run**; the non-passing checks are catalogued and known (a harness tag artifact,
  build-gated selftests, and experimental browser-wave apps), not silent failures. See
  [`docs/testing.md`](docs/testing.md).

## Documentation

- [Status](docs/status.md) · [Hardware](docs/hardware.md) · [Architecture](docs/architecture.md) · [Testing](docs/testing.md) · [Known limitations](docs/known-limitations.md)
- [Wiki](docs/wiki/Home.md) · [Roadmap](docs/ROADMAP.md) · [Brick specs](docs/bricks/)

## Credits

Created from scratch by **fourzerofour** & **Claude**, on the shoulders of the OSDev community's
collective knowledge and the GCC / NASM / GRUB / QEMU toolchains.

## License

MIT — see [LICENSE](LICENSE).
