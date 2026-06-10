#!/bin/bash
# choosecpu_smoke.sh -- the SMP-F3-6 CHOOSECPU proof vehicle.
# =============================================================================
# Same build profile as G0/G1/G2 (SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1
# SMP_IPI=1). F3-6 lands THE placement seam (scheduler_choose_cpu, the
# SCHEDULER_POLICY_LAYER spec): legality first, pin/role second, home-CPU0
# stub third, MLFQ untouched. Every CPU1 placement (F2 kthread + cpu1hello +
# every home-routed wake) now goes through ONE decider. Gates, in order:
#
#   1. LINK gate     -- "Link OK" grepped from the build log (never trust rc).
#   2. CHOOSECPU gate (kernel-printed, synthetic-branch proof):
#        CHOOSECPU: PASS pinned_cpu1=1 default_cpu0=1 illegal_clamped=1
#                        nomask_clamped=1 multimask_home=1 cpu1only_role=1
#   3. LIVE-SEAM gate -- "cpu1hello placed via scheduler_choose_cpu -> cpu1"
#      (the real ring-3 task placement went through the seam and chose CPU1).
#   4. REGRESSION gate -- the full ladder: TLBSHOOT + NEG + IPIWAKE pings +
#      IPILINK + F2 + APCURRENT + CPU1HELLO exit/reap + 0 invariant (sched AND
#      tlb) + desktop alive + 0 panic. F3-5 still passing IS the proof the
#      seam-routed placements behave identically to the hand placements.
#
# NOTE on byte-identity: F3-6 deliberately changes SMP_SCHED_DISPATCH builds
# (it IS a scheduler-policy brick on the dispatch path); only the DEFAULT
# kernel.elf must stay hash-identical (checked by the brick, not here).
#
# Run: wsl -d Arch bash -lc 'bash scripts/choosecpu_smoke.sh'
set -u
ROOT=/mnt/c/Users/wilde/Desktop/Kernel
cd "$ROOT" || exit 9

QEMU_TIMEOUT="${QEMU_TIMEOUT:-240}"
SER=/tmp/choosecpu_serial.log
QB=/tmp/choosecpu_qb.log

echo "[choosecpu-smoke] building kernel-smp.elf (SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1) ..."
SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 bash scripts/quick_build.sh > "$QB" 2>&1 \
    || { echo "[choosecpu-smoke] build FAILED"; tail -25 "$QB"; exit 1; }

if ! grep -qF 'Link OK -- no unresolved symbols' "$QB"; then
    echo "[choosecpu-smoke] LINK gate FAILED:"; grep -A8 -F 'LINK FAILED' "$QB" | head -20
    exit 1
fi
if grep -qE '^FAIL: ' "$QB"; then
    echo "[choosecpu-smoke] COMPILE gate FAILED:"; grep -B1 -A6 -E '^FAIL: ' "$QB" | head -30
    exit 1
fi
echo "[choosecpu-smoke] LINK gate OK"

if [ ! -s iso/boot/initrd.img ]; then echo "[choosecpu-smoke] initrd missing"; exit 1; fi
if [ ! -s build/kernel.elf ];   then echo "[choosecpu-smoke] default kernel.elf missing"; exit 1; fi

echo "[choosecpu-smoke] assembling ISO ..."
cp build/kernel-smp.elf iso/boot/kernel.elf
grub-mkrescue -o build/automationos-choosecpu.iso iso/ 2>/dev/null \
    || { echo "[choosecpu-smoke] grub-mkrescue FAILED"; cp build/kernel.elf iso/boot/kernel.elf; exit 1; }
cp build/kernel.elf iso/boot/kernel.elf

rm -f "$SER"
echo "[choosecpu-smoke] booting qemu -smp 2 (timeout ${QEMU_TIMEOUT}s) ..."
timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
    -cdrom build/automationos-choosecpu.iso \
    -m 512 -smp 2 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== gate 2: CHOOSECPU (the F3-6 acceptance, kernel-printed) ==="
