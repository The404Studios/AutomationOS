#!/bin/bash
# threadinherit_smoke.sh -- the SMP-THREAD-INHERIT-0 proof vehicle.
# =============================================================================
# Makes SHARED address spaces safe before more real workload lands on CPU1: a
# thread SHARES its parent's address space, so it must run on the SAME CPU as
# the rest of that address space (ONE mm, ONE execution CPU) until per-mm TLB
# shootdown exists. The kernel-spawned threaded BATCH probe (threadprobe) +
# its 2 worker threads must ALL run CPU1 (the workers INHERIT the parent's
# placement, not the CPU0 ctor default); the desktop core stays CPU0; every SMP
# wall stays green over a 30-minute soak.
#
# ONE boot: the FULL stack + SMP_DSPLIT + SMP_THREAD_INHERIT, qemu -smp 2.
# (SMP_DSPLIT is in the profile so the [DSPLIT] desktop-core observation gives
#  the desktop_cpu0 evidence; DESKTOP-SPLIT-0 is the shipped baseline.)
#
# Gates:
#   batch_parent_cpu1 : [THREADINHERIT] summary ... batch_parent_cpu1=1 (the
#                       probe parent ran on CPU1: ran=0x2).
#   workers_same_cpu  : summary ... workers_same_cpu=1 (both worker threads
#                       ran=0x2, NONE on CPU0).
#   sched_inherit     : summary ... sched_inherit=1 AND THREADINHERIT-CORE PASS
#                       class_inherit=1 (workers inherited BATCH).
#   runmask_clean     : RUNMASK-CORE PASS + exactly ONE [RUNMASK] VIOLATION
#                       (the planted selftest; the threaded probe adds ZERO --
#                       mm_single_cpu=1 in the summary proves the mm never
#                       spanned two CPUs).
#   desktop_cpu0      : [DSPLIT] observed 'sbin/compositor' ran=0x1.
#   matmuljobs_ready  : THREADINHERIT-CORE PASS matmuljobs_ready=1 (the inherit
#                       predicate a BATCH-CPU1 threaded app needs is proven).
#   no_allowlist_exp  : SOURCE check -- dsplit_allow[] is still {batchdemo,
#                       bklstorm} and matmuljobs is NOT BATCH-routed anywhere.
#   soak              : >= 55 [COMP] fps windows (~28+ min live), desktop alive,
#                       0 PANIC / 0 SCHED+TLB invariant.
#
# Composite acceptance assembled here:
#   THREADINHERIT: PASS batch_parent_cpu1=1 workers_same_cpu=1 sched_inherit=1
#                  runmask_clean=1 desktop_cpu0=1 matmuljobs_ready=1
#                  no_allowlist_expansion=1 soak=30m panic=0 invariant=0
#
# Prereq: a current world ISO (IDE=1 build_all -- ships sbin/threadprobe).
# Run: wsl -d Arch bash -lc 'bash scripts/threadinherit_smoke.sh'
#      SOAK_SECS=150 bash scripts/threadinherit_smoke.sh   # short validation
set -u
ROOT=/mnt/c/Users/wilde/Desktop/Kernel
cd "$ROOT" || exit 9

SOAK_SECS="${SOAK_SECS:-1980}"
SER=/tmp/ti_soak.log
QB=/tmp/ti_qb.log

echo "[ti] building the THREAD-INHERIT kernel (full stack + SMP_DSPLIT + SMP_THREAD_INHERIT) ..."
SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 SMP_BKL=1 SMP_BATCH=1 SMP_RUNMASK=1 SMP_DSPLIT=1 SMP_THREAD_INHERIT=1 \
    bash scripts/quick_build.sh > "$QB" 2>&1 || { echo "[ti] build FAILED"; tail -20 "$QB"; exit 1; }
grep -qF 'Link OK -- no unresolved symbols' "$QB" || { echo "[ti] LINK gate FAILED"; exit 1; }
echo "[ti] building the DEFAULT kernel (byte-identity) ..."
bash scripts/quick_build.sh > /tmp/ti_qb_def.log 2>&1
grep -qF 'Link OK -- no unresolved symbols' /tmp/ti_qb_def.log || { echo "[ti] default LINK FAILED"; exit 1; }
DEF_MD5=$(md5sum build/kernel.elf | awk '{print $1}')
echo "[ti] default kernel md5=$DEF_MD5 (expect 6f99ed9ffaf09a7fcb36996324c9450b)"

