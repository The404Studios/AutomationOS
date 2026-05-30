# Architecture

AutomationOS is a from-scratch x86_64 operating system that boots through GRUB into a 64-bit long-mode kernel and lands on a Windows-like graphical desktop — the same image runs under QEMU and on a physical 2010 Lenovo ThinkPad T410. This page is the high-level map: how the layers stack, how a boot actually proceeds, and where every piece lives in the tree.

> Scope note: this document describes what the code *does today*. The scheduler is **cooperative** (processes yield via `SYS_YIELD`; there is no timer preemption), the system is **single-core** (SMP source exists but is not compiled), and boot is **GRUB Multiboot1 on legacy BIOS** (not UEFI). See [Roadmap](../ROADMAP.md) for what is planned.

---

## The layering

From firmware up to the apps, each layer hands off to the next:

```
Legacy BIOS / firmware
        │  (El Torito ISO, GRUB stage built by grub-mkrescue)
        ▼
GRUB 2  ──  Multiboot1 loader   iso/boot/grub/grub.cfg
        │   loads kernel.elf + initrd.img as module 0
        ▼
boot.asm  ──  32-bit entry → long mode    kernel/arch/x86_64/boot.asm
        │   CPUID checks, PAE, page tables, EFER.LME, paging on,
        │   initrd-in-.bss rescue, .bss clear, call kernel_main
        ▼
kernel_main(C)  ──  subsystem bring-up     kernel/kernel.c
        │   GDT/IDT, PMM/VMM/heap, framebuffer+splash, drivers,
        │   VFS, mount initrd, load /sbin/init, scheduler_start()
        ▼
/sbin/init (PID 1, ring 3)   userspace/init/main.c
        │   SYS_SPAWN the compositor + apps; reap children
        ▼
/sbin/compositor (ring 3)    userspace/compositor/compositor_m8.c
        │   owns the framebuffer; draws panel, dock, wallpaper;
        │   composites client windows over SHM; forwards input
        ▼
GUI apps (ring 3)            userspace/apps/*
            terminal, filemanager, editor, browser2, ide, games…
```

Everything above `boot.asm` runs in 64-bit long mode. Everything from `init` down runs in **ring 3** — the kernel reaches userspace exactly once (loading PID 1) and thereafter serves it through the syscall interface.

---

## Boot sequence (with on-screen `[NN]` markers)

On real hardware the serial `kprintf` log is invisible, so `kernel_main` paints numbered progress markers straight to the framebuffer via `boot_mark()` (`kernel/kernel.c`). Each call stacks one green `[NN] label` line under the boot splash; the **last marker on screen** pinpoints exactly which init step froze if a boot ever hangs. The compositor harmlessly paints over them once the desktop comes up.

The sequence (`kernel_main`, `kernel/kernel.c`):

1. `serial_init()` + banner.
2. `parse_multiboot()` — read the GRUB info struct: memory map, initrd module, framebuffer geometry.
3. `gdt_init()`, `idt_init()`.
4. `pmm_reserve_initrd()` **before** PMM init, so the page allocator never hands out the initrd's pages.
5. `pmm_init()` → `vmm_init()` → `tlb_init()` → `pmm_add_remaining_pages()` → `heap_init()` → `cow_init()`.
6. Self-tests/benchmarks: heap, slab.
7. `pci_init()` — enumerate the PCI bus.
8. Framebuffer: map it (kernel + a userspace alias at `0x40000000`), `framebuffer_init()`, draw the **"Welcome to AutomationOS"** splash. From here on `boot_mark()` is live.
9. `[NN] timer (PIT)` — `pit_init(1000)`. **The PIT is armed as a monotonic 1000 Hz tick counter only.** IRQ0 increments a counter and never reschedules, which is what keeps scheduling cooperative; it gives userspace a real time source via `SYS_GET_TICKS_MS`.
10. `[NN] network (e1000)` — `net_init()` (NIC detect, ARP). Bounded spins so a T410 with no link bails fast. `sock_init()`, `rtc_init()`, `rng_init()`, clipboard/notify.
11. `[NN] vfs` → `[NN] fs drivers (ext2/fat32)` → `[NN] mount root (ramfs)`; create `/mnt`, `/home`, `/dev`.
12. `[NN] input subsystem` → `[NN] keyboard/mouse (PS/2)` → `[NN] pty`.
13. `process_init()`, `scheduler_init()`, `perf_init()`.
14. Load the initrd (`initrd_init` / `initrd_mount`), find `sbin/init`, `elf_load_and_exec()` it as PID 1.
15. `tss_init()`, `syscall_init()`, `shm_init()`/`msg_init()`, `syscall_msr_init()` (SYSCALL/SYSRET MSRs).
16. `[NN] starting services (scheduler)` — `scheduler_start()` enables interrupts and switches into PID 1. Control never returns; `kernel_main` would otherwise fall into an `hlt` idle loop.

