#!/bin/bash
# bkl_smoke.sh -- the SMP-H1 BKL-LITE proof vehicle.
# =============================================================================
# Build: the full ladder profile + SMP_BKL=1. Boots qemu -smp 2 and lets the
# two 60-second bklstorm instances (CPU1-pinned PINNED_RT + CPU0-home NORMAL)
# hammer the MARKED syscall groups concurrently while the desktop comes up.
#
# Gates:
#   1. LINK gate    -- "Link OK" grepped (never trust rc).
#   2. STORM gates  (serial):
#        spawn markers   : '[SMP] H1: bklstorm PID n -> CPU1' + '-> CPU0'
#        engagement      : '[BKL] engaged: cpu0_acq=.. cpu1_acq=..' -- BOTH
#                          cpus executed marked syscalls under the wall
#                          (the acceptance's cpu0=1 cpu1=1)
#        completion x2   : 'BKLSTORM pid=.. done iters=.. errors=0 secs=S'
#                          with S >= 60 on both (duration=60s) and errors=0
#                          on both (corruption=0 -- the shm pattern verify)
#        deadlock=0      : no '[BKL] possible deadlock', no '[BKL] BUG'
#   3. REGRESSION gate -- the whole ladder (SMPPROFILE-CORE, CHOOSECPU,
#      TLBSHOOT+NEG, IPIWAKE pings, IPILINK, F2, APCURRENT, CPU1HELLO
#      exit/reap, 0 sched/tlb invariant, desktop alive, 0 panic) under the
#      new lock -- the BKL must not regress anything it wrapped.
#      ("verify heap/futex/rq locks still behave under 2-CPU stress" = the
#      storms' shm/heap churn + the ladder's validators staying silent.)
#
# The composite acceptance line is assembled here (the house convention):
#   BKL: PASS syscall_storm=1 duration=60s cpu0=1 cpu1=1 corruption=0
#        deadlock=0 panic=0
#
# Prereq: iso/boot/initrd.img CONTAINING sbin/bklstorm (IDE=1 build_all).
# Run: wsl -d Arch bash -lc 'bash scripts/bkl_smoke.sh'
set -u
ROOT=/mnt/c/Users/wilde/Desktop/Kernel
cd "$ROOT" || exit 9

QEMU_TIMEOUT="${QEMU_TIMEOUT:-300}"     # 60s storms + boot need headroom
SER=/tmp/bkl_serial.log
QB=/tmp/bkl_qb.log

echo "[bkl-smoke] building kernel-smp.elf (full ladder profile + SMP_BKL=1) ..."
SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 SMP_BKL=1 bash scripts/quick_build.sh > "$QB" 2>&1 \
    || { echo "[bkl-smoke] build FAILED"; tail -25 "$QB"; exit 1; }

if ! grep -qF 'Link OK -- no unresolved symbols' "$QB"; then
    echo "[bkl-smoke] LINK gate FAILED:"; grep -A8 -F 'LINK FAILED' "$QB" | head -20
    exit 1
fi
if grep -qE '^FAIL: ' "$QB"; then
    echo "[bkl-smoke] COMPILE gate FAILED:"; grep -B1 -A6 -E '^FAIL: ' "$QB" | head -30
    exit 1
fi
echo "[bkl-smoke] LINK gate OK"

if [ ! -s iso/boot/initrd.img ]; then echo "[bkl-smoke] initrd missing"; exit 1; fi
if ! grep -qc 'BKLSTORM pid=' iso/boot/initrd.img; then
    echo "[bkl-smoke] initrd does not contain sbin/bklstorm -- run IDE=1 build_all"; exit 1
fi
if [ ! -s build/kernel.elf ]; then echo "[bkl-smoke] default kernel.elf missing"; exit 1; fi

echo "[bkl-smoke] assembling ISO ..."
cp build/kernel-smp.elf iso/boot/kernel.elf
grub-mkrescue -o build/automationos-bkl.iso iso/ 2>/dev/null \
    || { echo "[bkl-smoke] grub-mkrescue FAILED"; cp build/kernel.elf iso/boot/kernel.elf; exit 1; }
cp build/kernel.elf iso/boot/kernel.elf

rm -f "$SER"
echo "[bkl-smoke] booting qemu -smp 2 (timeout ${QEMU_TIMEOUT}s; the storms run 60s) ..."
timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
    -cdrom build/automationos-bkl.iso \
    -m 512 -smp 2 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== gate 2: storm spawn + engagement + completion ==="
grep -E '\[SMP\] H1: bklstorm' "$SER" | head -2
grep -F '[BKL] engaged' "$SER" | head -1
grep -F 'BKLSTORM pid=' "$SER" | head -3

