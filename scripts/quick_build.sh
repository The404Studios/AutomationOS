#!/bin/bash
cd "$(dirname "$0")/.."
mkdir -p build

CC=gcc
CFLAGS="-std=gnu11 -ffreestanding -nostdlib -nostdinc -fno-pic -fno-pie -fno-stack-protector -mno-red-zone -mcmodel=kernel -DSYSCALL_QUIET -DSCHEDULER_QUIET -DCONTEXT_SWITCH_QUIET -DEXEC_QUIET -DPROCESS_QUIET -Wno-unused-variable -Wno-unused-function -Wno-builtin-declaration-mismatch -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types -Ikernel/include -Ikernel/include/compat"

# Assembler flags. Empty by default so the cooperative build is byte-for-byte
# unchanged.
NASMFLAGS=""

# Kernel output. Default cooperative build writes build/kernel.elf (unchanged).
KERNEL_OUT="build/kernel.elf"

# =============================================================================
# OPT-IN PREEMPTIVE SCHEDULER (experimental). GATED behind the PREEMPT env var.
#   PREEMPT=1 bash scripts/quick_build.sh   ->  build/kernel-preempt.elf
# When PREEMPT is UNSET this whole block is skipped and the build behaves
# EXACTLY as before: cooperative scheduler, build/kernel.elf, no -DPREEMPTIVE.
# We add -DPREEMPTIVE to BOTH the nasm flags (so interrupt.asm + context_switch.asm
# assemble their %ifdef PREEMPTIVE blocks: irq0_preempt / context_save_irq /
# context_load_irq) AND the gcc CFLAGS (so scheduler.c compiles schedule_from_irq
# and idt.c points IDT[32] at irq0_preempt), and write to a SEPARATE output so
# the normal build/kernel.elf is never touched by a preemptive build.
# =============================================================================
if [ "${PREEMPT:-0}" = "1" ]; then
    CFLAGS="$CFLAGS -DPREEMPTIVE"
    NASMFLAGS="$NASMFLAGS -DPREEMPTIVE"
    KERNEL_OUT="build/kernel-preempt.elf"
    echo "*** PREEMPTIVE build: -DPREEMPTIVE enabled, output -> $KERNEL_OUT ***"
fi

GOOD=0
BAD=0
OBJS=""

compile() {
    local src="$1"
    local tag="$2"
    local obj="build/${tag}.o"
    if $CC $CFLAGS -c "$src" -o "$obj" 2>/tmp/gcc_err.txt; then
        echo "  OK: $src"
        GOOD=$((GOOD+1))
        OBJS="$OBJS $obj"
    else
        echo "FAIL: $src"
        cat /tmp/gcc_err.txt
        BAD=$((BAD+1))
    fi
}

assemble() {
    local src="$1"
    local tag="$2"
    local obj="build/${tag}.o"
    if nasm -f elf64 $NASMFLAGS "$src" -o "$obj" 2>/tmp/nasm_err.txt; then
        echo "  OK: $src"
        GOOD=$((GOOD+1))
        OBJS="$OBJS $obj"
    else
        echo "FAIL: $src"
        cat /tmp/nasm_err.txt
        BAD=$((BAD+1))
    fi
}

echo "=== AutomationOS Kernel Build ==="
echo ""

echo "[1/3] Assembling..."
assemble kernel/arch/x86_64/boot.asm         asm_boot
assemble kernel/arch/x86_64/gdt.asm          asm_gdt
assemble kernel/arch/x86_64/interrupt.asm    asm_interrupt
assemble kernel/arch/x86_64/syscall.asm      asm_syscall
assemble kernel/arch/x86_64/context_switch.asm asm_ctxswitch
assemble kernel/arch/x86_64/usermode.asm     asm_usermode