### Safe-boot gate

`kernel.c` defines `T410_SAFE_BOOT` (currently on). Under it the kernel **skips the NVIDIA GPU register probe and the AHCI/SATA + diskfs init** — both poke real device MMIO that QEMU never exercises and were prime suspects for a post-splash hang. A RAM-rooted boot reaches the desktop without either; you'll see `[NN] gpu SKIPPED (safe boot)` / `[NN] storage SKIPPED (safe boot)` markers. Networking stays enabled because its spins are hard-bounded.

---

## The T410 initrd-in-`.bss` rescue

This is the keystone fix that made real-hardware boot work, and it lives in **`kernel/arch/x86_64/boot.asm`** (the `[BITS 64] long_mode_start` block, label `.initrd_done`).

The problem: GRUB loads our initrd (Multiboot module 0) immediately after the kernel image. On the T410's fragmented E820 memory map that landing spot falls **inside** `[__bss_start, __bss_end]`. The kernel's `.bss` is a `NOBITS` section — it occupies no bytes in the ELF file, so GRUB sizes the post-kernel free space from the *file* sections and can't see it. The `rep stosq` that zeroes `.bss` during early boot would therefore wipe the compositor/init binary, and userspace spawn fails the instant the splash clears.

The fix: **before** clearing `.bss`, `boot.asm` reads module 0's `mod_start`/`mod_end` out of the Multiboot module struct, `rep movsb`-copies the initrd up to **16 MiB** (above `__bss_end`, which sits near ~4 MiB, and above the source), then rewrites `mod_start`/`mod_end` to point at the relocated copy. `kernel.c` then reads the safe copy transparently. It is a no-op in QEMU (where the module already lands outside `.bss`) and when no module is present.

The companion layout discipline lives in `kernel/linker.ld`: `.pagetables` is its own `NOBITS` section so the `.bss` clear can't destroy the early page tables, and large non-control-path buffers are emitted last (`.bss.deferred`) to keep critical kernel state packed below the `0x200000` userspace load base.

---

## What is actually compiled

The tree contains far more source than ships in the binary — staged drivers, alternative compositor milestones (`compositor_m2..m7`), SMP code, and experiments. **The build scripts are the ground truth for what is real.**

### Kernel — `scripts/quick_build.sh`

This script *is* the kernel source manifest. It assembles six `.asm` files and compiles the C kernel one file at a time, then links with `ld -T kernel/linker.ld` under a strict policy (undefined symbols are fatal). Compiled in:

- **Arch/boot**: `boot.asm`, `gdt.asm`, `interrupt.asm`, `syscall.asm`, `context_switch.asm`, `usermode.asm`; `gdt.c`, `idt.c`, `paging.c`, `syscall_init.c`, **`tlb_uni.c`** (the *single-CPU* TLB).
- **Memory**: `pmm`, `vmm`, `cow`, `slab`, `heap`, `vma`, `vma_region`, `vma_rbtree`.
- **Sched/IPC/signal**: `scheduler`, `process`, `context`, `waitqueue`, `nice`, `kill`; `clipboard`, `notify`, `shm`, `msgqueue`.
- **Syscalls**: `handlers`, `syscall`, plus `futex`, `sendfile`, `epoll`, `batch`, `vma_test`.
- **FS**: `vfs`, `vfs_dir`, `fs_registry`, `ext2`, `fat32`, `diskfs`, `page_cache`, `initrd`, `elf_loader`, `exec`.
- **Drivers**: `serial`, `pit`, `ps2`, `rtc`, `rng`, `framebuffer`, `core/irq`, `pci`, `gpu/nvidia`, `storage/{block,ahci,ahci_block}`, `net/{e1000,rtl8139}`, `input/{input,evdev,dev_input}`, `pty/{pty,pty_dev}`, `core/bus`.
- **Misc**: `lib/{string,printf,panic,perf}`, `stubs.c`, `core/procapi`, `core/usermode`, `security/namespace`, `net/{net,route,netsyscall,socket,udp,tcp}`.

