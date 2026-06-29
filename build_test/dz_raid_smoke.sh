#!/bin/bash
# Extraction/raid-loop proof: build init -DDZ_GAMETEST so it spawns sbin/deadzone,
# whose launch-time raid_selftest drives a real collect->extract (stash kept) and a
# collect->die (stash lost). Assert "DEADZONE: raid PASS" AND that the four prior
# selftests (systems/mp/loot/audio) are undisturbed. Run: wsl -d Arch bash build_test/dz_raid_smoke.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[dzraid] quick_build (default kernel)..."
bash scripts/quick_build.sh > /tmp/dzraid_qb.log 2>&1
grep -q SUCCESS /tmp/dzraid_qb.log || { echo "kernel build failed"; tail -6 /tmp/dzraid_qb.log; exit 1; }

echo "[dzraid] build_all DZ_GAMETEST=1 (init spawns sbin/deadzone)..."
DZ_GAMETEST=1 bash scripts/build_all.sh > /tmp/dzraid_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/dzraid_ba.log; then
  echo "USERSPACE BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/dzraid_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/dzraid_ser.log; rm -f "$SER"
echo "[dzraid] booting (80s)..."
timeout 80 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== DeadZone selftest markers ==="
grep -aE 'DEADZONE: (systems|mp|loot|audio|raid)' "$SER" | head
RAID=$(grep -aoE 'DEADZONE: raid PASS[^\n]*' "$SER" | head -1)
P=1
grep -qaE 'DEADZONE: systems PASS' "$SER" || P=0
grep -qaE 'DEADZONE: mp PASS'      "$SER" || P=0
grep -qaE 'DEADZONE: loot PASS'    "$SER" || P=0
grep -qaE 'DEADZONE: (audio PASS|audio SKIP)' "$SER" || P=0
[ -n "$RAID" ] || P=0
if grep -qaiE 'PANIC|TRIPLE FAULT' "$SER"; then echo "KERNEL FAULT"; P=0; fi
echo ""
if [ "$P" = "1" ]; then
  echo "DZ-RAID: PASS (extraction/raid lifecycle proven; prior selftests intact)"
  echo "  $RAID"
  exit 0
else
  echo "DZ-RAID: FAIL"
  echo "--- DeadZone lines ---"; grep -aE 'DEADZONE' "$SER" | head -20
  echo "--- tail ---"; tail -15 "$SER"
  exit 1
fi
