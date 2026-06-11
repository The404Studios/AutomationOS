#!/bin/bash
cd "$(dirname "$0")/.."
mkdir -p build

CC=gcc
CFLAGS="-std=gnu11 -ffreestanding -nostdlib -nostdinc -fno-pic -fno-pie -fno-stack-protector -mno-red-zone -mcmodel=kernel -DBOOT_QUIET -DSYSCALL_QUIET -DSCHEDULER_QUIET -DCONTEXT_SWITCH_QUIET -DEXEC_QUIET -DPROCESS_QUIET -Wno-unused-variable -Wno-unused-function -Wno-builtin-declaration-mismatch -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types -Ikernel/include -Ikernel/include/compat"

# SCHED_DEBUG: on-screen scheduler diagnostics -- the yellow boot markers PLUS
# the timer-ISR liveness heartbeat (pit.c: a 48x48 colour-cycling square at
# (1180,12) and the CURRENT PROCESS NAME in 2x text at (744,14), drawn straight
# onto the framebuffer over the desktop). DEFAULT OFF (IDE-REPAIR-0 I0): users
# saw "sbin/compositor" + a flashing square painted over the desktop on the
# T410. Set SCHED_DEBUG=1 to opt back in when debugging scheduler hangs.
if [ "${SCHED_DEBUG:-0}" = "1" ]; then
    CFLAGS="$CFLAGS -DSCHED_DEBUG"
    echo "*** SCHED_DEBUG build: on-screen scheduler diagnostics ENABLED ***"
fi

# ─────────────────────────────────────────────────────────────────────────────
# T410-SAFE PROFILE (GATED behind the T410_SAFE env var).
#   T410_SAFE=1 SCHED_DEBUG=0 bash scripts/quick_build.sh
# A boring-correctness profile for the 2010 Westmere i5-520M (ThinkPad T410):
# no modern-CPU optimizations are armed regardless of CPUID. Specifically
# -DT410_SAFE compiles OUT the ERMS REP-string fast path in paging.c (so the
# portable, DF-safe word-copy loop is always used). Combine with SCHED_DEBUG=0
# for a clean boot. The default build is byte-for-byte unchanged when unset.
# ─────────────────────────────────────────────────────────────────────────────
if [ "${T410_SAFE:-0}" = "1" ]; then
    CFLAGS="$CFLAGS -DT410_SAFE"
    echo "*** T410_SAFE build: modern-CPU optimizations disabled (Westmere-safe) ***"
fi

# ─────────────────────────────────────────────────────────────────────────────
# OPT-IN: 82577LM-class PCH NIC bring-up (the ThinkPad T410 onboard NIC).
#   PCH_NIC=1 bash scripts/quick_build.sh
# DEFAULT OFF. The PCH PHY shares an internal MDIO bus with the Management
# Engine; bringing it up can HARDWARE-STALL the T410 at boot (a bus stall, not a
# software spin). Enable ONLY to validate networking on the real device with a
# serial console attached. QEMU is unaffected either way (its NIC is not a PCH
# part). When unset, e1000.c declines the PCH NIC so the system always boots.
# ─────────────────────────────────────────────────────────────────────────────
if [ "${PCH_NIC:-0}" = "1" ]; then
    CFLAGS="$CFLAGS -DE1000_PCH_NIC"
    echo "*** PCH_NIC build: 82577LM PCH NIC bring-up ENABLED (T410 boot risk) ***"
fi

# ─────────────────────────────────────────────────────────────────────────────
# OPT-IN: the NET-P1-A0 in-kernel network test rig.
#   NET_SELFTEST=1 bash scripts/quick_build.sh
# DEFAULT OFF. Compiles kernel/net/net_testrig.c (a capturing ip_tx tap + raw
# IPv4 injection into ipv4_demux) and runs the NETRIG boot selftest after
# sock_init(). With it unset the rig TU compiles EMPTY and ip_tx carries no
# tap, so default kernel behavior is unchanged. The rig is how every NET-P1
# brick (SYN side-table, OOO reassembly, persist probes) proves itself
# deterministically -- no slirp timing, no NIC, no hardware.
# ─────────────────────────────────────────────────────────────────────────────
if [ "${NET_SELFTEST:-0}" = "1" ]; then
    CFLAGS="$CFLAGS -DNET_SELFTEST"
    echo "*** NET_SELFTEST build: in-kernel net test rig (NETRIG) ENABLED ***"
