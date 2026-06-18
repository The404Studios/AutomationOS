#!/bin/bash
# WIFI-M1 verify: WIFI_SIM=1 build -> wlan0 registered + SYS_WLAN_SCAN returns the
# 4 canned APs through the syscall->wifi_ops->backend chain (proven by wlanctl).
# Kernel change -> quick_build (WIFI_SIM=1) then build_all.
# Run: wsl -d Arch bash build_test/wifi_m1_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[m1] quick_build WIFI_SIM=1 (kernel)..."
WIFI_SIM=1 bash scripts/quick_build.sh > /tmp/m1_qb.log 2>&1
if grep -qiE 'error:|undefined reference|ABI size drift|static assertion failed|^FAIL:' /tmp/m1_qb.log; then
  echo "KERNEL BUILD ERRORS:"; grep -iE 'error:|undefined reference|ABI size drift|static assertion|^FAIL:' /tmp/m1_qb.log | head -20; exit 1
fi
grep -q 'SUCCESS' /tmp/m1_qb.log || { echo "kernel build no SUCCESS"; tail -6 /tmp/m1_qb.log; exit 1; }

echo "[m1] build_all (iso + wlanctl)..."
bash scripts/build_all.sh > /tmp/m1_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/m1_ba.log; then
  echo "USERSPACE BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/m1_ba.log | head -20; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/m1_ser.log; rm -f "$SER"
echo "[m1] booting (75s)..."
timeout 75 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== wifi markers ==="
grep -aE 'WIFISEAM:|WIFISIM:|WLANCTL:' "$SER"
SEAM=0; grep -qaE 'WIFISEAM: PASS' "$SER" && SEAM=1
SIM=0;  grep -qaE 'WIFISIM: registered wlan0' "$SER" && SIM=1
SCAN=0; grep -qaE 'WLANCTL: scan 4 aps' "$SER" && SCAN=1
PASS=0; grep -qaE 'WLANCTL: PASS' "$SER" && PASS=1
HN=0;   grep -qaE 'WLANCTL:.*ssid=HomeNet' "$SER" && HN=1
BOOT=0; grep -qaE 'All services started' "$SER" && BOOT=1
echo ""
echo "seam=$SEAM sim_registered=$SIM scan4=$SCAN wlanctl_pass=$PASS homenet=$HN desktop=$BOOT"
if [ "$SEAM" = "1" ] && [ "$SIM" = "1" ] && [ "$SCAN" = "1" ] && [ "$PASS" = "1" ] && \
   [ "$HN" = "1" ] && [ "$BOOT" = "1" ]; then
  echo "WIFI-M1: PASS (wlan0 registered; SYS_WLAN_SCAN -> 4 APs incl HomeNet; seam intact; desktop up)"
  exit 0
else
  echo "WIFI-M1: FAIL"; echo "--- tail ---"; tail -30 "$SER"; exit 1
fi
