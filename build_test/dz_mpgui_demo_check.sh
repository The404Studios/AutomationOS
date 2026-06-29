#!/bin/bash
# DZ_MPGUI_DEMO headless verification: build the 2-window co-op demo ISO and boot it
# HEADLESS, asserting BOTH GUI DeadZone windows auto-JOINED the server (the live
# connection data-path). The on-screen cyan-teammate RENDER is QEMU-eyes-only; this
# proves the demo will actually be in co-op when launched graphically.
# Run: wsl -d Arch bash build_test/dz_mpgui_demo_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[dzgui] quick_build (default kernel)..."
bash scripts/quick_build.sh > /tmp/dzgui_qb.log 2>&1
grep -q SUCCESS /tmp/dzgui_qb.log || { echo "kernel build failed"; tail -6 /tmp/dzgui_qb.log; exit 1; }

echo "[dzgui] build_all DZ_MPGUI_DEMO=1..."
DZ_MPGUI_DEMO=1 bash scripts/build_all.sh > /tmp/dzgui_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/dzgui_ba.log; then
  echo "BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/dzgui_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/dzgui_ser.log; rm -f "$SER"
echo "[dzgui] booting headless (75s)..."
timeout 75 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== co-op markers ==="
grep -aE 'DZ_MPGUI_DEMO|DEADZONED: (listening|client joined)|DEADZONE: (ready|mp connected)' "$SER" | head -16
JOINS=$(grep -acE 'DEADZONED: client joined slot=' "$SER")
CONN=$(grep -acE 'DEADZONE: mp connected' "$SER")
READY=$(grep -acE 'DEADZONE: ready' "$SER")
echo ""
if [ "$JOINS" -ge 2 ] && [ "$CONN" -ge 2 ]; then
  echo "DZ-MPGUI-DEMO: PASS (ready=$READY joins=$JOINS connected=$CONN -- both windows live-joined the server)"
  exit 0
else
  echo "DZ-MPGUI-DEMO: FAIL (ready=$READY joins=$JOINS connected=$CONN; need >=2 joins + >=2 connected)"
  echo "--- DZ lines ---"; grep -aE 'DEADZONE|DEADZONED' "$SER" | head -25
  echo "--- tail ---"; tail -15 "$SER"
  exit 1
fi