fi

# ─────────────────────────────────────────────────────────────────────────────
# OPT-IN: durable on-disk persistence (AHCI/SATA + diskfs at boot).
#   DISK_PERSIST=1 bash scripts/quick_build.sh
# DEFAULT OFF. ahci_init() pokes real SATA-controller MMIO at boot, a documented
# T410 post-splash-hang suspect, so it stays off for the safe RAM-rooted boot.
# Enabling it makes the IDE's settings (SYS_PERSIST_READ/WRITE -> diskfs) and any
# diskfs file SURVIVE A REBOOT on a present disk. QEMU's ICH9 AHCI works fine, so
# this is how scripts/smoke_persist.sh proves cross-reboot durability. With it off,
# the persist syscalls return ENODEV and callers fall back to a session file.
# ─────────────────────────────────────────────────────────────────────────────
if [ "${DISK_PERSIST:-0}" = "1" ]; then
    CFLAGS="$CFLAGS -DDISK_PERSIST"
    echo "*** DISK_PERSIST build: AHCI/SATA + diskfs durable persistence ENABLED ***"
fi

# ─────────────────────────────────────────────────────────────────────────────
# OPT-IN USB HID boot mouse (USB-MOUSE-0). GATED behind USB_UHCI=1. DEFAULT OFF.
#   USB_UHCI=1 bash scripts/quick_build.sh
# Compiles the dormant usb_core/uhci/hid sources (link-tested) and defines
# -DUSB_UHCI so kernel.c can #ifdef-gate the (separate, later) usb_init() call.
# When UNSET the USB sources are NOT compiled and no -DUSB_UHCI is defined, so the
# default kernel is byte-for-byte unchanged and never touches USB hardware at boot
# -- the T410-safe default. USB init must pass the loop-correctness laws (bounded
# controller/transfer waits) before it is ever called on real hardware.
# ─────────────────────────────────────────────────────────────────────────────
if [ "${USB_UHCI:-0}" = "1" ]; then
    CFLAGS="$CFLAGS -DUSB_UHCI"
    echo "*** USB_UHCI build: usb_core+uhci+hid compiled (boot init still gated in kernel.c) ***"
fi

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

