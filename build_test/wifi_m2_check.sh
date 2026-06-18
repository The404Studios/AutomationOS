#!/bin/bash
# WIFI-M2 verify: the connect path. WIFI_SIM kernel + WIFI_DEMO init auto-connect.
# Asserts the WPA2 4-way handshake KAT AND a live connect: wlan0 -> HomeNet (WPA2)
# -> wpasupp derives PMK/PTK -> SYS_WLAN_SET_KEY -> sim CONNECTED -> dhcpc on wlan0.
# Run: wsl -d Arch bash build_test/wifi_m2_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[m2] quick_build WIFI_SIM=1 (kernel)..."
WIFI_SIM=1 bash scripts/quick_build.sh > /tmp/m2_qb.log 2>&1
if grep -qiE 'error:|undefined reference|^FAIL:|ABI size drift|static assertion failed' /tmp/m2_qb.log; then
  echo "KERNEL BUILD ERRORS:"; grep -iE 'error:|undefined reference|^FAIL:|static assertion' /tmp/m2_qb.log | head -20; exit 1
fi
grep -q 'SUCCESS' /tmp/m2_qb.log || { echo "kernel no SUCCESS"; tail -6 /tmp/m2_qb.log; exit 1; }

echo "[m2] build_all WIFI_DEMO=1 (userspace + wpasupp)..."
WIFI_DEMO=1 bash scripts/build_all.sh > /tmp/m2_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/m2_ba.log; then
  echo "USERSPACE BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/m2_ba.log | head -20; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/m2_ser.log; rm -f "$SER"
echo "[m2] booting (90s)..."
timeout 90 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== wpa / connect markers ==="
grep -aE 'WPASUPP|WIFISIM: (connect|PTK)|dhcpc:|lease applied' "$SER" | head -30
SELF=0; grep -qaE 'WPASUPP SELFTEST: PASS' "$SER" && SELF=1
FWAY=0; grep -qaE 'WPASUPP: 4way complete' "$SER" && FWAY=1
CONN=0; grep -qaE 'WPASUPP: CONNECTED' "$SER" && CONN=1
PTK=0;  grep -qaE 'WIFISIM: PTK installed' "$SER" && PTK=1
DHCP=0; grep -qaE 'WPASUPP: dhcp on wlan0' "$SER" && DHCP=1
LEASE=0; grep -qaE 'lease applied' "$SER" && LEASE=1
BOOT=0; grep -qaE 'All services started' "$SER" && BOOT=1
echo ""
echo "selftest=$SELF 4way=$FWAY connected=$CONN sim_ptk=$PTK dhcp_started=$DHCP lease_applied=$LEASE desktop=$BOOT"
# core gate: the 4-way KAT + a real connect that installs the key + reaches CONNECTED
if [ "$SELF" = "1" ] && [ "$FWAY" = "1" ] && [ "$CONN" = "1" ] && [ "$PTK" = "1" ] && [ "$BOOT" = "1" ]; then
  echo "WIFI-M2: PASS (4-way KAT + live connect HomeNet -> PTK installed -> CONNECTED; dhcp_started=$DHCP lease=$LEASE; desktop up)"
  exit 0
else
  echo "WIFI-M2: FAIL"; echo "--- tail ---"; tail -35 "$SER"; exit 1
fi
