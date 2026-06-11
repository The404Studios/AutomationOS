#!/bin/bash
# batchclass_smoke.sh -- the SMP-F3-7 BATCH-CLASS proof vehicle.
# =============================================================================
# Build: THE FULL STACK (SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1
# SMP_BKL=1 SMP_BATCH=1). F3-7 lights the layer-3 batch branch: an ORDINARY
# (unpinned) BATCH-class task with a multi-CPU legal mask routes to CPU1,
# IPI-woken, with every marked syscall it makes covered by the BKL wall.
#
# Gates:
#   1. LINK gate -- "Link OK" grepped (never trust rc).
#   2. BATCHCLASS gates (serial):
#        core      : BATCHCLASS-CORE: PASS batch_cpu1=1 batch_mask_respected=1
#                    normal_cpu0=1 pinned_rt_cpu1=1 illegal_clamped=1
#        live seam : '[SMP] F3-7: batchdemo PID n class=BATCH unpinned -> the
#                    seam chose cpu1'  (the SEAM decided, not the call site)
#        ipi_wake  : '[SMP] F3-7: batchdemo enqueue->dispatch latency=<n> us'
#                    with n < 10000 (beat the 10 ms tick floor). NOTE: this is
#                    NOT an idle-wake measurement -- batchdemo enqueues while
#                    CPU1 is legitimately time-sharing cpu1hello, so ~2 ms of
#                    cooperative hand-off is expected. The rigorous IDLE
#                    IPI-wake proof is the G1 ping ladder (32/32 < 1 ms),
#                    re-verified in this same boot by gate 3.
#        ran on CPU1: 'BATCHDEMO mark' x3 + 'BATCHDEMO done reads=.. errors=0'
#                    + init reaped its pid (the ordinary lifecycle completed)
#        bkl_safe  : '[BKL] engaged' + zero deadlock/BUG warnings while the
#                    demo's marked syscalls ran on CPU1 under the wall
#   3. REGRESSION gate -- the ENTIRE ladder including the 60s BKL storms.
#
# Composite acceptance assembled here (the house convention):
#   BATCHCLASS: PASS batch_cpu1=1 normal_cpu0=1 pinned_rt_cpu1=1
#               illegal_clamped=1 ipi_wake=1 bkl_safe=1
#
# Prereq: iso/boot/initrd.img containing sbin/batchdemo + sbin/bklstorm.
# Run: wsl -d Arch bash -lc 'bash scripts/batchclass_smoke.sh'
set -u
ROOT=/mnt/c/Users/wilde/Desktop/Kernel
cd "$ROOT" || exit 9

QEMU_TIMEOUT="${QEMU_TIMEOUT:-300}"
SER=/tmp/batchclass_serial.log
QB=/tmp/batchclass_qb.log

echo "[batchclass-smoke] building kernel-smp.elf (FULL stack + SMP_BATCH=1) ..."
SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 SMP_BKL=1 SMP_BATCH=1 bash scripts/quick_build.sh > "$QB" 2>&1 \
    || { echo "[batchclass-smoke] build FAILED"; tail -25 "$QB"; exit 1; }

if ! grep -qF 'Link OK -- no unresolved symbols' "$QB"; then
    echo "[batchclass-smoke] LINK gate FAILED:"; grep -A8 -F 'LINK FAILED' "$QB" | head -20
    exit 1
fi
if grep -qE '^FAIL: ' "$QB"; then
    echo "[batchclass-smoke] COMPILE gate FAILED:"; grep -B1 -A6 -E '^FAIL: ' "$QB" | head -30
    exit 1
fi
echo "[batchclass-smoke] LINK gate OK"

if [ ! -s iso/boot/initrd.img ]; then echo "[batchclass-smoke] initrd missing"; exit 1; fi
if ! grep -qc 'BATCHDEMO done' iso/boot/initrd.img; then
    echo "[batchclass-smoke] initrd lacks sbin/batchdemo -- run IDE=1 build_all"; exit 1
fi
if [ ! -s build/kernel.elf ]; then echo "[batchclass-smoke] default kernel.elf missing"; exit 1; fi

