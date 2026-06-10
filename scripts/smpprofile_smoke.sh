#!/bin/bash
# smpprofile_smoke.sh -- the SMP-PROFILE-0 proof vehicle.
# =============================================================================
# Same build profile as the whole G/F ladder (SMP=1 SMP_SCHED=1
# SMP_SCHED_DISPATCH=1 SMP_IPI=1). PROFILE-0 lands TYPED INTENT: sched_class_t
# (NORMAL/BATCH/PINNED_RT + reserved INTERACTIVE/RECOVERY), cpu_role_t, the
# sched_profile_t field (gated, end-of-struct, memset->NORMAL), and THE NAMED
# funnel scheduler_submit_task (choose_cpu + legality re-assert + the gated
# enqueue sink). NO behavior change: BATCH is data, layer 3 stays a stub.
#
# Gates:
#   1. LINK gate     -- "Link OK" grepped (never trust rc).
#   2. PROFILE gates (serial):
#        a. SMPPROFILE-CORE: PASS normal_home=1 batch_declared=1
#           pinned_rt_legal=1 no_behavior_change=1     (kernel-printed,
#           synthetic-branch proof -- a synthetic shell never enters a real
#           runqueue, so the funnel flag is proven by (b) instead)
#        b. submit_funnel: BOTH live placements route through the named
#           funnel -- "[SCHED] submit: ... class=2 -> cpu1" appears for the
#           F2 kthread AND cpu1hello (class 2 = SCHED_CLASS_PINNED_RT).
#      The composite user acceptance line is assembled here (the F3-5/G1
#      convention):
#        SMPPROFILE: PASS normal_home=1 batch_declared=1 pinned_rt_legal=1
#                         submit_funnel=1 no_behavior_change=1
#   3. REGRESSION gate -- the full ladder: CHOOSECPU + live F3-6 marker +
#      TLBSHOOT(+NEG) + IPIWAKE pings + IPILINK + F2 + APCURRENT + CPU1HELLO
#      exit/reap + 0 sched/tlb invariant + desktop alive + 0 panic. The
#      ladder still passing IS no_behavior_change at system level.
#
# Run: wsl -d Arch bash -lc 'bash scripts/smpprofile_smoke.sh'
set -u
ROOT=/mnt/c/Users/wilde/Desktop/Kernel
cd "$ROOT" || exit 9

QEMU_TIMEOUT="${QEMU_TIMEOUT:-240}"
SER=/tmp/smpprofile_serial.log
QB=/tmp/smpprofile_qb.log

echo "[smpprofile-smoke] building kernel-smp.elf (SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1) ..."
SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 bash scripts/quick_build.sh > "$QB" 2>&1 \
    || { echo "[smpprofile-smoke] build FAILED"; tail -25 "$QB"; exit 1; }

if ! grep -qF 'Link OK -- no unresolved symbols' "$QB"; then
    echo "[smpprofile-smoke] LINK gate FAILED:"; grep -A8 -F 'LINK FAILED' "$QB" | head -20
    exit 1
fi
if grep -qE '^FAIL: ' "$QB"; then
    echo "[smpprofile-smoke] COMPILE gate FAILED:"; grep -B1 -A6 -E '^FAIL: ' "$QB" | head -30
    exit 1
fi
echo "[smpprofile-smoke] LINK gate OK"

if [ ! -s iso/boot/initrd.img ]; then echo "[smpprofile-smoke] initrd missing"; exit 1; fi
if [ ! -s build/kernel.elf ];   then echo "[smpprofile-smoke] default kernel.elf missing"; exit 1; fi

echo "[smpprofile-smoke] assembling ISO ..."
cp build/kernel-smp.elf iso/boot/kernel.elf
grub-mkrescue -o build/automationos-smpprofile.iso iso/ 2>/dev/null \
    || { echo "[smpprofile-smoke] grub-mkrescue FAILED"; cp build/kernel.elf iso/boot/kernel.elf; exit 1; }
