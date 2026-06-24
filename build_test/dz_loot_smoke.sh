#!/bin/bash
# Loot/equipment/durability proof: build init -DDZ_GAMETEST so it spawns sbin/deadzone,
# whose launch-time headless selftests (systems / mp / loot) print to serial.
# Assert "DEADZONE: loot PASS". Run: wsl -d Arch bash build_test/dz_loot_smoke.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[dzloot] quick_build (default kernel)..."
bash scripts/quick_build.sh > /tmp/dzloot_qb.log 2>&1
grep -q SUCCESS /tmp/dzloot_qb.log || { echo "kernel build failed"; tail -6 /tmp/dzloot_qb.log; exit 1; }

echo "[dzloot] build_all DZ_GAMETEST=1 (init spawns sbin/deadzone)..."
DZ_GAMETEST=1 bash scripts/build_all.sh > /tmp/dzloot_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/dzloot_ba.log; then
  echo "USERSPACE BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/dzloot_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/dzloot_ser.log; rm -f "$SER"
echo "[dzloot] booting (80s)..."
timeout 80 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== DeadZone selftest markers ==="
grep -aE 'DEADZONE: (systems|mp|loot)' "$SER" | head
LOOT=$(grep -aoE 'DEADZONE: loot PASS[^\n]*' "$SER" | head -1)
if [ -n "$LOOT" ]; then
  echo ""; echo "DZ-LOOT: PASS"; echo "  $LOOT"; exit 0
else
  echo ""; echo "DZ-LOOT: FAIL (no loot PASS marker)"
  echo "--- DeadZone lines ---"; grep -aE 'DEADZONE|deadzone|DZ_GAMETEST' "$SER" | head -30
  echo "--- tail ---"; tail -20 "$SER"
  exit 1
fi