# =============================================================================
# OPT-IN SMP FOUNDATION (brick 1: BSP Local APIC bring-up). GATED behind the SMP
#   env var.   SMP=1 bash scripts/quick_build.sh   ->  build/kernel-smp.elf
# When SMP is UNSET this whole block is skipped and the build behaves EXACTLY as
# before: NO -DSMP_FOUNDATION macro, kernel/arch/x86_64/lapic.c is NOT compiled,
# output stays build/kernel.elf -- byte-for-byte the default kernel.
#
# We define the FRESH macro -DSMP_FOUNDATION (NOT -DCONFIG_SMP, which would
# #ifndef-out the cooperative scheduler and brick the kernel; NOT -DSMP_ENABLE,
# the stale smp.c gate). kernel.c calls lapic_init() under #ifdef SMP_FOUNDATION,
# right after the brick-0 "[SMP] detected" line, to bring the BSP local APIC
# online (safe-enable + read APIC ID/version only -- the PIC/IOAPIC/timer/IDT are
# left untouched). The lapic.c source is added to the compile list ONLY here, and
# we write a SEPARATE output so the normal build/kernel.elf is never touched.
# =============================================================================
SMP_SOURCES=""
SMP_IPI_SOURCES=""
SMP_BKL_SOURCES=""
if [ "${SMP:-0}" = "1" ]; then
    CFLAGS="$CFLAGS -DSMP_FOUNDATION"
    KERNEL_OUT="build/kernel-smp.elf"
    SMP_SOURCES="1"
    echo "*** SMP build: -DSMP_FOUNDATION enabled, +lapic.c +ap_boot.c +ap_trampoline.asm, output -> $KERNEL_OUT ***"

    # TEST-ONLY knob (brick 3 safe-degradation proof). When SMP_FORCE_AP_FAIL=1 is
    # ALSO set, ap_boot.c's try_start_cpu1() aims the SIPI at a bogus APIC id (0xFE)
    # that no CPU answers, so CPU 1 never sets its online flag and the BOUNDED
    # ~100ms TSC wait must expire -> the BSP logs "AP failed to start, continuing
    # single-core" and keeps booting. This PROVES the hard rule (AP failure never
    # hangs/panics the BSP). It is OFF by default and has NO effect on a normal SMP
    # build (only -DSMP_FOUNDATION is defined). Never set this for a shipping image.
    if [ "${SMP_FORCE_AP_FAIL:-0}" = "1" ]; then
        CFLAGS="$CFLAGS -DSMP_FORCE_AP_FAIL"
        echo "*** SMP_FORCE_AP_FAIL=1: AP start will be forced to FAIL (degradation test) ***"
    fi

    # =========================================================================
    # SMP SCHEDULER master sub-gate (real per-CPU scheduling: CPU1 runs actual
    # processes). GATED behind SMP_SCHED=1, which REQUIRES SMP=1 (it lives inside
    # this block). When SMP_SCHED is unset, the build is today's coprocessor
    # kernel EXACTLY (cpu1_submit/cpu1_wait, cpu_id()==0 from stubs.c) -- a clean
    # rollback. When set, -DSMP_SCHED is added on top of -DSMP_FOUNDATION and
    # EVERY scheduler change is wrapped #ifdef SMP_SCHED:
    #   - ap_boot.c defines a REAL cpu_id() (xAPIC id -> logical id), so stubs.c's
    #     cpu_id() #ifndef-s out (no duplicate symbol).
    # Build:  SMP=1 SMP_SCHED=1 bash scripts/quick_build.sh  -> build/kernel-smp.elf
    # =========================================================================
    if [ "${SMP_SCHED:-0}" = "1" ]; then
        CFLAGS="$CFLAGS -DSMP_SCHED"
        # syscall.asm's %ifdef SMP_SCHED path (swapgs + GS-base per-CPU kernel RSP,
        # Brick C) needs the macro at ASSEMBLE time too -- nasm doesn't see CFLAGS.
        NASMFLAGS="$NASMFLAGS -DSMP_SCHED"
        echo "*** SMP_SCHED build: real per-CPU scheduling enabled (-DSMP_SCHED, gcc+nasm) ***"

        # =====================================================================
        # SMP_SCHED_DISPATCH (Brick F) -- BRUTALLY gated 2nd sub-gate, REQUIRES
        # SMP_SCHED. When set, CPU1 switches from COPROCESSOR mode (the cpu1_job
        # worker loop servicing matmul offload) to SCHEDULER mode: ap_main enters
        # ap_scheduler_loop() and CPU1's LAPIC timer drives ap_schedule_from_irq()
        # to context-switch CPU1 between its own runqueue processes. This is the
        # triple-fault frontier (AP ring-3 dispatch), built sub-brick by sub-brick
        # (F1 skeleton -> F2 kernel thread -> F3 ring-3 -> F4/F5 apps). With this
        # UNSET, SMP_SCHED is exactly A-E (CPU1 = coprocessor, instant rollback).
        # Needs the macro in BOTH gcc + nasm (interrupt.asm/context_switch.asm).
        # Build: SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 bash scripts/quick_build.sh
        # =====================================================================
        if [ "${SMP_SCHED_DISPATCH:-0}" = "1" ]; then
            CFLAGS="$CFLAGS -DSMP_SCHED_DISPATCH"
            NASMFLAGS="$NASMFLAGS -DSMP_SCHED_DISPATCH"
            echo "*** SMP_SCHED_DISPATCH: CPU1 in SCHEDULER mode (AP ring-3 dispatch) ***"
        fi
    fi

    # =========================================================================
    # SMP_IPI (SMP-G0 IPI-LINK) -- sub-gate, REQUIRES SMP=1. Compiles the
    # (previously dormant) kernel/arch/x86_64/ipi.c + ipi_handlers.asm and
    # defines -DSMP_IPI. THE DESIGN RULE (user-set at G0): SMP_IPI means the
    # IPI code is LINKED, INITIALIZED, AND PROVEN -- not merely that a dormant
    # file exists. Defining it makes "SMP_FOUNDATION && SMP_IPI" actually true:
    #   - kernel.c calls ipi_init() (IDT vectors 0x50-0x55 claimed AFTER an
    #     explicit free-check; the old 0x40 block collided with the CPU1 LAPIC
    #     timer gate) and runs the IPILINK selftest after the F2 checkpoint.
    #   - kernel_panic's ipi_stop_all_cpus re-arms (the SMP-R0 link fix gated
    #     it on this macro precisely so it could only return with ipi.c linked).
    # When UNSET: ipi.c is not compiled, no -DSMP_IPI, every SMP build is
    # byte-for-byte the F3-5 configuration. NASMFLAGS untouched (the handler
    # stubs have no %ifdef). Build:
    #   SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 bash scripts/quick_build.sh
    # =========================================================================
    if [ "${SMP_IPI:-0}" = "1" ]; then
        CFLAGS="$CFLAGS -DSMP_IPI"
        SMP_IPI_SOURCES="1"
        echo "*** SMP_IPI build: ipi.c + ipi_handlers.asm linked (SMP-G0 IPI-LINK) ***"
    fi

    # =========================================================================
    # SMP_BKL (SMP-H1 BKL-LITE) -- sub-gate, REQUIRES SMP=1. ONE owner-
    # recursive outer kernel lock (kernel/core/syscall/bkl.c) acquired in the
    # syscall dispatcher around the MARKED unsafe groups (FS/net/IPC/proc --
    # the table + the loud blocking-exclusion doc live in bkl.c). The Linux-
    # 2.0-style safety wall before BATCH work runs real userspace on CPU1.
    # kernel.c additionally spawns the two 60s bklstorm instances (CPU1
    # pinned + CPU0 home) for the acceptance run. When UNSET: bkl.c is not
    # compiled, no -DSMP_BKL, every other build is byte-for-byte unchanged.
    # Build: SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 SMP_BKL=1 ...
    # =========================================================================
    if [ "${SMP_BKL:-0}" = "1" ]; then
        CFLAGS="$CFLAGS -DSMP_BKL"
        SMP_BKL_SOURCES="1"
        echo "*** SMP_BKL build: the BKL-LITE outer kernel lock armed (SMP-H1) ***"
    fi

    # =========================================================================
    # SMP_BATCH (SMP-F3-7 BATCH-CLASS) -- sub-gate, REQUIRES SMP=1. Lights the
    # layer-3 batch branch in scheduler_choose_cpu: a BATCH-class task whose
    # legal mask includes CPU1 routes to the PINNED_WORKER core (law 5: batch
    # fills idle capacity). NORMAL stays home CPU0; PINNED_RT obeys its pin;
    # the funnel + enqueue legality walls and the G1 IPI kick are unchanged
    # and mandatory. kernel.c additionally spawns the batchdemo acceptance
    # task (class=BATCH, mask CPU0|CPU1, unpinned -> the seam must choose
    # CPU1). When UNSET: no -DSMP_BATCH, BATCH remains data-only (the
    # PROFILE-0 world), every other build byte-for-byte unchanged. The
    # acceptance profile is the FULL stack:
    #   SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 SMP_BKL=1 SMP_BATCH=1
    # =========================================================================
    if [ "${SMP_BATCH:-0}" = "1" ]; then
        CFLAGS="$CFLAGS -DSMP_BATCH"
        echo "*** SMP_BATCH build: BATCH-class CPU1 routing live (SMP-F3-7) ***"
    fi

    # =========================================================================
    # SMP_RUNMASK (SMP-RUNMASK-0) -- sub-gate, REQUIRES SMP=1. The audit
    # audits REALITY: p->ran_on_cpus is stamped at the single dispatch
    # chokepoint (cpu_set_current_thread) and the TLB pin audit aggregates it
    # per CR3 -- a declared multi-CPU mask is now OK (batchdemo); the same
    # ADDRESS SPACE actually executing on >1 CPU fails loudly. The forced
    # case is a planted footprint on a live PCB (detect + restore). When
    # UNSET: no -DSMP_RUNMASK, no new field, the G2 mask heuristic stands,
    # every other build byte-for-byte unchanged. Acceptance profile:
    #   SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 SMP_BKL=1 \
    #   SMP_BATCH=1 SMP_RUNMASK=1
    # =========================================================================
    if [ "${SMP_RUNMASK:-0}" = "1" ]; then
        CFLAGS="$CFLAGS -DSMP_RUNMASK"
        echo "*** SMP_RUNMASK build: execution-reality pin audit live (SMP-RUNMASK-0) ***"
    fi

    # =========================================================================
    # SMP_DSPLIT (DESKTOP-SPLIT-0) -- sub-gate, REQUIRES SMP=1 (acceptance
    # profile = the FULL stack + SMP_RUNMASK). The milestone brick: desktop-
    # spawned apps on a BORING, EXPLICIT allowlist (batchdemo, bklstorm --
    # single-threaded only; matmuljobs is EXCLUDED because its worker THREADS
    # would put one address space on two CPUs, the exact thing law 18
    # forbids) are declared BATCH + multi-CPU at the sys_spawn seam, so the
    # choose_cpu batch branch routes them to CPU1. Compositor/input/shell
    # remain NORMAL/CPU0. A health-monitor one-shot prints the observed
    # ran_on_cpus reality for the proof. When UNSET: byte-for-byte unchanged.
    #   SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 SMP_BKL=1 \
    #   SMP_BATCH=1 SMP_RUNMASK=1 SMP_DSPLIT=1 bash scripts/quick_build.sh
    # =========================================================================
    if [ "${SMP_DSPLIT:-0}" = "1" ]; then
        CFLAGS="$CFLAGS -DSMP_DSPLIT"
        echo "*** SMP_DSPLIT build: desktop-split allowlist live (DESKTOP-SPLIT-0) ***"
    fi

    # =========================================================================
    # SMP_THREAD_INHERIT (SMP-THREAD-INHERIT-0) -- sub-gate, REQUIRES SMP=1
    # (acceptance profile = the FULL stack + SMP_RUNMASK). Makes SHARED address
    # spaces safe before more real workload lands on CPU1: a thread SHARES its
    # parent's CR3, so it must run on the SAME CPU as the rest of that address
    # space (one mm, one execution CPU) until per-mm TLB shootdown exists. Adds
    # a shared mm_placement {home_cpu, ran_on_cpus, sched_class} tied to the
    # as_refcount lifetime; thread_create INHERITS the mm's home CPU and PINS to
    # it (never widens); dispatch stamps fold into the shared accumulator; a
    # kernel-spawned threaded BATCH probe (threadprobe) proves parent+workers
    # all run CPU1. Does NOT expand the sys_spawn allowlist and does NOT route
    # matmuljobs (the predicate is proven ready; the routing is the next brick).
    # When UNSET: no -DSMP_THREAD_INHERIT, no new field, threads keep the F3-2
    # CPU0-only default, every other build byte-for-byte unchanged. Profile:
    #   SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 SMP_BKL=1 \
    #   SMP_BATCH=1 SMP_RUNMASK=1 SMP_THREAD_INHERIT=1 bash scripts/quick_build.sh
    # =========================================================================
    if [ "${SMP_THREAD_INHERIT:-0}" = "1" ]; then
        CFLAGS="$CFLAGS -DSMP_THREAD_INHERIT"
        echo "*** SMP_THREAD_INHERIT build: shared-mm placement inheritance live (SMP-THREAD-INHERIT-0) ***"
    fi