[ -s iso/boot/initrd.img ] || { echo "[ti] initrd missing -- run IDE=1 build_all"; exit 1; }

# ---- the soak: THREAD-INHERIT kernel, -smp 2 -------------------------------
echo "[ti] soak (${SOAK_SECS}s, THREAD-INHERIT kernel, -smp 2) ..."
cp build/kernel-smp.elf iso/boot/kernel.elf
grub-mkrescue -o build/automationos-ti.iso iso/ 2>/dev/null || { echo "grub fail"; exit 1; }
cp build/kernel.elf iso/boot/kernel.elf      # restore the default in the tree
rm -f "$SER"
timeout "$SOAK_SECS" qemu-system-x86_64 -cdrom build/automationos-ti.iso \
    -m 512 -smp 2 -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== threadprobe + inheritance, observed ==="
grep -E '\[THREADINHERIT\]|THREADINHERIT-CORE|THREAD-INHERIT: threadprobe|THREADPROBE' "$SER" | head -16

# ---- gates ------------------------------------------------------------------
# source-level no_allowlist_expansion: the allowlist is unchanged + matmuljobs unrouted.
ALLOW_SRC=$(grep -E 'dsplit_allow\[\] *= *\{' kernel/core/syscall/handlers.c | head -1)
NOEXP=0
echo "$ALLOW_SRC" | grep -q 'batchdemo' && echo "$ALLOW_SRC" | grep -q 'bklstorm' \
    && ! echo "$ALLOW_SRC" | grep -q 'matmuljobs' && NOEXP=1
# matmuljobs must not be BATCH-routed anywhere in the kernel spawn paths.
grep -qE 'matmuljobs.*SCHED_CLASS_BATCH|SCHED_CLASS_BATCH.*matmuljobs' kernel/kernel.c && NOEXP=0

CORE_OK=0
grep -qE 'THREADINHERIT-CORE: PASS .*class_inherit=1 .*matmuljobs_ready=1' "$SER" && CORE_OK=1
MMJ_OK=0
grep -qE 'THREADINHERIT-CORE: PASS .*matmuljobs_ready=1' "$SER" && MMJ_OK=1

PAR_OK=0;  grep -qE '\[THREADINHERIT\] summary: batch_parent_cpu1=1 ' "$SER" && PAR_OK=1
WRK_OK=0;  grep -qE '\[THREADINHERIT\] summary: .* workers_same_cpu=1 ' "$SER" && WRK_OK=1
SI_OK=0;   grep -qE '\[THREADINHERIT\] summary: .* sched_inherit=1 ' "$SER" && [ "$CORE_OK" = "1" ] && SI_OK=1
MM1_OK=0;  grep -qE '\[THREADINHERIT\] summary: .* mm_single_cpu=1 ' "$SER" && MM1_OK=1

DESK_OK=0
grep -qE "\[DSPLIT\] observed: pid=[0-9]+ 'sbin/compositor' ran=0x1\b" "$SER" && DESK_OK=1

RM_OK=0
grep -qF 'RUNMASK-CORE: PASS baseline_clean=1 forced_crosscpu_detected=1 restored_clean=1' "$SER" && RM_OK=1
NVIOL=$(grep -cF '[RUNMASK] VIOLATION' "$SER" || true)
RM_CLEAN=0; [ "$NVIOL" = "1" ] && [ "$MM1_OK" = "1" ] && RM_CLEAN=1

