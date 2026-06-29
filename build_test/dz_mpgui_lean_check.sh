#!/bin/bash
# DZ_MPGUI_DEMO + DESKTOP_MINIMAL: lean-boot 2-window co-op demo, with a HEADLESS
# RENDER PROOF. Each window prints "DEADZONE: coop slot=S drew_cyan=N" = how many
# cyan teammate sprites it actually drew (passed projection) -- N>=1 proves the
# co-op teammate is VISIBLE on screen, no eyes/QEMU-display needed. Also asserts
# both windows joined + the lean desktop booted clean.
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
echo "[lean] quick_build (default kernel)..."
bash scripts/quick_build.sh > /tmp/lean_qb.log 2>&1
grep -q SUCCESS /tmp/lean_qb.log || { echo "kernel build failed"; tail -6 /tmp/lean_qb.log; exit 1; }
echo "[lean] build_all DESKTOP_MINIMAL=1 DZ_MPGUI_DEMO=1..."
DESKTOP_MINIMAL=1 DZ_MPGUI_DEMO=1 bash scripts/build_all.sh > /tmp/lean_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/lean_ba.log; then
  echo "BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/lean_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }
SER=build_test/lean_ser.log; rm -f "$SER"
echo "[lean] booting headless (90s)..."
timeout 90 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null
echo "=== markers ==="
grep -aE 'DEADZONED: (listening|client joined)|DEADZONE: (mp connected|coop slot=)' "$SER" | head -16
JOINS=$(grep -acE 'DEADZONED: client joined slot=' "$SER")
CONN=$(grep -acE 'DEADZONE: mp connected' "$SER")
FAULT=$(grep -aciE 'PANIC|TRIPLE FAULT' "$SER")
# the render proof: each window must draw >=1 cyan teammate at some point.
CYAN0=$(grep -aoE 'coop slot=0 drew_cyan=[1-9][0-9]*' "$SER" | head -1)
CYAN1=$(grep -aoE 'coop slot=1 drew_cyan=[1-9][0-9]*' "$SER" | head -1)
echo ""
echo "joins=$JOINS connected=$CONN fault=$FAULT"
echo "render proof: slot0='$CYAN0'  slot1='$CYAN1'"
echo ""
if [ "$JOINS" -ge 2 ] && [ "$CONN" -ge 2 ] && [ "$FAULT" -eq 0 ] && [ -n "$CYAN0" ] && [ -n "$CYAN1" ]; then
  echo "DZ-LEAN-DEMO: PASS -- both windows joined AND each DREW a cyan teammate sprite (co-op render visible)"
  exit 0
else
  echo "DZ-LEAN-DEMO: FAIL (joins=$JOINS conn=$CONN fault=$FAULT cyan0='$CYAN0' cyan1='$CYAN1')"
  echo "--- all coop lines ---"; grep -aE 'DEADZONE: coop slot=' "$SER" | head -20
  echo "--- tail ---"; tail -15 "$SER"
  exit 1
fi
