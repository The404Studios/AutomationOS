#!/bin/bash
# runmask_smoke.sh -- the SMP-RUNMASK-0 proof vehicle.
# =============================================================================
# Build: THE FULL STACK + SMP_RUNMASK=1. The audit micro-brick before
# DESKTOP-SPLIT: the TLB pin audit now audits EXECUTION REALITY
# (per-CR3-aggregated ran_on_cpus, stamped at the dispatch chokepoint), not
# declared masks.
#
# Gates:
#   1. LINK gate -- "Link OK" grepped (never trust rc).
#   2. RUNMASK gates (serial):
#        core      : RUNMASK-CORE: PASS baseline_clean=1
#                    forced_crosscpu_detected=1 restored_clean=1
#                    (the planted footprint on init was caught + restored)
#        plant loud: exactly ONE '[RUNMASK] VIOLATION' line in the whole boot
#                    (the planted one -- proving detection is loud AND that
#                    real traffic produced zero violations)
#        exit rec  : '[RUNMASK] exit record: ... \'batchdemo\' allowed=0x3
#                    ran=0x2 single_cpu=1' -- THE brick's reason to exist: a
#                    declared multimask process that actually ran on exactly
#                    one CPU, recorded at its exit boundary
#        neg valid : 'TLBSHOOT_NEG: PASS no_user_crossflush_needed_under_
#                    pinning=1' still prints (exact prefix preserved) with
#                    the '(RUNMASK upgrade:' detail -- the upgraded semantics
#   3. REGRESSION gate -- the ENTIRE ladder (BATCHCLASS live proof, storms,
#      everything) under the new audit.
#
# Composite acceptance assembled here:
#   RUNMASK: PASS declared_multimask_ok=1 actual_single_cpu=1
#            forced_crosscpu_detected=1 tlb_neg_valid=1
#
# Run: wsl -d Arch bash -lc 'bash scripts/runmask_smoke.sh'
set -u
ROOT=/mnt/c/Users/wilde/Desktop/Kernel
cd "$ROOT" || exit 9

QEMU_TIMEOUT="${QEMU_TIMEOUT:-300}"
SER=/tmp/runmask_serial.log
QB=/tmp/runmask_qb.log

echo "[runmask-smoke] building kernel-smp.elf (FULL stack + SMP_RUNMASK=1) ..."
SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 SMP_BKL=1 SMP_BATCH=1 SMP_RUNMASK=1 bash scripts/quick_build.sh > "$QB" 2>&1 \
    || { echo "[runmask-smoke] build FAILED"; tail -25 "$QB"; exit 1; }

if ! grep -qF 'Link OK -- no unresolved symbols' "$QB"; then
    echo "[runmask-smoke] LINK gate FAILED:"; grep -A8 -F 'LINK FAILED' "$QB" | head -20
    exit 1
fi
if grep -qE '^FAIL: ' "$QB"; then
    echo "[runmask-smoke] COMPILE gate FAILED:"; grep -B1 -A6 -E '^FAIL: ' "$QB" | head -30
    exit 1
fi
echo "[runmask-smoke] LINK gate OK"

if [ ! -s iso/boot/initrd.img ]; then echo "[runmask-smoke] initrd missing"; exit 1; fi
if [ ! -s build/kernel.elf ]; then echo "[runmask-smoke] default kernel.elf missing"; exit 1; fi

echo "[runmask-smoke] assembling ISO ..."
cp build/kernel-smp.elf iso/boot/kernel.elf
grub-mkrescue -o build/automationos-runmask.iso iso/ 2>/dev/null \
    || { echo "[runmask-smoke] grub-mkrescue FAILED"; cp build/kernel.elf iso/boot/kernel.elf; exit 1; }
cp build/kernel.elf iso/boot/kernel.elf

rm -f "$SER"
echo "[runmask-smoke] booting qemu -smp 2 (timeout ${QEMU_TIMEOUT}s) ..."
timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
    -cdrom build/automationos-runmask.iso \
    -m 512 -smp 2 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== gate 2: RUNMASK (core + plant + exit record + upgraded NEG) ==="
grep -F 'RUNMASK-CORE' "$SER" | head -1
grep -F '[RUNMASK] exit record' "$SER" | head -3
grep -F 'TLBSHOOT_NEG' "$SER" | head -1

