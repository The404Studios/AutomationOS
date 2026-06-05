#!/bin/bash
# smp_smoke.sh -- the 2-CPU SMP proof vehicle.
# =============================================================================
# Builds the SMP kernel (SMP_FOUNDATION: CPU1 coprocessor + SYS_CPU1_OFFLOAD),
# packages it into an ISO with the SAME initrd build_all already produced (which
# contains sbin/smpstress), boots it under `qemu -smp 2`, and verifies the stress
# harness reports SMPSTRESS: PASS -- i.e. CPU1 ran thousands of dispatched jobs
# with the exact result and ZERO lost wakeups.
#
# The default smoke (scripts/smoke_boot.sh) boots the single-core kernel where
# smpstress SKIPs; THIS script is the separate run that proves the real 2-CPU path.
# It must be the green light BEFORE any per-CPU scheduler / dispatch change lands.
#
# Prereq: run `IDE=1 bash scripts/build_all.sh` first so iso/boot/initrd.img holds
# the current sbin/smpstress. Run: wsl -d Arch bash -lc 'bash scripts/smp_smoke.sh'
set -u
ROOT=/mnt/c/Users/wilde/Desktop/Kernel
cd "$ROOT" || exit 9

QEMU_TIMEOUT="${QEMU_TIMEOUT:-240}"
SER=/tmp/smp_serial.log

echo "[smp-smoke] building kernel-smp.elf (SMP=1) ..."
SMP=1 bash scripts/quick_build.sh > /tmp/smp_qb.log 2>&1 \
    || { echo "[smp-smoke] SMP build FAILED"; tail -20 /tmp/smp_qb.log; exit 1; }

if [ ! -s iso/boot/initrd.img ]; then
    echo "[smp-smoke] iso/boot/initrd.img missing/empty -- run build_all first"; exit 1
fi
if [ ! -s build/kernel.elf ]; then
    echo "[smp-smoke] build/kernel.elf missing (need it to restore the iso tree)"; exit 1
fi

echo "[smp-smoke] assembling SMP ISO (kernel-smp.elf + existing initrd) ..."
cp build/kernel-smp.elf iso/boot/kernel.elf
grub-mkrescue -o build/automationos-smp.iso iso/ 2>/dev/null \
    || { echo "[smp-smoke] grub-mkrescue FAILED"; cp build/kernel.elf iso/boot/kernel.elf; exit 1; }
# Restore the default kernel in the iso tree so a later default smoke isn't poisoned.
cp build/kernel.elf iso/boot/kernel.elf

rm -f "$SER"
echo "[smp-smoke] booting qemu -smp 2 (timeout ${QEMU_TIMEOUT}s) ..."
timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
    -cdrom build/automationos-smp.iso \
    -m 512 -smp 2 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== [SMP] counter lines ==="
grep -F '[SMP] dispatch=' "$SER" | tail -4
echo "=== SMPSTRESS result ==="
grep -F 'SMPSTRESS:' "$SER" | tail -3

if grep -qF 'SMPSTRESS: PASS' "$SER"; then
    echo "[smp-smoke] RESULT: PASS -- CPU1 ran the dispatch stress correctly under -smp 2"
    exit 0
else
    echo "[smp-smoke] RESULT: FAIL -- no 'SMPSTRESS: PASS' in the serial log"
    echo "--- last 30 serial lines ---"; tail -30 "$SER"
    exit 1
fi