SP1=0; grep -qE '\[SMP\] H1: bklstorm PID [0-9]+ -> CPU1' "$SER" && SP1=1
SP0=0; grep -qE '\[SMP\] H1: bklstorm PID [0-9]+ -> CPU0' "$SER" && SP0=1
ENG=0; grep -qE '\[BKL\] engaged: cpu0_acq=[1-9][0-9]* cpu1_acq=[1-9][0-9]*' "$SER" && ENG=1

DONE=$(grep -cE 'BKLSTORM pid=[0-9]+ done iters=[0-9]+ errors=[0-9]+ secs=[0-9]+' "$SER" || true)
CORR=0
BADERR=$(grep -oE 'BKLSTORM pid=[0-9]+ done iters=[0-9]+ errors=[0-9]+' "$SER" | grep -cv 'errors=0' || true)
[ "${BADERR:-1}" = "0" ] && CORR=1
DUR=0
SHORT=$(grep -oE 'BKLSTORM pid=[0-9]+ done .* secs=[0-9]+' "$SER" | grep -oE 'secs=[0-9]+' | grep -oE '[0-9]+' | awk '$1 < 60' | wc -l)
[ "${SHORT:-1}" = "0" ] && [ "$DONE" = "2" ] && DUR=1
NDEAD=$(grep -cE '\[BKL\] (possible deadlock|BUG)' "$SER" || true)

echo "  spawn_cpu1=$SP1 spawn_cpu0=$SP0 engaged=$ENG completed=$DONE/2 corruption_free=$CORR duration_ok=$DUR bkl_warnings=$NDEAD"

echo "=== gate 3: the full regression ladder ==="
PR_OK=0
grep -qF 'SMPPROFILE-CORE: PASS normal_home=1 batch_declared=1 pinned_rt_legal=1 no_behavior_change=1' "$SER" && PR_OK=1
CC_OK=0
grep -qF 'CHOOSECPU: PASS pinned_cpu1=1 default_cpu0=1 illegal_clamped=1 nomask_clamped=1 multimask_home=1 cpu1only_role=1' "$SER" && CC_OK=1
TS_OK=0;  grep -qE 'TLBSHOOT: PASS kernel_flush=1 acked=1 bounded=1 invariant=1' "$SER" && TS_OK=1
NEG_OK=0; grep -qF 'TLBSHOOT_NEG: PASS no_user_crossflush_needed_under_pinning=1' "$SER" && NEG_OK=1
NTLBV=$(grep -cF '[TLB_INVARIANT] VIOLATION' "$SER" || true)
WAKE_OK=0; grep -qE 'ping summary acks=32/32 max_latency_us=[0-9]+' "$SER" && WAKE_OK=1
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
echo "  profile=$PR_OK choosecpu=$CC_OK tlbshoot=$TS_OK neg=$NEG_OK wake=$WAKE_OK ipilink=$IPI_OK F2=$F2_OK APCURRENT=$AC_OK marks=$MARKS exit42=$EXIT42 reaped=$REAPED alive=$ALIVE sched_viol=$NSCHED tlb_viol=$NTLBV panic=$NPANIC"

if [ "$SP1" = "1" ] && [ "$SP0" = "1" ] && [ "$ENG" = "1" ] && \
   [ "$DONE" = "2" ] && [ "$CORR" = "1" ] && [ "$DUR" = "1" ] && [ "$NDEAD" = "0" ] && \
   [ "$PR_OK" = "1" ] && [ "$CC_OK" = "1" ] && \
   [ "$TS_OK" = "1" ] && [ "$NEG_OK" = "1" ] && [ "$NTLBV" = "0" ] && \
   [ "$WAKE_OK" = "1" ] && [ "$IPI_OK" = "1" ] && \
   [ "$F2_OK" = "1" ] && [ "$AC_OK" = "1" ] && [ "$MARKS" -ge 1 ] && \
   [ "$EXIT42" = "1" ] && [ "$REAPED" = "1" ] && [ "$ALIVE" = "1" ] && \
   [ "$NSCHED" = "0" ] && [ "$NPANIC" = "0" ]; then
    echo "BKL: PASS syscall_storm=1 duration=60s cpu0=1 cpu1=1 corruption=0 deadlock=0 panic=0"
    echo "[bkl-smoke] RESULT: PASS -- the safety wall held under a 60s two-CPU storm"
    exit 0
else
    echo "BKL: FAIL storm=(cpu1=$SP1 cpu0=$SP0 engaged=$ENG done=$DONE corr=$CORR dur=$DUR warn=$NDEAD) ladder=(prof=$PR_OK cc=$CC_OK ts=$TS_OK neg=$NEG_OK wake=$WAKE_OK ipi=$IPI_OK f2=$F2_OK ac=$AC_OK marks=$MARKS exit42=$EXIT42 reaped=$REAPED) alive=$ALIVE sched=$NSCHED tlb=$NTLBV panic=$NPANIC"
    echo "--- last 50 serial lines ---"; tail -50 "$SER"
    exit 1
fi
