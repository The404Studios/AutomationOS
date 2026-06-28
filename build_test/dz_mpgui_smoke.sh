#!/bin/bash
# MP-GUI-1 proof: the live co-op GUI wiring (connect menu + per-frame
# send/apply + render-others + the wire<->fx coordinate bridge) is present and
# the single-player game is UNCHANGED with no connection (g_mp_on defaults 0).
# Headless can't validate the rendering itself (that needs QEMU eyes), so this
# asserts: deadzone.c builds into the ISO, boots, and ALL launch selftests stay
# green -- crucially "DEADZONE: mp PASS", which now also runs the new wire<->fx
# coordinate-bridge sanity check. Run: wsl -d Arch bash build_test/dz_mpgui_smoke.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[mpgui] quick_build (default kernel -- byte-unchanged by MP-GUI-1)..."
bash scripts/quick_build.sh > /tmp/mpgui_qb.log 2>&1
grep -q SUCCESS /tmp/mpgui_qb.log || { echo "KERNEL BUILD FAIL"; tail -8 /tmp/mpgui_qb.log; exit 1; }

echo "[mpgui] build_all DZ_GAMETEST=1 (init spawns sbin/deadzone -> launch selftests)..."
DZ_GAMETEST=1 bash scripts/build_all.sh > /tmp/mpgui_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/mpgui_ba.log; then
  echo "BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/mpgui_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/mpgui_ser.log; rm -f "$SER"
echo "[mpgui] booting DZ_GAMETEST (75s)..."
timeout 75 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== DEADZONE launch markers ==="
grep -aE 'DEADZONE: (ready|mp PASS|mp FAIL|systems|loot|audio|raid)' "$SER" | head -12
echo ""

READY=$(grep -ac 'DEADZONE: ready' "$SER")
MPPASS=$(grep -ac 'DEADZONE: mp PASS' "$SER")
FAILS=$(grep -acE 'DEADZONE: (mp FAIL|FAIL)' "$SER")
RAID=$(grep -ac 'DEADZONE: raid PASS' "$SER")

if [ "$READY" -ge 1 ] && [ "$MPPASS" -ge 1 ] && [ "$RAID" -ge 1 ] && [ "$FAILS" -eq 0 ]; then
  echo "DZ-MPGUI: PASS (MP-GUI-1 wired; single-player + all selftests green; coord bridge ok)"
  echo "  ready=$READY  mpPASS=$MPPASS  raidPASS=$RAID  fails=$FAILS"
  exit 0
else
  echo "DZ-MPGUI: FAIL (ready=$READY mpPASS=$MPPASS raidPASS=$RAID fails=$FAILS)"
  echo "--- DEADZONE lines ---"; grep -aE 'DEADZONE' "$SER" | head -20
  echo "--- tail ---"; tail -15 "$SER"
  exit 1
fi
