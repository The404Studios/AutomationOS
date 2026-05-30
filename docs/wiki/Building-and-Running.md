# Building & Running

AutomationOS builds under **WSL Arch Linux** (or any Arch/Linux with the toolchain).
This page covers the toolchain, building the kernel and the bootable ISO, running in
QEMU, and flashing a USB stick to boot it on a real ThinkPad T410.

## Toolchain

You need `gcc` (the host x86_64 gcc, used freestanding), `nasm`, `ld`,
`grub-mkrescue` (+ `xorriso`/`mtools`), and `qemu-system-x86_64`. On Arch:

```sh
sudo pacman -S base-devel nasm grub xorriso mtools qemu-system-x86
```

The kernel and userspace are **freestanding** — no system libc. Userspace programs
use the in-tree `userspace/libc` + `userspace/lib`. Build flags (see the `cc()`
helper in `scripts/build_all.sh`): `-ffreestanding -nostdlib -fno-builtin
-fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2`.

## Building

```sh
bash scripts/quick_build.sh   # kernel only -> build/kernel.elf
bash scripts/build_all.sh     # userspace apps + initrd + the GRUB ISO
```

- `quick_build.sh` compiles the kernel from a hardcoded source list and links it with
  `kernel/linker.ld` into `build/kernel.elf` (strict — undefined symbols are fatal).
- `build_all.sh` compiles every userspace app, packs them into the initrd, installs the
  freshly-built kernel into the ISO tree, and runs `grub-mkrescue` to produce
  `build/automationos.iso`. It does **not** rebuild the kernel — run `quick_build.sh`
  first if you changed kernel code.

## Running in QEMU

```sh
qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev user,id=n0 -device e1000,netdev=n0
```

QEMU's emulated NIC is the Intel 82540EM (`-device e1000`), which the driver fully
supports — ARP/DNS/TCP/HTTPS all work in QEMU.

### The smoke test

```sh
bash scripts/smoke_boot.sh    # boots the ISO headless, asserts 32 checks
```

It boots diskless with the serial console to a log and verifies the kernel reaches the
desktop, spawns processes, brings up networking, renders the browser, etc. A green run
is **32/32** — the project's gate.

## Flashing the ThinkPad T410 (real hardware)

1. Build the ISO (above) — it's a hybrid ISO, so it's USB-bootable as-is.
2. Write it to a USB stick with **Rufus in DD mode** (Windows) or `dd` (Linux):
   ```sh
   sudo dd if=build/automationos.iso of=/dev/sdX bs=4M status=progress conv=fsync
   ```
   Replace `/dev/sdX` with your USB device — **double-check it**, `dd` is unforgiving.
3. Boot the T410 from USB (legacy/BIOS boot). It's **RAM-rooted** — no internal disk needed.

The T410 has no serial console, so on-screen green **`[NN]` boot markers** trace each
subsystem as it loads; if it ever hangs, the last marker tells you exactly where. See
[Architecture](Architecture.md) for the boot sequence and the initrd-rescue fix that
made real-hardware boot reliable.

## Toolchain gotchas

- This WSL Arch `gcc` re-injects a stack canary (`fs:0x28`) into some shared-library
  objects even with `-fno-stack-protector`. The OS tolerates it (`ld -static` resolves
  the reference and it never trips), so a *linked* ELF showing `fs:0x28` is **not** a
  bug — verify the app's *own* object is canary-free.
- AutomationOS syscall numbers are **not** Linux's. Notably `SYS_WRITE = 3`, and syscall
  `1` is `SYS_FORK` — so an inline `write` using Linux's `rax=1` **forks** instead of
  writing. See [Kernel Internals](Kernel-Internals.md).

## See also

[Home](Home.md) · [Architecture](Architecture.md) · [Kernel Internals](Kernel-Internals.md) · [Drivers & I/O](Drivers-and-IO.md) · [Desktop & Apps](Desktop-and-Apps.md) · [Roadmap](../ROADMAP.md)
