#!/bin/bash
# MP-HELLO-0 proof: the server's DZ_HELLO join-ack hands two clients DISTINCT
# slots. Build DZ_MPLIVE=1 (init spawns deadzoned + ONE dzclient) DZ_MP2=1 (that
# dzclient opens TWO connections internally). Assert TWO "client joined slot="
# lines + "DEADZONE: mp2 PASS slotA=0 slotB=1 ...". Run: wsl -d Arch bash build_test/dz_mp2_smoke.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[dzmp2] quick_build (default kernel)..."
bash scripts/quick_build.sh > /tmp/dzmp2_qb.log 2>&1
grep -q SUCCESS /tmp/dzmp2_qb.log || { echo "kernel build failed"; tail -6 /tmp/dzmp2_qb.log; exit 1; }

echo "[dzmp2] build_all DZ_MPLIVE=1 DZ_MP2=1..."
DZ_MPLIVE=1 DZ_MP2=1 bash scripts/build_all.sh > /tmp/dzmp2_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/dzmp2_ba.log; then
  echo "BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/dzmp2_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/dzmp2_ser.log; rm -f "$SER"
echo "[dzmp2] booting (90s)..."
timeout 90 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== MP markers ==="
grep -aE 'DEADZONED: (listening|client joined)|DEADZONE: mp2' "$SER" | head
JOINS=$(grep -acE 'DEADZONED: client joined slot=' "$SER")
PASS=$(grep -aoE 'DEADZONE: mp2 PASS[^\n]*' "$SER" | head -1)
echo ""
if [ -n "$PASS" ] && [ "$JOINS" -ge 2 ]; then
  echo "DZ-MP2: PASS (DZ_HELLO gave two clients distinct slots; both drove the server)"
  echo "  joins=$JOINS  $PASS"
  exit 0
else
  echo "DZ-MP2: FAIL (joins=$JOINS pass='$PASS')"
  echo "--- MP lines ---"; grep -aE 'DEADZONE|DEADZONED' "$SER" | head -25
  echo "--- tail ---"; tail -15 "$SER"
  exit 1
fi