# ladder walls (inherited, must stay green)
NEG_OK=0;  grep -qF 'TLBSHOOT_NEG: PASS no_user_crossflush_needed_under_pinning=1' "$SER" && NEG_OK=1
ST_DONE=$(grep -cE 'BKLSTORM pid=[0-9]+ done iters=[0-9]+ errors=0 secs=' "$SER" || true)
ENG=0;   grep -qE '\[BKL\] engaged' "$SER" && ENG=1
NDEAD=$(grep -cE '\[BKL\] (possible deadlock|BUG)' "$SER" || true)
BKL_OK=0; [ "$ST_DONE" = "2" ] && [ "$ENG" = "1" ] && [ "$NDEAD" = "0" ] && BKL_OK=1
WK_OK=0; grep -qE 'ping summary acks=32/32' "$SER" && WK_OK=1
IPI_OK=0; grep -qF 'IPILINK: PASS' "$SER" && IPI_OK=1
TS_OK=0;  grep -qE 'TLBSHOOT: PASS kernel_flush=1' "$SER" && TS_OK=1

SOAK_WINS=$(grep -cF '[COMP] fps window' "$SER" || true)
SOAK_OK=0; [ "$SOAK_WINS" -ge 55 ] && SOAK_OK=1
NTLBV=$(grep -cF '[TLB_INVARIANT] VIOLATION' "$SER" || true)
NSCHED=$(grep -cF '[SCHED_INVARIANT]' "$SER" || true)
NPANIC=$(grep -cE 'PANIC|CPU EXCEPTION|TRIPLE FAULT|KERNEL PANIC' "$SER" || true)
ALIVE=1
grep -qF 'entering frame loop' "$SER" || ALIVE=0
grep -qF '[INIT] Compositor died' "$SER" && ALIVE=0

echo "split:  parent_cpu1=$PAR_OK workers_same_cpu=$WRK_OK sched_inherit=$SI_OK(core=$CORE_OK) mm_single_cpu=$MM1_OK desktop_cpu0=$DESK_OK matmuljobs_ready=$MMJ_OK no_allowlist_expansion=$NOEXP"
echo "walls:  runmask=$RM_OK(viol=$NVIOL clean=$RM_CLEAN) tlb_neg=$NEG_OK bkl=$BKL_OK(storms=$ST_DONE) ipiwake=$WK_OK ipilink=$IPI_OK tlbshoot=$TS_OK"
echo "soak:   windows=$SOAK_WINS alive=$ALIVE sched_viol=$NSCHED tlb_viol=$NTLBV panic=$NPANIC"

if [ "$DEF_MD5" = "6f99ed9ffaf09a7fcb36996324c9450b" ] && \
   [ "$PAR_OK" = "1" ] && [ "$WRK_OK" = "1" ] && [ "$SI_OK" = "1" ] && \
   [ "$RM_OK" = "1" ] && [ "$RM_CLEAN" = "1" ] && [ "$DESK_OK" = "1" ] && \
   [ "$MMJ_OK" = "1" ] && [ "$NOEXP" = "1" ] && \
   [ "$NEG_OK" = "1" ] && [ "$BKL_OK" = "1" ] && [ "$WK_OK" = "1" ] && \
   [ "$IPI_OK" = "1" ] && [ "$TS_OK" = "1" ] && [ "$SOAK_OK" = "1" ] && \
   [ "$ALIVE" = "1" ] && [ "$NSCHED" = "0" ] && [ "$NTLBV" = "0" ] && [ "$NPANIC" = "0" ]; then
    echo "THREADINHERIT: PASS batch_parent_cpu1=1 workers_same_cpu=1 sched_inherit=1 runmask_clean=1 desktop_cpu0=1 matmuljobs_ready=1 no_allowlist_expansion=1 soak=30m panic=0 invariant=0"
    echo "[ti] RESULT: PASS -- shared address spaces are safe: a threaded BATCH workload runs wholly on CPU1, the desktop stays CPU0, every wall green"
    exit 0
else
    echo "THREADINHERIT: FAIL parent=$PAR_OK workers=$WRK_OK sched_inherit=$SI_OK runmask=$RM_OK/$RM_CLEAN(viol=$NVIOL) desktop=$DESK_OK mmj_ready=$MMJ_OK noexp=$NOEXP neg=$NEG_OK bkl=$BKL_OK wake=$WK_OK ipi=$IPI_OK ts=$TS_OK soak=$SOAK_OK($SOAK_WINS) alive=$ALIVE sched=$NSCHED tlb=$NTLBV panic=$NPANIC def_md5=$DEF_MD5"
    echo "--- last 40 serial lines ---"; tail -40 "$SER"
    exit 1
fi
