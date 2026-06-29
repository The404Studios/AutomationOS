#!/bin/bash
# AUDIO B1 verify: gapless on_bcis refill. Build AUDIO_SELFTEST=1 (implies HDA_ENABLE),
# boot with an HDA output codec, assert the stream selftest drove >1 buffer cycle of
# per-chunk refills (refills>8) with real DMA movement (lpib_adv>0).
# Run: wsl -d Arch bash build_test/audio_stream_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[audiostream] quick_build AUDIO_SELFTEST=1 (implies HDA_ENABLE)..."
AUDIO_SELFTEST=1 bash scripts/quick_build.sh > /tmp/as_qb.log 2>&1
if grep -qiE 'error:|^FAIL:|undefined reference' /tmp/as_qb.log; then
  echo "KERNEL BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/as_qb.log | head; exit 1
fi
grep -q SUCCESS /tmp/as_qb.log || { echo "kernel no SUCCESS"; tail -6 /tmp/as_qb.log; exit 1; }

echo "[audiostream] build_all..."
bash scripts/build_all.sh > /tmp/as_ba.log 2>&1
grep -qiE 'error:|undefined reference' /tmp/as_ba.log && { echo "USERSPACE BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/as_ba.log | head; exit 1; }
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/audiostream_ser.log; rm -f "$SER"
echo "[audiostream] booting (60s) with an HDA output codec..."
timeout 60 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -audiodev none,id=snd0 -device intel-hda -device hda-output,audiodev=snd0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== AUDIO stream markers ==="
grep -aE 'AUDIO: (tone|mixed|stream) done|AUDIO: streaming' "$SER" | head
SLINE=$(grep -aoE 'AUDIO: stream done bcis=[0-9]+ refills=[0-9]+ lpib_adv=[0-9]+' "$SER" | head -1)
REF=$(echo "$SLINE" | grep -oE 'refills=[0-9]+' | grep -oE '[0-9]+'); REF=${REF:-0}
LP=$(echo "$SLINE" | grep -oE 'lpib_adv=[0-9]+' | grep -oE '[0-9]+'); LP=${LP:-0}
BOOT=0; grep -qaE 'All services started' "$SER" && BOOT=1
echo ""
echo "refills=$REF lpib_adv=$LP desktop=$BOOT"
if [ "$REF" -gt 8 ] 2>/dev/null && [ "$LP" -gt 0 ] 2>/dev/null; then
  echo "AUDIO-B1: PASS (gapless on_bcis refill: $REF chunk refills across >1 buffer cycle, lpib_adv=$LP)"
  exit 0
else
  echo "AUDIO-B1: FAIL (refills=$REF lpib_adv=$LP desktop=$BOOT)"
  echo "--- AUDIO/HDA lines ---"; grep -aiE 'AUDIO|HDA' "$SER" | head -30
  echo "--- tail ---"; tail -15 "$SER"
  exit 1
fi