fi

# =============================================================================
# FRAMEBUFFER WRITE-COMBINING (display speedup) -- always enabled.
# fb_enable_write_combining() is always compiled and called at boot. It programs
# a free variable-range MTRR to mark the framebuffer region Write-Combining,
# coalescing pixel stores into PCIe bursts -- a 10-50x compositor speedup on
# real hardware (the T410) where the firmware maps the FB uncached (UC).
# Runtime-safe: bails cleanly if no free MTRR slot, base unaligned, or VCNT==0.
# In QEMU the FB is already cached, so WC is redundant but harmless.
# The FB_WC=1 env var is no longer needed (kept as a no-op for back-compat).
# =============================================================================

# =============================================================================
# OPT-IN LAPTOP POWER SAVING. GATED behind T410_POWER_SAVE env var.
#   T410_POWER_SAVE=1 bash scripts/quick_build.sh
# Reduces the PIT timer from 1000 Hz to 250 Hz, cutting timer-IRQ wakeups by
# 75%. Each wakeup forces the CPU out of C1 (HLT), so fewer IRQs = more time
# in the low-power halt state. The 4 ms tick granularity is fine for the
# cooperative/light-preempt desktop (~16 ms frame time at 60 fps). The
# scheduler's DEFAULT_TIME_SLICE (10 ticks) becomes 40 ms at 250 Hz -- plenty
# for a laptop workload. Write-combining is now always enabled (no env var needed).
# =============================================================================
if [ "${T410_POWER_SAVE:-0}" = "1" ]; then
    CFLAGS="$CFLAGS -DT410_POWER_SAVE"
    echo "*** T410_POWER_SAVE build: PIT at 250 Hz (power saving) ***"
