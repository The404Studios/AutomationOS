# repo_map — where things live (retrieval index seed)

Compact map for "what file defines X?". Not exhaustive; refresh per milestone.

## Kernel (`kernel/`)
- `kernel/kernel.c` — boot/init orchestration.
- `kernel/core/syscall/` — `syscall.c` (dispatch table + `syscall_init`), `handlers.c` (most
  `sys_*`), `futex.c`. Syscall numbers: `kernel/include/syscall.h` (used 0–100, 200; free 101–199).
- `kernel/core/sched/` — `process.c` (PCB alloc/teardown, `process_create`/`process_unref`),
  `scheduler.c`, `waitqueue.c` (`wait_object_block`/`wo_park_and_cleanup`). PCB struct: `process_t`
  in `kernel/include/sched.h` (has `fd_table[1024]`, the CHANNEL-0 `ch_handles[32]`, `stdio_chan[3]`).
- `kernel/core/mem/` — `pmm.c` (`pmm_alloc_page[s]`), `heap.c` (`kmalloc`/`kfree`), `slab.c`.
  API in `kernel/include/mem.h`. Identity-mapped 0–16 GB (phys == usable ptr; MMIO BARs too).
- `kernel/ipc/` — `shm.c`, `msgqueue.c`, `notify.c`, `clipboard.c`, **`channel.c` (CHANNEL-0)**.
- `kernel/fs/` — `vfs.c`, `exec.c` (`elf_load_and_exec`), `ext2.c`, `fat32.c`, `diskfs.c`, ramfs.
- `kernel/drivers/` — `net/e1000.c`, `usb/{uhci,ehci?,hid,usb_core}.c`, `pty/{pty,pty_dev}.c`,
  `ps2.c`, `pit.c`, `rtc.c`, `pci.c`, `storage/ahci.c`, audio, input.
- `kernel/crypto/`, `kernel/net/` (ARP/IP/ICMP/UDP/TCP/DNS/sockets/TLS 1.2).
- `kernel/include/` — headers; `kernel/include/compat/` — freestanding shims (note: `errno.h` is
  **negative**, `compat/errno.h` is positive; kernel build uses the negative one).

## Userspace (`userspace/`)
- `userspace/init/main.c` — spawns the compositor.
- `userspace/compositor/compositor_m8.c` — the live compositor (windows, dock, desktop icons,
  panel/clock/battery). `userspace/lib/wl/` — the wl client lib + `userspace/include/wl_proto.h`.
- `userspace/apps/terminal/terminal_m3.c` — **the live terminal** (+ `sh_git.c`); ships as
  `/sbin/terminal`. Siblings (`terminal.c`, `vt_parser.c`, `renderer.c`, …) and `userspace/terminal/*`,
  `userspace/shell/*` are DEAD duplicates.
- `userspace/apps/cc/` + `userspace/apps/ide/` — the on-device C→ELF compiler + IDE (the forge).
- `userspace/lib/` — `syscall.c`/`syscall.h` (the `syscall()` wrapper), `epoll.h`, font, keymap, net.
- `/bin` tools installed by `build_all.sh` (sed/awk/grep/cc/wget/…); `/sbin` apps.

## Build / scripts (`scripts/`, `build_test/`)
- `scripts/quick_build.sh` — kernel only -> `build/kernel.elf` (gates: `T410_SAFE`, `SCHED_DEBUG`,
  `USB_UHCI`, `EHCI_USB`, `DISK_PERSIST`, `PCH_NIC`, `PREEMPT`, `SMP`). `compile <src> <obj>` helper.
- `scripts/build_all.sh` — userspace + initrd + ISO (copies the existing kernel.elf).
- `scripts/smoke_boot.sh` — boot the ISO under QEMU, assert invariants. `build_test/*.sh` —
  per-brick verifiers + `shot.sh` (QMP screendump).

## Specs / memory
- `docs/superpowers/specs/` — brick design specs. `docs/bricks/` — brick summaries.
- `docs/dev-memory/` — this structured-data scaffold (laws, images, failures, brick records).
