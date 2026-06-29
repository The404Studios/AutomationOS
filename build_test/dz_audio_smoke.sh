#!/bin/bash
# In-game audio verify: DeadZone's SFX mixer streams PCM via SYS_AUDIO_STREAM_WRITE
# (AUDIO B1) into the HDA DMA. Build AUDIO_SELFTEST=1 (kernel -> HDA_ENABLE, the
# syscall + ring exist) + DZ_GAMETEST=1 (init spawns sbin/deadzone, whose launch
# audio_selftest fires a gunshot through the mixer). Assert "DEADZONE: audio PASS".
# Run: wsl -d Arch bash build_test/dz_audio_smoke.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[dzaudio] quick_build AUDIO_SELFTEST=1 (implies HDA_ENABLE)..."
AUDIO_SELFTEST=1 bash scripts/quick_build.sh > /tmp/dza_qb.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/dza_qb.log; then
  echo "KERNEL BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/dza_qb.log | head; exit 1
fi
grep -q SUCCESS /tmp/dza_qb.log || { echo "kernel no SUCCESS"; tail -6 /tmp/dza_qb.log; exit 1; }

echo "[dzaudio] build_all DZ_GAMETEST=1 (init spawns sbin/deadzone)..."
DZ_GAMETEST=1 bash scripts/build_all.sh > /tmp/dza_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/dza_ba.log; then
  echo "USERSPACE BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/dza_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/dzaudio_ser.log; rm -f "$SER"
echo "[dzaudio] booting (80s) with an HDA output codec..."
timeout 80 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -audiodev none,id=snd0 -device intel-hda -device hda-output,audiodev=snd0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== DeadZone audio marker ==="
grep -aE 'DEADZONE: (audio|ready|systems|loot)' "$SER" | head
LINE=$(grep -aoE 'DEADZONE: audio PASS streamed=[0-9]+' "$SER" | head -1)
echo ""
if [ -n "$LINE" ]; then
  echo "DZ-AUDIO: PASS (in-game SFX streamed to HDA DMA)"
  echo "  $LINE"
  exit 0
else
  echo "DZ-AUDIO: FAIL (no 'audio PASS streamed' marker)"
  echo "--- DeadZone/AUDIO/HDA lines ---"; grep -aiE 'DEADZONE: audio|AUDIO|HDA' "$SER" | head -20
  echo "--- tail ---"; tail -12 "$SER"
  exit 1
fi
