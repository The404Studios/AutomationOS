#!/bin/bash
# WPA3-CONNECT verify: wpasupp derives the PMK via SAE (dragonfly) for a WPA3
# network, then the same 4-way -> CONNECTED. WIFI_SIM kernel + WIFI_DEMO_WPA3
# init auto-connect to the simulated SecureMesh (WPA3). Run: wsl -d Arch bash build_test/wpa3_connect_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[wpa3] quick_build WIFI_SIM=1..."
WIFI_SIM=1 bash scripts/quick_build.sh > /tmp/wpa3_qb.log 2>&1
if grep -qiE 'error:|^FAIL:|undefined reference' /tmp/wpa3_qb.log; then
  echo "KERNEL BUILD ERRORS:"; grep -iE 'error:|^FAIL:|undefined reference' /tmp/wpa3_qb.log | head; exit 1
fi
grep -q SUCCESS /tmp/wpa3_qb.log || { echo "kernel no SUCCESS"; tail -6 /tmp/wpa3_qb.log; exit 1; }

echo "[wpa3] build_all WIFI_DEMO_WPA3=1..."
WIFI_DEMO_WPA3=1 bash scripts/build_all.sh > /tmp/wpa3_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/wpa3_ba.log; then
  echo "USERSPACE BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/wpa3_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/wpa3_ser.log; rm -f "$SER"
echo "[wpa3] booting (90s)..."
timeout 90 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== WPA3 / SAE markers ==="
grep -aE 'WPASUPP|WIFISIM: connect' "$SER" | head
SAE=0;  grep -qaE 'WPASUPP: SAE PMK derived' "$SER" && SAE=1
SEC2=0; grep -qaE 'WIFISIM: connect .* sec=2' "$SER" && SEC2=1
CONN=0; grep -qaE 'WPASUPP: CONNECTED ssid=SecureMesh' "$SER" && CONN=1
BOOT=0; grep -qaE 'All services started' "$SER" && BOOT=1
echo ""
echo "sae_pmk=$SAE wpa3_assoc=$SEC2 connected=$CONN desktop=$BOOT"
if [ "$SAE" = 1 ] && [ "$CONN" = 1 ] && [ "$BOOT" = 1 ]; then
  echo "WPA3-CONNECT: PASS (SAE dragonfly PMK -> SecureMesh CONNECTED)"
  exit 0
else
  echo "WPA3-CONNECT: FAIL (sae=$SAE assoc=$SEC2 conn=$CONN boot=$BOOT)"
  echo "--- tail ---"; tail -25 "$SER"; exit 1
fi