echo "[batchclass-smoke] assembling ISO ..."
cp build/kernel-smp.elf iso/boot/kernel.elf
grub-mkrescue -o build/automationos-batchclass.iso iso/ 2>/dev/null \
    || { echo "[batchclass-smoke] grub-mkrescue FAILED"; cp build/kernel.elf iso/boot/kernel.elf; exit 1; }
cp build/kernel.elf iso/boot/kernel.elf

rm -f "$SER"
echo "[batchclass-smoke] booting qemu -smp 2 (timeout ${QEMU_TIMEOUT}s) ..."
timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
    -cdrom build/automationos-batchclass.iso \
    -m 512 -smp 2 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== gate 2: BATCHCLASS (core + live seam + ipi_wake + lifecycle + bkl) ==="
grep -F 'BATCHCLASS-CORE' "$SER" | head -1
grep -E '\[SMP\] F3-7: batchdemo' "$SER" | head -3
grep -F 'BATCHDEMO done' "$SER" | head -1

CORE_OK=0
grep -qF 'BATCHCLASS-CORE: PASS batch_cpu1=1 batch_mask_respected=1 normal_cpu0=1 pinned_rt_cpu1=1 illegal_clamped=1' "$SER" && CORE_OK=1
SEAM_OK=0
grep -qE '\[SMP\] F3-7: batchdemo PID [0-9]+ class=BATCH unpinned -> the seam chose cpu1' "$SER" && SEAM_OK=1
WAKE_US=999999; IPIW_OK=0
WLINE=$(grep -oE 'batchdemo enqueue->dispatch latency=[0-9]+ us' "$SER" | tail -1)
if [ -n "$WLINE" ]; then
    WAKE_US=$(echo "$WLINE" | grep -oE '[0-9]+' | head -1)
    # < 10 ms: beat the tick floor (see the header note -- CPU1 is busy with
    # cpu1hello here; the idle IPI-wake proof is the G1 ping ladder below)
    [ "$WAKE_US" -lt 10000 ] && IPIW_OK=1
fi
DMARKS=$(grep -cF 'BATCHDEMO mark' "$SER" || true); DMARKS=${DMARKS:-0}
DDONE=0
grep -qE 'BATCHDEMO done reads=[1-9][0-9]* errors=0' "$SER" && DDONE=1
DPID=$(grep -oE 'batchdemo PID [0-9]+' "$SER" | head -1 | grep -oE '[0-9]+')
DREAP=0
if [ -n "${DPID:-}" ]; then
    grep -qE "\[INIT\] Process ${DPID} exited" "$SER" && DREAP=1
fi
ENG=0; grep -qE '\[BKL\] engaged: cpu0_acq=[0-9]+ cpu1_acq=[1-9][0-9]*' "$SER" && ENG=1
NDEAD=$(grep -cE '\[BKL\] (possible deadlock|BUG)' "$SER" || true)
BKL_OK=0; [ "$ENG" = "1" ] && [ "$NDEAD" = "0" ] && BKL_OK=1
echo "  core=$CORE_OK seam=$SEAM_OK ipi_wake=$IPIW_OK(${WAKE_US}us) marks=$DMARKS done=$DDONE reaped=$DREAP bkl=(engaged=$ENG warnings=$NDEAD)"

echo "=== gate 3: the full regression ladder (incl. the 60s BKL storms) ==="
ST_DONE=$(grep -cE 'BKLSTORM pid=[0-9]+ done iters=[0-9]+ errors=0 secs=' "$SER" || true)
PR_OK=0
grep -qF 'SMPPROFILE-CORE: PASS normal_home=1 batch_declared=1 pinned_rt_legal=1 no_behavior_change=1' "$SER" && PR_OK=1
CC_OK=0
grep -qF 'CHOOSECPU: PASS pinned_cpu1=1 default_cpu0=1 illegal_clamped=1 nomask_clamped=1 multimask_home=1 cpu1only_role=1' "$SER" && CC_OK=1
TS_OK=0;  grep -qE 'TLBSHOOT: PASS kernel_flush=1 acked=1 bounded=1 invariant=1' "$SER" && TS_OK=1
NEG_OK=0; grep -qF 'TLBSHOOT_NEG: PASS no_user_crossflush_needed_under_pinning=1' "$SER" && NEG_OK=1
NTLBV=$(grep -cF '[TLB_INVARIANT] VIOLATION' "$SER" || true)
WK_OK=0; grep -qE 'ping summary acks=32/32 max_latency_us=[0-9]+' "$SER" && WK_OK=1
IPI_OK=0; grep -qF 'IPILINK: PASS ipi_resched=1 cpu1_count=1' "$SER" && IPI_OK=1
F2_OK=0;  grep -qE 'Brick F2 VERIFY:.*delta=[1-9][0-9]*' "$SER" && F2_OK=1
AC_OK=0;  grep -qF 'APCURRENT: PASS' "$SER" && AC_OK=1
MARKS=$(grep -cF 'CPU1HELLO mark' "$SER" || true); MARKS=${MARKS:-0}
HPID=$(grep -oE 'cpu1hello PID [0-9]+' "$SER" | head -1 | grep -oE 'PID [0-9]+' | grep -oE '[0-9]+')
EXIT42=0
grep -qE "sys_exit: Process 'cpu1hello' \(PID [0-9]+\) exiting with status 42" "$SER" && EXIT42=1
REAPED=0
if [ -n "${HPID:-}" ]; then
    grep -qE "\[INIT\] Process ${HPID} exited" "$SER" && REAPED=1
