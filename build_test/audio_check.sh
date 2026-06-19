#!/bin/bash
# AUDIO verify: now that HDA_ENABLE un-gates hda_init (was compiled out under the
# always-on T410_SAFE_BOOT), boot a test tone and assert real DMA playback.
# AUDIO_SELFTEST=1 implies HDA_ENABLE. Run: wsl -d Arch bash build_test/audio_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[audio] quick_build AUDIO_SELFTEST=1 (implies HDA_ENABLE)..."
AUDIO_SELFTEST=1 bash scripts/quick_build.sh > /tmp/audio_qb.log 2>&1
if grep -qiE 'error:|^FAIL:|undefined reference' /tmp/audio_qb.log; then
  echo "KERNEL BUILD ERRORS:"; grep -iE 'error:|^FAIL:|undefined reference' /tmp/audio_qb.log | head; exit 1
fi
grep -q SUCCESS /tmp/audio_qb.log || { echo "kernel no SUCCESS"; tail -6 /tmp/audio_qb.log; exit 1; }

echo "[audio] build_all..."
bash scripts/build_all.sh > /tmp/audio_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/audio_ba.log; then
  echo "USERSPACE BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/audio_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/audio_ser.log; rm -f "$SER"
echo "[audio] booting (60s) with an HDA output codec..."
timeout 60 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -audiodev none,id=snd0 -device intel-hda -device hda-output,audiodev=snd0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== AUDIO/HDA markers ==="
grep -aiE 'AUDIO:|HDA:|HDA ' "$SER" | head -25
ADRAW=$(grep -aoE 'AUDIO: tone done bcis=[0-9]+ lpib_adv=[0-9]+' "$SER" | head -1)
BCIS=$(echo "$ADRAW" | grep -oE 'bcis=[0-9]+' | grep -oE '[0-9]+'); BCIS=${BCIS:-0}
LPIB=$(echo "$ADRAW" | grep -oE 'lpib_adv=[0-9]+' | grep -oE '[0-9]+'); LPIB=${LPIB:-0}
BOOT=0; grep -qaE 'All services started' "$SER" && BOOT=1
AUD=0; { [ "$BCIS" -gt 0 ] 2>/dev/null || [ "$LPIB" -gt 0 ] 2>/dev/null; } && AUD=1
echo ""
echo "bcis=$BCIS lpib_adv=$LPIB audio_played=$AUD desktop=$BOOT"
if [ "$AUD" = "1" ] && [ "$BOOT" = "1" ]; then
  echo "AUDIO: PASS (HDA DMA actually ran: bcis=$BCIS lpib_adv=$LPIB; desktop up)"
  exit 0
else
  echo "AUDIO: FAIL/PARTIAL (audio=$AUD desktop=$BOOT)"
  echo "--- all AUDIO/HDA lines ---"; grep -aiE 'AUDIO|HDA' "$SER" | head -30
  echo "--- tail ---"; tail -15 "$SER"
  exit 1
fi