**Explicitly NOT compiled** (per the comments in `quick_build.sh`): the staged HDA audio / NVMe / ACPI drivers, whose large static DMA buffers would push the kernel's `.bss` past where GRUB drops the initrd and corrupt it at runtime. And **`tlb.c` (the SMP/IPI TLB shootdown) is deferred** in favor of `tlb_uni.c` — the SMP files (`smp.c`, `lapic.c`, `ap_trampoline.asm`, `ipi*.c`) exist under `kernel/arch/x86_64/` but **none of them appear in the build**, so the system runs single-core.

### Userspace + ISO — `scripts/build_all.sh`

Builds the shared libs, the compositor (`compositor_m8.c` → `/sbin/compositor`), `init` (`userspace/init/main.c` → `/sbin/init`), and the full app/tool suite (each compiled freestanding, `-mstackrealign`, linked with `userspace/userspace.ld`). It then unpacks the existing initrd, copies the fresh ELFs into `/sbin` and `/bin`, runs a `fs:0x28` stack-canary check on every binary, re-tars `iso/boot/initrd.img`, copies `build/kernel.elf` into the ISO tree, and runs `grub-mkrescue` to emit `build/automationos.iso`.

The boot contract that ties them: `grub.cfg` does `multiboot /boot/kernel.elf` + `module /boot/initrd.img`; `init` (PID 1) `SYS_SPAWN`s `sbin/compositor` first, then the terminal, file manager, and a long list of apps and self-test probes; if the compositor ever exits, `init` restarts it.

---

## Directory map

| Path | What lives here |
|------|-----------------|
| `kernel/` | The kernel. `kernel.c` (`kernel_main`), `linker.ld`, `stubs.c`. |
| `kernel/arch/x86_64/` | Long-mode entry (`boot.asm`), GDT/IDT, paging, syscall/context-switch/usermode asm+C, the single-CPU `tlb_uni.c` (and the un-compiled SMP files). |
| `kernel/core/` | `mem/` (pmm, vmm, heap, slab, cow, vma), `sched/` (scheduler, process, context), `syscall/` (dispatch + futex/epoll/sendfile/batch), `signal/`, `procapi/`. |
| `kernel/drivers/` | `serial`, `pit`, `ps2`, `rtc`, `rng`, `framebuffer`, `pci`, `gpu/`, `storage/` (AHCI), `net/` (e1000/rtl8139), `input/`, `pty/`. |
| `kernel/fs/` | VFS + `ext2`, `fat32`, `diskfs`, `page_cache`, ELF loader, `exec`. |
| `kernel/net/` | IPv4 stack: `net`, `route`, `socket`, `udp`, `tcp`, `netsyscall`. |
| `kernel/ipc/`, `kernel/security/`, `kernel/init/` | Shared memory / message queues / clipboard / notify; namespaces; `initrd.c`. |
| `kernel/include/` | Kernel headers (`kernel.h`, `sched.h`, `syscall.h`, `x86_64.h`, …). |
| `userspace/` | Userland. `init/` (PID 1), `compositor/` (`compositor_m8.c` is the live one), `crt0.asm`, `userspace.ld`. |
| `userspace/lib/` | Shared libs: `wl` (compositor client), `ui`, `font`/`bitfont`, `crypto`, `tls`, `net`, `js`, `dom`/`html`/`css`/`layout`, `imgcodec`, `game`. |
| `userspace/apps/` | GUI apps + CLI tools: terminal, filemanager, editor, `browser`/`browser2`, `ide`, `cc` (on-device C compiler), games, coreutils, and the boot self-test probes. |
| `boot/` | A separate experimental bootloader (`boot.asm`, `loader.c`, …). **Not** the active boot path — the shipped image boots via GRUB; the active long-mode entry is `kernel/arch/x86_64/boot.asm`. |
| `scripts/` | Build + run + test scripts. `quick_build.sh` (kernel), `build_all.sh` (userspace + ISO), `run-qemu.*`, `smoke*.sh`. |
| `docs/` | Documentation, including `docs/wiki/` (this wiki) and `docs/ROADMAP.md`. |
| `iso/` | The GRUB ISO staging tree: `iso/boot/grub/grub.cfg`, `kernel.elf`, `initrd.img`. |

---

## See also

- [Home](Home.md) — wiki landing page
- [Kernel Internals](Kernel-Internals.md) — memory, scheduler, syscalls, the cooperative model
- [Drivers & I/O](Drivers-and-IO.md) — PS/2, framebuffer, PCI, storage, networking
- [Desktop & Apps](Desktop-and-Apps.md) — the compositor, the SHM window protocol, the app suite
- [Building & Running](Building-and-Running.md) — `quick_build.sh`, `build_all.sh`, QEMU + T410
- [Roadmap](../ROADMAP.md) — what's done, in progress, and planned