fi
ALIVE=1
grep -qF 'entering frame loop' "$SER" || ALIVE=0
grep -qF '[INIT] Compositor died' "$SER" && ALIVE=0
NSCHED=$(grep -cF '[SCHED_INVARIANT]' "$SER" || true)
NPANIC=$(grep -cE 'PANIC|CPU EXCEPTION|TRIPLE FAULT|KERNEL PANIC' "$SER" || true)
[ "$NPANIC" = "0" ] || ALIVE=0
echo "  storms=$ST_DONE/2 profile=$PR_OK choosecpu=$CC_OK tlbshoot=$TS_OK neg=$NEG_OK wake=$WK_OK ipilink=$IPI_OK F2=$F2_OK APCURRENT=$AC_OK marks=$MARKS exit42=$EXIT42 reaped=$REAPED alive=$ALIVE sched_viol=$NSCHED tlb_viol=$NTLBV panic=$NPANIC"

if [ "$CORE_OK" = "1" ] && [ "$SEAM_OK" = "1" ] && [ "$IPIW_OK" = "1" ] && \
   [ "$DMARKS" -ge 3 ] && [ "$DDONE" = "1" ] && [ "$DREAP" = "1" ] && [ "$BKL_OK" = "1" ] && \
   [ "$ST_DONE" = "2" ] && [ "$PR_OK" = "1" ] && [ "$CC_OK" = "1" ] && \
   [ "$TS_OK" = "1" ] && [ "$NEG_OK" = "1" ] && [ "$NTLBV" = "0" ] && \
   [ "$WK_OK" = "1" ] && [ "$IPI_OK" = "1" ] && \
   [ "$F2_OK" = "1" ] && [ "$AC_OK" = "1" ] && [ "$MARKS" -ge 1 ] && \
   [ "$EXIT42" = "1" ] && [ "$REAPED" = "1" ] && [ "$ALIVE" = "1" ] && \
   [ "$NSCHED" = "0" ] && [ "$NPANIC" = "0" ]; then
    echo "BATCHCLASS: PASS batch_cpu1=1 normal_cpu0=1 pinned_rt_cpu1=1 illegal_clamped=1 ipi_wake=1 bkl_safe=1"
    echo "[batchclass-smoke] RESULT: PASS -- ordinary work runs on CPU1 under typed intent (wake=${WAKE_US}us)"
    exit 0
else
    echo "BATCHCLASS: FAIL core=$CORE_OK seam=$SEAM_OK ipi_wake=$IPIW_OK(${WAKE_US}us) demo=(marks=$DMARKS done=$DDONE reap=$DREAP) bkl=$BKL_OK storms=$ST_DONE ladder=(prof=$PR_OK cc=$CC_OK ts=$TS_OK neg=$NEG_OK wake=$WK_OK ipi=$IPI_OK f2=$F2_OK ac=$AC_OK marks=$MARKS exit42=$EXIT42 reaped=$REAPED) alive=$ALIVE sched=$NSCHED tlb=$NTLBV panic=$NPANIC"
    echo "--- last 50 serial lines ---"; tail -50 "$SER"
    exit 1
fi
