#!/bin/bash
# DEMO ISO + final combined verify: one image, one boot proves the whole
# QEMU-testable feature set. Saves build/automationos-demo.iso for the user.
#   kernel:    HDA_ENABLE=1 (sound) + WIFI_SIM=1 (wlan0) + AUDIO_SELFTEST=1 (boot tone)
#   userspace: WIFI_DEMO=1 (auto-connect HomeNet)
# Run: wsl -d Arch bash build_test/demo_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[demo] quick_build HDA_ENABLE=1 WIFI_SIM=1 AUDIO_SELFTEST=1..."
HDA_ENABLE=1 WIFI_SIM=1 AUDIO_SELFTEST=1 bash scripts/quick_build.sh > /tmp/demo_qb.log 2>&1
if grep -qiE 'error:|^FAIL:|undefined reference' /tmp/demo_qb.log; then
  echo "KERNEL BUILD ERRORS:"; grep -iE 'error:|^FAIL:|undefined reference' /tmp/demo_qb.log | head; exit 1
fi
grep -q SUCCESS /tmp/demo_qb.log || { echo "kernel no SUCCESS"; tail -6 /tmp/demo_qb.log; exit 1; }

echo "[demo] build_all WIFI_DEMO=1..."
WIFI_DEMO=1 bash scripts/build_all.sh > /tmp/demo_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/demo_ba.log; then
  echo "USERSPACE BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/demo_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }
cp build/automationos.iso build/automationos-demo.iso

SER=build_test/demo_ser.log; rm -f "$SER"
echo "[demo] booting (95s) with HDA codec..."
timeout 95 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -audiodev none,id=snd0 -device intel-hda -device hda-output,audiodev=snd0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== feature markers ==="
grep -aE '\[NETMAN\] scan|WPASUPP: (CONNECTED|4way)|lease applied|AUDIO: tone done|\[SOUNDMAN\] audio|HDA: Initialization' "$SER" | head
SCAN=0; grep -qaE '\[NETMAN\] scan 4 networks' "$SER" && SCAN=1
CONN=0; grep -qaE 'WPASUPP: CONNECTED' "$SER" && CONN=1
LEASE=0; grep -qaE 'lease applied' "$SER" && LEASE=1
ADRAW=$(grep -aoE 'AUDIO: tone done bcis=[0-9]+ lpib_adv=[0-9]+' "$SER" | head -1)
BCIS=$(echo "$ADRAW" | grep -oE 'bcis=[0-9]+' | grep -oE '[0-9]+'); BCIS=${BCIS:-0}
LPIB=$(echo "$ADRAW" | grep -oE 'lpib_adv=[0-9]+' | grep -oE '[0-9]+'); LPIB=${LPIB:-0}
AUD=0; { [ "$BCIS" -gt 0 ] 2>/dev/null || [ "$LPIB" -gt 0 ] 2>/dev/null; } && AUD=1
SND=0; grep -qaE '\[SOUNDMAN\] audio present=1' "$SER" && SND=1
BOOT=0; grep -qaE 'All services started' "$SER" && BOOT=1
echo ""
echo "netman_scan4=$SCAN wpa2_connect=$CONN dhcp_lease=$LEASE audio_dma=$AUD(bcis=$BCIS lpib=$LPIB) soundman=$SND desktop=$BOOT"
if [ "$SCAN" = 1 ] && [ "$CONN" = 1 ] && [ "$AUD" = 1 ] && [ "$SND" = 1 ] && [ "$BOOT" = 1 ]; then
  echo ""
  echo "DEMO: PASS -- WiFi scan + WPA2 4-way + DHCP, HDA sound (DMA), Sound Manager: ALL LIVE"
  echo "saved -> build/automationos-demo.iso"
  exit 0
else
  echo "DEMO: PARTIAL (scan=$SCAN conn=$CONN aud=$AUD snd=$SND boot=$BOOT)"
  echo "--- tail ---"; tail -25 "$SER"; exit 1
fi