CORE_OK=0
grep -qF 'RUNMASK-CORE: PASS baseline_clean=1 forced_crosscpu_detected=1 restored_clean=1' "$SER" && CORE_OK=1
NVIOL=$(grep -cF '[RUNMASK] VIOLATION' "$SER" || true)
PLANT_OK=0; [ "$NVIOL" = "1" ] && PLANT_OK=1
EXREC_OK=0
grep -qE "\[RUNMASK\] exit record: pid=[0-9]+ 'batchdemo' allowed=0x3 ran=0x[0-9a-f]+ single_cpu=1" "$SER" && EXREC_OK=1
NEG_OK=0
grep -qF 'TLBSHOOT_NEG: PASS no_user_crossflush_needed_under_pinning=1' "$SER" && \
grep -qF '(RUNMASK upgrade:' "$SER" && NEG_OK=1
NCROSS=$(grep -oE 'exit record: .* single_cpu=0' "$SER" | wc -l)
echo "  core=$CORE_OK plant_loud_once=$PLANT_OK(viol_lines=$NVIOL) batchdemo_exit_record=$EXREC_OK crosscpu_exits=$NCROSS neg_upgraded=$NEG_OK"

echo "=== gate 3: the full regression ladder ==="
BC_OK=0
grep -qF 'BATCHCLASS-CORE: PASS batch_cpu1=1 batch_mask_respected=1 normal_cpu0=1 pinned_rt_cpu1=1 illegal_clamped=1' "$SER" && BC_OK=1
SEAM_OK=0
grep -qE '\[SMP\] F3-7: batchdemo PID [0-9]+ class=BATCH unpinned -> the seam chose cpu1' "$SER" && SEAM_OK=1
ST_DONE=$(grep -cE 'BKLSTORM pid=[0-9]+ done iters=[0-9]+ errors=0 secs=' "$SER" || true)
ENG=0; grep -qE '\[BKL\] engaged' "$SER" && ENG=1
NDEAD=$(grep -cE '\[BKL\] (possible deadlock|BUG)' "$SER" || true)
PR_OK=0
grep -qF 'SMPPROFILE-CORE: PASS normal_home=1 batch_declared=1 pinned_rt_legal=1 no_behavior_change=1' "$SER" && PR_OK=1
CC_OK=0
grep -qF 'CHOOSECPU: PASS pinned_cpu1=1 default_cpu0=1 illegal_clamped=1 nomask_clamped=1 multimask_home=1 cpu1only_role=1' "$SER" && CC_OK=1
TS_OK=0;  grep -qE 'TLBSHOOT: PASS kernel_flush=1 acked=1 bounded=1 invariant=1' "$SER" && TS_OK=1
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
echo "  batchclass=$BC_OK seam=$SEAM_OK storms=$ST_DONE/2 bkl=(eng=$ENG warn=$NDEAD) profile=$PR_OK choosecpu=$CC_OK tlbshoot=$TS_OK wake=$WK_OK ipilink=$IPI_OK F2=$F2_OK APCURRENT=$AC_OK marks=$MARKS exit42=$EXIT42 reaped=$REAPED alive=$ALIVE sched_viol=$NSCHED tlb_viol=$NTLBV panic=$NPANIC"

if [ "$CORE_OK" = "1" ] && [ "$PLANT_OK" = "1" ] && [ "$EXREC_OK" = "1" ] && \
   [ "$NEG_OK" = "1" ] && [ "$NCROSS" = "0" ] && \
   [ "$BC_OK" = "1" ] && [ "$SEAM_OK" = "1" ] && [ "$ST_DONE" = "2" ] && \
   [ "$ENG" = "1" ] && [ "$NDEAD" = "0" ] && \
   [ "$PR_OK" = "1" ] && [ "$CC_OK" = "1" ] && [ "$TS_OK" = "1" ] && \
   [ "$NTLBV" = "0" ] && [ "$WK_OK" = "1" ] && [ "$IPI_OK" = "1" ] && \
   [ "$F2_OK" = "1" ] && [ "$AC_OK" = "1" ] && [ "$MARKS" -ge 1 ] && \
   [ "$EXIT42" = "1" ] && [ "$REAPED" = "1" ] && [ "$ALIVE" = "1" ] && \
   [ "$NSCHED" = "0" ] && [ "$NPANIC" = "0" ]; then
    echo "RUNMASK: PASS declared_multimask_ok=1 actual_single_cpu=1 forced_crosscpu_detected=1 tlb_neg_valid=1"
    echo "[runmask-smoke] RESULT: PASS -- the audit audits reality; the forcing function is armed for DESKTOP-SPLIT"
    exit 0
else
    echo "RUNMASK: FAIL core=$CORE_OK plant=$PLANT_OK(viol=$NVIOL) exitrec=$EXREC_OK crossexits=$NCROSS neg=$NEG_OK batchclass=$BC_OK seam=$SEAM_OK storms=$ST_DONE bkl=($ENG/$NDEAD) ladder=(pr=$PR_OK cc=$CC_OK ts=$TS_OK wake=$WK_OK ipi=$IPI_OK f2=$F2_OK ac=$AC_OK marks=$MARKS exit42=$EXIT42 reaped=$REAPED) alive=$ALIVE sched=$NSCHED tlb=$NTLBV panic=$NPANIC"
    echo "--- last 50 serial lines ---"; tail -50 "$SER"
    exit 1
fi