cp build/kernel.elf iso/boot/kernel.elf

rm -f "$SER"
echo "[smpprofile-smoke] booting qemu -smp 2 (timeout ${QEMU_TIMEOUT}s) ..."
timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
    -cdrom build/automationos-smpprofile.iso \
    -m 512 -smp 2 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== gate 2a: SMPPROFILE-CORE (kernel-printed synthetic proof) ==="
grep -F 'SMPPROFILE-CORE' "$SER" | head -1
CORE_OK=0
grep -qF 'SMPPROFILE-CORE: PASS normal_home=1 batch_declared=1 pinned_rt_legal=1 no_behavior_change=1' "$SER" && CORE_OK=1

echo "=== gate 2b: the named funnel (live placements, class=2 = PINNED_RT) ==="
grep -F '[SCHED] submit:' "$SER" | head -4
FUNNEL_OK=0
KSUB=$(grep -cE "\[SCHED\] submit: pid=[0-9]+ 'ap_ktest' class=2 -> cpu1" "$SER" || true)
HSUB=$(grep -cE "\[SCHED\] submit: pid=[0-9]+ 'cpu1hello' class=2 -> cpu1" "$SER" || true)
[ "${KSUB:-0}" -ge 1 ] && [ "${HSUB:-0}" -ge 1 ] && FUNNEL_OK=1

echo "=== gate 3: the full regression ladder ==="
CC_OK=0
grep -qF 'CHOOSECPU: PASS pinned_cpu1=1 default_cpu0=1 illegal_clamped=1 nomask_clamped=1 multimask_home=1 cpu1only_role=1' "$SER" && CC_OK=1
LIVE_OK=0
grep -qF 'cpu1hello placed via scheduler_choose_cpu -> cpu1' "$SER" && LIVE_OK=1
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
echo "  choosecpu=$CC_OK live_f36=$LIVE_OK tlbshoot=$TS_OK neg=$NEG_OK wake=$WAKE_OK ipilink=$IPI_OK F2=$F2_OK APCURRENT=$AC_OK marks=$MARKS exit42=$EXIT42 reaped=$REAPED alive=$ALIVE sched_viol=$NSCHED tlb_viol=$NTLBV panic=$NPANIC"

if [ "$CORE_OK" = "1" ] && [ "$FUNNEL_OK" = "1" ] && \
   [ "$CC_OK" = "1" ] && [ "$LIVE_OK" = "1" ] && \
   [ "$TS_OK" = "1" ] && [ "$NEG_OK" = "1" ] && [ "$NTLBV" = "0" ] && \
   [ "$WAKE_OK" = "1" ] && [ "$IPI_OK" = "1" ] && \
   [ "$F2_OK" = "1" ] && [ "$AC_OK" = "1" ] && [ "$MARKS" -ge 1 ] && \
   [ "$EXIT42" = "1" ] && [ "$REAPED" = "1" ] && [ "$ALIVE" = "1" ] && \
   [ "$NSCHED" = "0" ] && [ "$NPANIC" = "0" ]; then
    echo "SMPPROFILE: PASS normal_home=1 batch_declared=1 pinned_rt_legal=1 submit_funnel=1 no_behavior_change=1"
    echo "[smpprofile-smoke] RESULT: PASS -- typed intent landed, nothing moved"
    exit 0
else
    echo "SMPPROFILE: FAIL core=$CORE_OK funnel=$FUNNEL_OK(k=$KSUB h=$HSUB) choosecpu=$CC_OK live=$LIVE_OK tlbshoot=$TS_OK neg=$NEG_OK wake=$WAKE_OK ipilink=$IPI_OK f35=(f2=$F2_OK ac=$AC_OK marks=$MARKS exit42=$EXIT42 reaped=$REAPED) alive=$ALIVE sched=$NSCHED tlb=$NTLBV panic=$NPANIC"
    echo "--- last 40 serial lines ---"; tail -40 "$SER"
    exit 1
fi
