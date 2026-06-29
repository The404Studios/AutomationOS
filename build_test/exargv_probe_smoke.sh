#!/bin/bash
# EX_ARGV-PROBE proof: the compositor's project-icon spawn convention
# (SYS_SPAWN_EX_ARGV: NUL-separated argv[1..] byte buffer + byte length + explicit
# stdio handles) delivers a single argv[1] with embedded spaces INTACT -- no
# whitespace split. init -DEX_ARGV_PROBE spawns argvtest with "/Desktop/Projects/My
# Game"; argvtest echoes its argv. If the ABI is correct the spaced path arrives as
# ONE contiguous argv[1]; if it were the legacy/buggy path it would split into
# "[/Desktop/Projects/My] [Game]" and the contiguous "My Game" would NOT appear.
# This is the headless coverage for the compositor DI_PROJECT -> sbin/ide spawn that
# no other harness exercises. Run: wsl -d Arch bash build_test/exargv_probe_smoke.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[exargv] quick_build (default kernel)..."
bash scripts/quick_build.sh > /tmp/exargv_qb.log 2>&1
grep -q SUCCESS /tmp/exargv_qb.log || { echo "kernel build failed"; tail -6 /tmp/exargv_qb.log; exit 1; }

echo "[exargv] build_all EX_ARGV_PROBE=1..."
EX_ARGV_PROBE=1 bash scripts/build_all.sh > /tmp/exargv_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/exargv_ba.log; then
  echo "BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/exargv_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/exargv_ser.log; rm -f "$SER"
echo "[exargv] booting (80s)..."
timeout 80 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== argvtest EX_ARGV markers ==="
grep -aE 'EX_ARGV_PROBE|ARGVTEST:' "$SER" | head
echo ""
# argvtest writes each argv entry as an atomic SYS_WRITE, so the spaced argv[1]
# appears as a contiguous "/Desktop/Projects/My Game" ONLY when delivered un-split.
if grep -aq '/Desktop/Projects/My Game' "$SER"; then
  echo "EX_ARGV-PROBE: PASS (SYS_SPAWN_EX_ARGV delivered a spaced argv[1] intact -- no whitespace split)"
  exit 0
else
  echo "EX_ARGV-PROBE: FAIL (spaced argv[1] was split or dropped)"
  echo "--- argvtest lines ---"; grep -aE 'ARGVTEST' "$SER" | head
  echo "--- tail ---"; tail -15 "$SER"
  exit 1
fi
