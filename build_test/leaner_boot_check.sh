#!/bin/bash
# LEANER-BOOT-0 proof: the two anchors of the default-flip.
#   A) a bare `build_all` now boots the LEAN desktop -- the persistent apps
#      spawn, the ~70-app self-test storm does NOT (no FORKTEST/CRYPTOTEST/
#      SIGTEST markers), and the boot is clean (desktop up, no fault).
#   B) `FULL=1 build_all` still gives the complete storm -> smoke_boot 43/43.
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 9

echo "=== [A] bare build_all -> LEAN boot (default is now lean) ==="
bash scripts/quick_build.sh > /tmp/lb_qb.log 2>&1
grep -qF 'SUCCESS: build/kernel.elf' /tmp/lb_qb.log || { echo "kernel build failed"; exit 1; }
bash scripts/build_all.sh > /tmp/lb_ba_lean.log 2>&1
grep -qE 'error:|undefined reference' /tmp/lb_ba_lean.log && { echo "lean build errors"; exit 1; }
grep -qF 'LEAN build (default)' /tmp/lb_ba_lean.log || { echo "  build_all did not report LEAN default"; exit 1; }
SER=/tmp/lb_lean_ser.log; rm -f "$SER"
timeout 60 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev user,id=n0 -device e1000,netdev=n0 -serial "file:$SER" -display none -no-reboot 2>/dev/null
LEAN=1
grep -qaE 'entering frame loop|All services started|compositor' "$SER" || { echo "  lean: desktop not up"; LEAN=0; }
# The storm must be ABSENT in the lean default.
for m in 'FORKTEST' 'CRYPTOTEST' 'SIGTEST' 'POLLSELFTEST'; do
    if grep -qaF "$m" "$SER"; then echo "  lean: storm marker $m present (NOT lean!)"; LEAN=0; fi
done
grep -qiE 'KERNEL PANIC|TRIPLE FAULT' "$SER" && { echo "  lean: kernel fault"; LEAN=0; }
echo "  lean_desktop_up_no_storm=$LEAN"

echo "=== [B] FULL=1 build_all -> storm -> smoke_boot 43/43 ==="
FULL=1 bash scripts/build_all.sh > /tmp/lb_ba_full.log 2>&1
grep -qE 'error:|undefined reference' /tmp/lb_ba_full.log && { echo "full build errors"; exit 1; }
grep -qF 'FULL build' /tmp/lb_ba_full.log || { echo "  build_all did not report FULL"; exit 1; }
bash scripts/smoke_boot.sh > /tmp/lb_smoke.log 2>&1
grep -E 'Total checks:|Passed:|Failed:|RESULT' /tmp/lb_smoke.log | head -4
FULL=0; grep -qF 'RESULT: PASS' /tmp/lb_smoke.log && FULL=1

echo ""
if [ "$LEAN" = 1 ] && [ "$FULL" = 1 ]; then
    echo "LEANER-BOOT-0: PASS (bare build_all = lean desktop no storm; FULL=1 = storm + smoke 43/43)"
    exit 0
else
    echo "LEANER-BOOT-0: FAIL (lean=$LEAN full=$FULL)"; exit 1
fi
