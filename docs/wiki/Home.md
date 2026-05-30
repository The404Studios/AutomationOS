# AutomationOS Wiki

Welcome to the documentation for **AutomationOS** — a from-scratch x86_64 operating
system that boots to a Windows-like graphical desktop on **QEMU and real hardware**
(a 2010 Lenovo ThinkPad T410). Every layer is original code: the kernel, the
compositor, the web browser engine, and the C compiler.

![AutomationOS desktop](../../screenshots/desktop.png)

## Pages

| Page | What's inside |
|------|---------------|
| [Architecture](Architecture.md) | The big picture: boot → kernel → desktop, what's compiled, the directory map |
| [Kernel Internals](Kernel-Internals.md) | Memory, the **cooperative** scheduler, processes, system calls, interrupts |
| [Drivers & I/O](Drivers-and-IO.md) | Framebuffer, PS/2, PCI, storage/AHCI, filesystems, networking |
| [Networking & Security](Networking-and-Security.md) | The e1000 NIC, the ARP/IP/ICMP/UDP/TCP stack, sockets, DNS/HTTP/DHCP, and the hand-rolled crypto + TLS 1.2/HTTPS |
| [Browser & Web Engine](Browser-and-Web-Engine.md) | The from-scratch browser: DOM, HTML parser, CSS, layout, the ES5 JS engine, and the web APIs |
| [Desktop & Apps](Desktop-and-Apps.md) | The compositor, the window protocol, the app suite, the self-hosting toolchain |
| [Building & Running](Building-and-Running.md) | Toolchain, building the kernel + ISO, QEMU, flashing the T410 |
| [Roadmap](../ROADMAP.md) | What's done, in progress, and planned |

## What makes it interesting

- **It boots on real 2010 hardware**, not just an emulator — from USB, RAM-rooted.
- **Self-hosting**: an on-device C compiler turns C into ELF binaries on the machine itself.
- **The IDE is a forge**: start from a game/app/service template, **Ctrl+B** to compile, **Ctrl+R** to run.
- **From-scratch browser**: its own DOM, HTML/CSS/layout, a JavaScript engine, and a TLS/HTTPS stack.
- **A real desktop**: a compositor with a dock, window snap/maximize, a Windows-11 Start menu,
  a circular boot transition, and eased animations.

## Quick start

```sh
# under WSL Arch Linux
bash scripts/quick_build.sh    # build the kernel
bash scripts/build_all.sh      # build userspace + the bootable ISO
bash scripts/smoke_boot.sh     # 32-check boot test in QEMU
```

See [Building & Running](Building-and-Running.md) for QEMU options and flashing a USB for the T410.

## Honest scope

AutomationOS is a hobby OS and a work in progress. The scheduler is **cooperative**
(no preemption) and the system is **single-core** (SMP exists in-tree but isn't
compiled). It's GRUB Multiboot on legacy BIOS, not UEFI. The ThinkPad T410's
Wi-Fi/Ethernet (Intel 82577LM) isn't working on real hardware yet. See the
[Roadmap](../ROADMAP.md) for the full picture.

---
Created from scratch by **fourzerofour** & **Claude**.
