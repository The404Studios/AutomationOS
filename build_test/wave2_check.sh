#!/bin/bash
# WAVE-2 verify (audio + Network Manager GUI + connect, one build+boot):
#   kernel:    AUDIO_SELFTEST=1 (boot test tone -> DMA proof) + WIFI_SIM=1 (wlan0)
#   userspace: WIFI_DEMO=1 (headless auto-connect)
#   boot with an HDA OUTPUT codec so the DMA engine actually clocks the stream.
# Run: wsl -d Arch bash build_test/wave2_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[w2] quick_build AUDIO_SELFTEST=1 WIFI_SIM=1 (kernel)..."
AUDIO_SELFTEST=1 WIFI_SIM=1 bash scripts/quick_build.sh > /tmp/w2_qb.log 2>&1
if grep -qiE 'error:|undefined reference|^FAIL:|static assertion failed' /tmp/w2_qb.log; then
  echo "KERNEL BUILD ERRORS:"; grep -iE 'error:|undefined reference|^FAIL:|static assertion' /tmp/w2_qb.log | head -20; exit 1
fi
grep -q 'SUCCESS' /tmp/w2_qb.log || { echo "kernel no SUCCESS"; tail -8 /tmp/w2_qb.log; exit 1; }

echo "[w2] build_all WIFI_DEMO=1 (userspace + GUI)..."
WIFI_DEMO=1 bash scripts/build_all.sh > /tmp/w2_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/w2_ba.log; then
  echo "USERSPACE BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/w2_ba.log | head -20; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/w2_ser.log; rm -f "$SER"
echo "[w2] booting (90s) with HDA output codec..."
timeout 90 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -audiodev none,id=snd0 -device intel-hda -device hda-output,audiodev=snd0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== audio markers ==="
grep -aE 'HDA:|AUDIO:' "$SER" | head
echo "=== netman / wifi markers ==="
grep -aE '\[NETMAN\]|WPASUPP: (CONNECTED|4way)|WIFISIM: registered' "$SER" | head

ADRAW=$(grep -aoE 'AUDIO: tone done bcis=[0-9]+ lpib_adv=[0-9]+' "$SER" | head -1)
BCIS=$(echo "$ADRAW" | grep -oE 'bcis=[0-9]+' | grep -oE '[0-9]+'); BCIS=${BCIS:-0}
LPIB=$(echo "$ADRAW" | grep -oE 'lpib_adv=[0-9]+' | grep -oE '[0-9]+'); LPIB=${LPIB:-0}
AUD=0; { [ "$BCIS" -gt 0 ] 2>/dev/null || [ "$LPIB" -gt 0 ] 2>/dev/null; } && AUD=1
NMWIN=0;  grep -qaE '\[NETMAN\] window created' "$SER" && NMWIN=1
NMSCAN=0; grep -qaE '\[NETMAN\] scan 4 networks' "$SER" && NMSCAN=1
CONN=0;   grep -qaE 'WPASUPP: CONNECTED' "$SER" && CONN=1
BOOT=0;   grep -qaE 'All services started' "$SER" && BOOT=1
echo ""
echo "audio(bcis=$BCIS lpib=$LPIB)=$AUD netman_window=$NMWIN netman_scan4=$NMSCAN connected=$CONN desktop=$BOOT"
if [ "$AUD" = "1" ] && [ "$NMWIN" = "1" ] && [ "$NMSCAN" = "1" ] && [ "$CONN" = "1" ] && [ "$BOOT" = "1" ]; then
  echo "WAVE-2: PASS (audio DMA ran; Network Manager scans 4 nets; WPA2 connect; desktop up)"
  exit 0
else
  echo "WAVE-2: PARTIAL (audio=$AUD netman_win=$NMWIN scan4=$NMSCAN conn=$CONN boot=$BOOT)"
  echo "--- tail ---"; tail -30 "$SER"; exit 1
fi