echo ""
echo "[2/3] Compiling kernel..."
compile kernel/kernel.c                      c_kernel
compile kernel/lib/string.c                  c_string
compile kernel/lib/printf.c                  c_printf
compile kernel/lib/panic.c                   c_panic
compile kernel/stubs.c                       c_stubs
compile kernel/arch/x86_64/gdt.c             c_gdt
compile kernel/arch/x86_64/idt.c             c_idt
compile kernel/arch/x86_64/paging.c          c_paging
# SMP brick 0: standalone READ-ONLY ACPI MADT CPU enumerator. Defines only the
# new symbol madt_count_cpus(); reuses acpi.h struct layouts (no symbols). Does
# NOT pull in either acpi.c or smp.c -- system stays single-core, this only logs
# "SMP: detected N cpus" so the kernel is AWARE of the core count.
compile kernel/arch/x86_64/madt.c            c_madt
compile kernel/drivers/serial.c              c_serial
compile kernel/drivers/pit.c                 c_pit
compile kernel/drivers/ps2.c                 c_ps2
compile kernel/drivers/rtc.c                 c_rtc
compile kernel/drivers/rng.c                 c_rng
compile kernel/drivers/framebuffer.c         c_framebuffer
compile kernel/drivers/core/irq.c            c_irq
compile kernel/drivers/pci.c                 c_pci
# NVIDIA GPU driver (detection + firmware-framebuffer foundation). SAFE to link:
# it keeps a single tiny static gpu snapshot (no large DMA arrays in .bss),
# probes the GPU read-only, and never programs the display. On QEMU (no 0x10DE
# device) nvidia_init() is a no-op. Wired into kernel.c by the dispatcher.
compile kernel/drivers/gpu/nvidia.c          c_nvidia
# AHCI/SATA read+write block driver (#13). Safe to link in: its DMA structures
# come from pmm_alloc_page() (identity-mapped, DMA-addressable), NOT from large
# static arrays, so it adds only a few hundred bytes of .bss and does NOT push
# __bss_end into the GRUB-placed initrd. On platforms with no AHCI controller
# (e.g. the default QEMU 'pc' machine) ahci_init() returns cleanly without
# touching MMIO, so a diskless boot is unaffected.
compile kernel/drivers/storage/block.c       c_block
compile kernel/drivers/storage/ahci.c        c_ahci
compile kernel/drivers/storage/ahci_block.c  c_ahci_block
# Networking: e1000 NIC + IPv4/ARP/ICMP stack + the SYS_NET_* helpers. SAFE to
# link in: the e1000 DMA rings/buffers come from pmm_alloc_page() (NOT static
# arrays), net.c keeps only a tiny ARP cache, and netsyscall.c uses stack
# buffers -- so the added .bss is a few hundred bytes, nowhere near the initrd.
compile kernel/drivers/net/e1000.c           c_e1000
compile kernel/drivers/net/rtl8139.c          c_rtl8139
compile kernel/net/net.c                      c_net
# IPv4 routing table (net.c/socket.c call route_init/route_lookup) -- was on disk
# but missing from the build list.
compile kernel/net/route.c                    c_route
compile kernel/net/netsyscall.c              c_netsyscall
# BSD-ish sockets (UDP + active-open TCP) on top of net.c. The ~338KB socket
# table now lives in kmalloc (see socket.c), NOT .bss, so these are safe to link.
compile kernel/net/socket.c                  c_socket
compile kernel/net/udp.c                      c_udp
compile kernel/net/tcp.c                      c_tcp
# NOTE: the staged HDA/e1000/NVMe/ACPI drivers remain intentionally NOT compiled
# in. Their large static DMA buffers bloat the kernel .bss past 0x1d8000 — where
# GRUB places the initrd (GRUB sizes free space from the kernel's FILE sections;
# .bss is NOBITS and invisible to it). The overlap let kernel .bss writes corrupt
# the initrd at runtime (spawns failing) and adjacent state (mouse breaking).
# Re-enable those only after the boot memory layout reserves the full .bss extent
# or relocates the initrd above it.
compile kernel/drivers/core/bus.c            c_bus
compile kernel/core/mem/pmm.c                c_pmm
compile kernel/core/mem/vmm.c                c_vmm
compile kernel/core/mem/cow.c                c_cow
compile kernel/core/mem/slab.c               c_slab
compile kernel/core/mem/heap.c               c_heap
compile kernel/core/mem/vma.c                c_vma
compile kernel/core/mem/vma_region.c         c_vma_region
compile kernel/core/procapi/procapi.c        c_procapi
compile kernel/ipc/clipboard.c               c_clipboard
compile kernel/ipc/notify.c                  c_notify
compile kernel/core/sched/scheduler.c        c_scheduler
compile kernel/core/sched/process.c          c_process
compile kernel/core/sched/context.c          c_context
compile kernel/core/sched/waitqueue.c        c_waitqueue
compile kernel/core/syscall/handlers.c       c_syscall_handlers
compile kernel/core/syscall/syscall.c        c_syscall
compile kernel/arch/x86_64/syscall_init.c   c_syscall_init
compile kernel/fs/vfs.c                      c_vfs
compile kernel/fs/vfs_dir.c                  c_vfs_dir
# Filesystem registry + ext2/fat32 drivers (added by the FS integration work).
# vfs.c calls fs_registry_*; kernel.c calls ext2_init/fat32_init -- these source
# files were on disk but missing from the build list, breaking the link.
compile kernel/fs/fs_registry.c              c_fs_registry
compile kernel/fs/ext2.c                     c_ext2
compile kernel/fs/fat32.c                    c_fat32
# diskfs: durable superblock over AHCI (persistence #57). Self-contained, only
# touches a fixed LBA via ahci_read/write; no-op when no disk is attached.
compile kernel/fs/diskfs.c                   c_diskfs
compile kernel/init/initrd.c                 c_initrd
compile kernel/fs/elf_loader.c               c_elf_loader
compile kernel/fs/exec.c                     c_exec
compile kernel/core/usermode.c               c_usermode
compile kernel/security/namespace.c          c_namespace
compile kernel/ipc/shm.c                      c_shm
compile kernel/ipc/msgqueue.c                 c_msgqueue
compile kernel/drivers/input/input.c          c_input
compile kernel/drivers/input/evdev.c          c_evdev
compile kernel/drivers/input/dev_input.c      c_dev_input
compile kernel/core/signal/kill.c             c_kill
compile kernel/core/sched/nice.c              c_nice
compile kernel/drivers/pty/pty.c              c_pty
compile kernel/drivers/pty/pty_dev.c          c_pty_dev
# Advanced subsystems added by the overhaul agents (wired into the build here):
# perf counters, page cache, VMA red-black tree (vma_add), and the io_uring-style
# batch / epoll / futex / sendfile / vma_test syscalls. syscall.c, vfs.c and
# exec.c call into these, so they must be linked.
compile kernel/lib/perf.c                     c_perf
# Single-CPU TLB (tlb.c is the SMP/IPI version, deferred until multi-core).
compile kernel/arch/x86_64/tlb_uni.c          c_tlb_uni
compile kernel/fs/page_cache.c                c_page_cache
compile kernel/core/mem/vma_rbtree.c          c_vma_rbtree
compile kernel/core/syscall/futex.c           c_futex
compile kernel/core/syscall/sendfile.c        c_sendfile
compile kernel/core/syscall/epoll.c           c_epoll
compile kernel/core/syscall/batch.c           c_batch
compile kernel/core/syscall/vma_test.c        c_vma_test

echo ""
echo "[3/3] Linking (strict: undefined symbols are fatal)..."
if ld -T kernel/linker.ld -nostdlib $OBJS -o "$KERNEL_OUT" 2>/tmp/ld_err.txt; then
    echo "  Link OK -- no unresolved symbols"
else
    echo "  LINK FAILED:"
    cat /tmp/ld_err.txt
fi

echo ""
echo "=== Results: $GOOD compiled, $BAD failed ==="
if [ -f "$KERNEL_OUT" ]; then
    echo ""
    echo "========================================="
    echo "  SUCCESS: $KERNEL_OUT ($(stat -c%s "$KERNEL_OUT") bytes)"
    echo "========================================="
else
    echo "FAILED: No $KERNEL_OUT produced"
fi