fi

# OPT-IN slab-corruption hardware watchpoint (debug-branch tooling). SLAB_WATCH=1
# arms DR0/DR1 on the first slab page in the observed corruption region and logs the
# stray writer's RIP via the #DB handler. NOT for shipping builds; default off.
if [ "${SLAB_WATCH:-0}" = "1" ]; then
    CFLAGS="$CFLAGS -DSLAB_WATCH"
    echo "*** SLAB_WATCH build: DR0/DR1 slab-corruption watchpoint enabled ***"
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
# SMP brick 3 (GATED by SMP=1): the 16-bit real-mode -> long-mode AP trampoline.
# Assembled ONLY for the SMP build, so the default kernel.elf is byte-for-byte
# unchanged (this section's input does not exist in the default link). The blob
# is copied to a low page (0x8000) at runtime by ap_boot.c and handed the BSP
# CR3 + the AP stack + the kernel GDTR/IDTR images via its param block.
if [ -n "$SMP_SOURCES" ]; then
    assemble kernel/arch/x86_64/ap_trampoline.asm asm_ap_trampoline
fi
# SMP-G0 (GATED by SMP=1 SMP_IPI=1): the IPI IDT entry stubs (push-GPRs ->
# C handler -> iretq). Only in the SMP_IPI link, so every other build is
# byte-for-byte unchanged.
if [ -n "$SMP_IPI_SOURCES" ]; then
    assemble kernel/arch/x86_64/ipi_handlers.asm asm_ipi_handlers
