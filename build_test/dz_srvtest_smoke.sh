#!/bin/bash
# MP-PHANTOM-FIX proof: run the authoritative server's headless self-test
# (deadzoned -t), which now also asserts that INACTIVE player slots serialize as
# DZ_SLOT_EMPTY -- so the GUI client's `if (id==DZ_SLOT_EMPTY) continue;` guard
# fires and no ghost teammate is rendered for an empty slot. Build init
# -DDZ_SRVTEST (init runs `deadzoned -t` and waits for it).
# Run: wsl -d Arch bash build_test/dz_srvtest_smoke.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[dzsrv] quick_build (default kernel)..."
bash scripts/quick_build.sh > /tmp/dzsrv_qb.log 2>&1
grep -q SUCCESS /tmp/dzsrv_qb.log || { echo "kernel build failed"; tail -6 /tmp/dzsrv_qb.log; exit 1; }

echo "[dzsrv] build_all DZ_SRVTEST=1..."
DZ_SRVTEST=1 bash scripts/build_all.sh > /tmp/dzsrv_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/dzsrv_ba.log; then
  echo "BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/dzsrv_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/dzsrv_ser.log; rm -f "$SER"
echo "[dzsrv] booting (80s)..."
timeout 80 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== server selftest markers ==="
grep -aE 'DEADZONED SELFTEST' "$SER" | head
PASS=$(grep -aoE 'DEADZONED SELFTEST: PASS.*phantoms_ok=1' "$SER" | head -1)
echo ""
if [ -n "$PASS" ]; then
  echo "DZ-SRVTEST: PASS (inactive slots serialize as DZ_SLOT_EMPTY -> no phantom teammates)"
  echo "  $PASS"
  exit 0
else
  echo "DZ-SRVTEST: FAIL"
  echo "--- server lines ---"; grep -aE 'DEADZONED' "$SER" | head -20
  echo "--- tail ---"; tail -15 "$SER"
  exit 1
fi
