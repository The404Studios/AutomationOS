#!/bin/bash
# cpu1_smoke.sh -- the SMP_SCHED_DISPATCH (CPU1 scheduler-mode) proof vehicle.
# =============================================================================
# Builds the kernel with SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 -- the AP
# scheduler-mode dispatch build where CPU1 leaves coprocessor mode, runs
# ap_scheduler_loop, and context-switches into the F2 pinned ring-0 kthread.
# Boots under `qemu -smp 2` and gates:
#   - Brick F2 VERIFY delta>0   (CPU1 did the first AP context switch)
#   - APCURRENT: PASS           (F3-4: the kthread, ON CPU1, called the real
#                                process_get_current() and proved it resolves
#                                cpu1-local, distinct from CPU0's current --
#                                the AP per-cpu "current" boundary proof)
#   - 0 [SCHED_INVARIANT]       (the F3-0..F3-3a validators stay clean)
#   - no PANIC / CPU EXCEPTION / triple-fault
#
# This is the SEPARATE, gated dispatch run. The default smoke and smp_smoke.sh
# (SMP=1 = SMP_FOUNDATION only) do NOT compile SMP_SCHED_DISPATCH, so APCURRENT
# never fires there -- it is asserted ONLY here. Prereq: iso/boot/initrd.img and
# build/kernel.elf exist (run `IDE=1 bash scripts/build_all.sh` first).
# Run: wsl -d Arch bash -lc 'bash scripts/cpu1_smoke.sh'
set -u
ROOT=/mnt/c/Users/wilde/Desktop/Kernel
cd "$ROOT" || exit 9

QEMU_TIMEOUT="${QEMU_TIMEOUT:-240}"
SER=/tmp/cpu1_serial.log

echo "[cpu1-smoke] building kernel-smp.elf (SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1) ..."
SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 bash scripts/quick_build.sh > /tmp/cpu1_qb.log 2>&1 \
    || { echo "[cpu1-smoke] DISPATCH build FAILED"; tail -25 /tmp/cpu1_qb.log; exit 1; }

if [ ! -s iso/boot/initrd.img ]; then
    echo "[cpu1-smoke] iso/boot/initrd.img missing -- run build_all first"; exit 1
fi
if [ ! -s build/kernel.elf ]; then
    echo "[cpu1-smoke] build/kernel.elf missing (need it to restore the iso tree)"; exit 1
fi

echo "[cpu1-smoke] assembling DISPATCH ISO (kernel-smp.elf + existing initrd) ..."
cp build/kernel-smp.elf iso/boot/kernel.elf
grub-mkrescue -o build/automationos-cpu1.iso iso/ 2>/dev/null \
    || { echo "[cpu1-smoke] grub-mkrescue FAILED"; cp build/kernel.elf iso/boot/kernel.elf; exit 1; }
# Restore the default kernel in the iso tree so a later default smoke isn't poisoned.
cp build/kernel.elf iso/boot/kernel.elf

rm -f "$SER"
echo "[cpu1-smoke] booting qemu -smp 2 (timeout ${QEMU_TIMEOUT}s) ..."
timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
    -cdrom build/automationos-cpu1.iso \
    -m 512 -smp 2 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== Brick F2 VERIFY (AP context switch) ==="
grep -F 'Brick F2 VERIFY' "$SER" | tail -2
F2_OK=0
# delta=<n> with n>0 proves CPU1 ran the kthread. Accept any non-zero delta.
if grep -qE 'Brick F2 VERIFY:.*delta=[1-9][0-9]*' "$SER"; then F2_OK=1; fi

echo "=== APCURRENT (F3-4 AP per-cpu current boundary) ==="
grep -F 'APCURRENT' "$SER" | tail -2
AC_OK=0; grep -qF 'APCURRENT: PASS' "$SER" && AC_OK=1

echo "=== scheduler invariant guard ==="
grep -F '[SCHED_INVARIANT]' "$SER" | tail -3 || echo "  (clean)"
NSCHED=$(grep -cF '[SCHED_INVARIANT]' "$SER" || true)

echo "=== fault scan ==="
NPANIC=$(grep -cE 'PANIC|CPU EXCEPTION|TRIPLE FAULT|KERNEL PANIC' "$SER" || true)

if [ "$F2_OK" = "1" ] && [ "$AC_OK" = "1" ] && [ "$NSCHED" = "0" ] && [ "$NPANIC" = "0" ]; then
    echo "[cpu1-smoke] RESULT: PASS -- F2 dispatch + APCURRENT PASS + 0 sched-invariant + 0 panic"
    exit 0
else
    echo "[cpu1-smoke] RESULT: FAIL -- F2_OK=$F2_OK APCURRENT_OK=$AC_OK sched_violations=$NSCHED panics=$NPANIC"
    echo "--- last 40 serial lines ---"; tail -40 "$SER"
    exit 1
fi