fi

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
# SMP brick 1 (GATED by SMP=1 -> -DSMP_FOUNDATION): the BSP Local APIC driver.
# Compiled ONLY for the SMP build so the default kernel.elf is byte-for-byte
# unchanged. lapic.c is real salvage (xAPIC + x2APIC); kernel.c calls lapic_init()
# under #ifdef SMP_FOUNDATION to bring the BSP local APIC online (enable + read
# APIC ID/version only -- PIC/IOAPIC/timer/IDT untouched, single-core preserved).
if [ -n "$SMP_SOURCES" ]; then
    compile kernel/arch/x86_64/lapic.c       c_lapic
    # SMP brick 3 (GATED by SMP=1 -> -DSMP_FOUNDATION): the AP-start driver.
    # Stages the trampoline, sends INIT-SIPI-SIPI to CPU 1, and waits on a
    # bounded TSC deadline polling a shared memory flag (try_start_cpu1). Defines
    # the AP's 64-bit entry ap_main() (marks online + cli;hlt). Self-contained
    # like madt.c -- it does NOT define cpu_id(), so there is no collision with
    # stubs.c::cpu_id() (smp.c stays uncompiled). Only built for the SMP kernel.
    compile kernel/arch/x86_64/ap_boot.c     c_ap_boot
    # SMP brick 8.5 (GATED by SMP=1): rapid-fire CPU1 offload stress test (100
    # sequential offloads to check for races in job slot reuse).
    compile kernel/arch/x86_64/test_rapid_cpu1.c c_test_rapid_cpu1
    # SMP race-fix support (GATED by SMP=1): reference counting (kref.c) and the
    # ownership state machine (ownership.c). ap_boot.c's cpu1_job slot uses
    # own_init/own_transition/own_orphan to track CPU0<->CPU1 buffer handoff and to
    # safely orphan in-flight jobs when a process exits; sys_cpu1_offload() (in the
    # SMP-only block of handlers.c) uses kmalloc_ref/kput for refcounted offload
    # buffers. Both are referenced ONLY under SMP_FOUNDATION, so they are linked
    # ONLY in the SMP kernel -- the default kernel.elf stays byte-for-byte
    # unchanged. ownership.c depends on kref.c, so kref.c is compiled first.
    compile kernel/core/mem/kref.c           c_kref
    compile kernel/core/mem/ownership.c      c_ownership
    # Health monitor (session 3): runtime observability - heartbeat, leak detection,
    # deadlock detection, stall detection. Provides health_monitor_tick() for scheduler,
    # health_monitor_record_alloc/free() for kref/ownership, and health_monitor_report()
    # for diagnostics. Only built in SMP kernel.
    compile kernel/core/health_monitor.c    c_health_monitor
    # SMP-G0 (GATED by SMP_IPI=1): the IPI subsystem, adapted to the ap_boot.c
    # CPU-model seam (cpu1_is_online / smp_cpu1_apic_id; NOT smp.c's
    # percpu_data, whose .apic_id is never filled). NOTE: ipi_fixed.c is an
    # older duplicate-symbol variant and must NEVER be added alongside this.
    if [ -n "$SMP_IPI_SOURCES" ]; then
        compile kernel/arch/x86_64/ipi.c     c_ipi
    fi
    # SMP-H1 (GATED by SMP_BKL=1): the BKL-LITE outer kernel lock.
    if [ -n "$SMP_BKL_SOURCES" ]; then
        compile kernel/core/syscall/bkl.c    c_bkl
    fi
