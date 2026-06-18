#!/bin/bash
# WIFI-SEAM verify: kernel changed -> quick_build then build_all, boot, assert
# WIFISEAM: PASS and that the desktop still comes up (no regression).
# Run: wsl -d Arch bash build_test/wifi_seam_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[seam] quick_build (kernel)..."
bash scripts/quick_build.sh > /tmp/seam_qb.log 2>&1
# NB: do NOT match bare "unresolved" -- the success line is "no unresolved symbols".
if grep -qiE 'error:|undefined reference|ABI size drift|static assertion failed' /tmp/seam_qb.log; then
  echo "KERNEL BUILD ERRORS:"; grep -iE 'error:|undefined reference|ABI size drift|static assertion' /tmp/seam_qb.log | head; exit 1
fi
grep -q 'SUCCESS' /tmp/seam_qb.log || { echo "kernel build no SUCCESS"; tail -6 /tmp/seam_qb.log; exit 1; }

echo "[seam] build_all (iso)..."
bash scripts/build_all.sh > /tmp/seam_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/seam_ba.log; then
  echo "USERSPACE BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/seam_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/seam_ser.log; rm -f "$SER"
echo "[seam] booting (70s)..."
timeout 70 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== seam + boot markers ==="
grep -aE 'WIFISEAM:|All services started' "$SER" || echo "(none)"
SEAM=0; grep -qaE 'WIFISEAM: PASS' "$SER" && SEAM=1
BOOT=0; grep -qaE 'All services started' "$SER" && BOOT=1
echo ""
echo "seam_pass=$SEAM desktop_up=$BOOT"
if [ "$SEAM" = "1" ] && [ "$BOOT" = "1" ]; then
  echo "WIFI-SEAM: PASS (seam resolves + desktop unregressed)"
  exit 0
else
  echo "WIFI-SEAM: FAIL"
  echo "--- tail ---"; tail -25 "$SER"
  exit 1
fi