grep -F 'CHOOSECPU' "$SER" | head -2
CC_OK=0
grep -qF 'CHOOSECPU: PASS pinned_cpu1=1 default_cpu0=1 illegal_clamped=1 nomask_clamped=1 multimask_home=1 cpu1only_role=1' "$SER" && CC_OK=1

echo "=== gate 3: live seam placement (cpu1hello) ==="
grep -F 'placed via scheduler_choose_cpu' "$SER" | head -2
LIVE_OK=0
grep -qF 'cpu1hello placed via scheduler_choose_cpu -> cpu1' "$SER" && LIVE_OK=1

echo "=== gate 4: G2 + G1 + G0 + F3-5 regression ladder ==="
TS_OK=0;  grep -qE 'TLBSHOOT: PASS kernel_flush=1 acked=1 bounded=1 invariant=1' "$SER" && TS_OK=1
NEG_OK=0; grep -qF 'TLBSHOOT_NEG: PASS no_user_crossflush_needed_under_pinning=1' "$SER" && NEG_OK=1
NTLBV=$(grep -cF '[TLB_INVARIANT] VIOLATION' "$SER" || true)
WAKE_OK=0; grep -qE 'ping summary acks=32/32 max_latency_us=[0-9]+' "$SER" && WAKE_OK=1
ENQ_OK=0;  grep -qE 'enqueue->dispatch latency=[0-9]+ us' "$SER" && ENQ_OK=1
IPI_OK=0;  grep -qF 'IPILINK: PASS ipi_resched=1 cpu1_count=1' "$SER" && IPI_OK=1
F2_OK=0;   grep -qE 'Brick F2 VERIFY:.*delta=[1-9][0-9]*' "$SER" && F2_OK=1
AC_OK=0;   grep -qF 'APCURRENT: PASS' "$SER" && AC_OK=1
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
echo "  tlbshoot=$TS_OK neg=$NEG_OK wake=$WAKE_OK enq=$ENQ_OK ipilink=$IPI_OK F2=$F2_OK APCURRENT=$AC_OK marks=$MARKS exit42=$EXIT42 reaped=$REAPED alive=$ALIVE sched_viol=$NSCHED tlb_viol=$NTLBV panic=$NPANIC"

if [ "$CC_OK" = "1" ] && [ "$LIVE_OK" = "1" ] && \
   [ "$TS_OK" = "1" ] && [ "$NEG_OK" = "1" ] && [ "$NTLBV" = "0" ] && \
   [ "$WAKE_OK" = "1" ] && [ "$ENQ_OK" = "1" ] && [ "$IPI_OK" = "1" ] && \
   [ "$F2_OK" = "1" ] && [ "$AC_OK" = "1" ] && [ "$MARKS" -ge 1 ] && \
   [ "$EXIT42" = "1" ] && [ "$REAPED" = "1" ] && [ "$ALIVE" = "1" ] && \
   [ "$NSCHED" = "0" ] && [ "$NPANIC" = "0" ]; then
    echo "CHOOSECPU-SMOKE: PASS seam=1 live_placement=1 regression_green=1"
    echo "[choosecpu-smoke] RESULT: PASS -- the placement seam is live, one decider"
    exit 0
else
    echo "CHOOSECPU-SMOKE: FAIL choosecpu=$CC_OK live=$LIVE_OK tlbshoot=$TS_OK neg=$NEG_OK wake=$WAKE_OK enq=$ENQ_OK ipilink=$IPI_OK f35=(f2=$F2_OK ac=$AC_OK marks=$MARKS exit42=$EXIT42 reaped=$REAPED) alive=$ALIVE sched=$NSCHED tlb=$NTLBV panic=$NPANIC"
    echo "--- last 40 serial lines ---"; tail -40 "$SER"
    exit 1
fi