fi
compile kernel/drivers/serial.c              c_serial
compile kernel/drivers/pit.c                 c_pit
compile kernel/drivers/ps2.c                 c_ps2
compile kernel/drivers/input/ps2mouse.c      c_ps2mouse
compile kernel/drivers/rtc.c                 c_rtc
compile kernel/drivers/rng.c                 c_rng
compile kernel/drivers/framebuffer.c         c_framebuffer
compile kernel/drivers/core/irq.c            c_irq
compile kernel/drivers/pci.c                 c_pci
# ACPI subsystem: RSDP/RSDT/FADT/DSDT parsing, _S5_ sleep-type decode, S5
# poweroff (ACPI PM1a/PM1b write), ACPI reset-register reboot + 8042 + triple-
# fault fallbacks, MADT/HPET/MCFG enumeration. The acpi_state_t global in .bss
# is ~300 bytes (pointers + a few uint16s). Called from kernel_main() after
# pci_init(). NOTE: only kernel/acpi/acpi.c is compiled; kernel/drivers/acpi/
# acpi.c is a standalone variant that is NOT linked (duplicate symbols).
compile kernel/acpi/acpi.c                   c_acpi
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
compile kernel/drivers/hda.c                  c_hda
compile kernel/drivers/hda_stream.c           c_hda_stream
compile kernel/drivers/audio/audio_core.c     c_audio_core
compile kernel/drivers/audio/audio_tone.c     c_audio_tone
compile kernel/net/route.c                    c_route
compile kernel/net/netif.c                   c_netif
compile kernel/net/netsyscall.c              c_netsyscall
# BSD-ish sockets (UDP + active-open TCP) on top of net.c. The ~338KB socket
# table now lives in kmalloc (see socket.c), NOT .bss, so these are safe to link.
compile kernel/net/socket.c                  c_socket
compile kernel/net/udp.c                      c_udp
compile kernel/net/tcp.c                      c_tcp
# NET-P1-A0 test rig: compiles EMPTY unless -DNET_SELFTEST (NET_SELFTEST=1).
compile kernel/net/net_testrig.c             c_net_testrig
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
# IPC umbrella initializer: kernel.c calls ipc_init() (shm/msg/notify/clipboard
# init in order). ipc.c has 0 bytes .bss and only references already-linked
# symbols, so it is safe in BOTH the default and SMP builds. Was missing from the
# build list, leaving ipc_init undefined and breaking the strict link.
compile kernel/ipc/ipc.c                      c_ipc
compile kernel/core/sched/scheduler.c        c_scheduler
compile kernel/core/sched/process.c          c_process
compile kernel/core/sched/context.c          c_context
compile kernel/core/sched/waitqueue.c        c_waitqueue
compile kernel/core/syscall/handlers.c       c_syscall_handlers
compile kernel/core/syscall/syscall.c        c_syscall
# rlimit + seccomp: subsystems have unresolved deps (get_timer_ticks, bpf_prog_destroy)
# and are not fully integrated into the kernel yet. Syscall handlers are registered
# but the source files are excluded from the build until integration is complete.
# compile kernel/core/rlimit/*.c
# compile kernel/security/seccomp/*.c
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
# USB-MOUSE-0 (GATED by USB_UHCI=1): dormant usb_core/uhci/hid, compiled here so a
# default build is byte-for-byte unchanged. hid.c injects pointer/key events via
# the SAME input_report_*/input_sync path as ps2mouse.c -> the compositor needs no
# new input path. Boot init (usb_init) is NOT called yet -- compile/link only.
if [ "${USB_UHCI:-0}" = "1" ]; then
    compile kernel/drivers/usb/usb_core.c     c_usb_core
    compile kernel/drivers/usb/uhci.c         c_uhci
    compile kernel/drivers/usb/hid.c          c_hid
fi
compile kernel/ipc/channel.c                  c_channel
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
