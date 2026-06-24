#!/bin/bash
# AUDIO B1 part-2 verify: a ring-3 app (sbin/streamtest) streams PCM via
# SYS_AUDIO_STREAM_WRITE; the kernel's on_bcis ring consumer drains it into the
# HDA DMA. Build AUDIO_SELFTEST=1 (kernel, implies HDA_ENABLE -> the syscall +
# ring exist) + AUDIO_STREAMTEST=1 (init spawns streamtest). Assert STREAMTEST: PASS.
# Run: wsl -d Arch bash build_test/audio_streamtest_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[streamtest] quick_build AUDIO_SELFTEST=1 (implies HDA_ENABLE)..."
AUDIO_SELFTEST=1 bash scripts/quick_build.sh > /tmp/stt_qb.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/stt_qb.log; then
  echo "KERNEL BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/stt_qb.log | head; exit 1
fi
grep -q SUCCESS /tmp/stt_qb.log || { echo "kernel no SUCCESS"; tail -6 /tmp/stt_qb.log; exit 1; }

echo "[streamtest] build_all AUDIO_STREAMTEST=1 (init spawns sbin/streamtest)..."
AUDIO_STREAMTEST=1 bash scripts/build_all.sh > /tmp/stt_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/stt_ba.log; then
  echo "USERSPACE BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/stt_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/streamtest_ser.log; rm -f "$SER"
echo "[streamtest] booting (75s) with an HDA output codec..."
timeout 75 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -audiodev none,id=snd0 -device intel-hda -device hda-output,audiodev=snd0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== STREAMTEST markers ==="
grep -aE 'STREAMTEST:' "$SER" | head
PASS=0; grep -qaE 'STREAMTEST: PASS' "$SER" && PASS=1
SKIP=0; grep -qaE 'STREAMTEST: SKIP' "$SER" && SKIP=1
LINE=$(grep -aoE 'STREAMTEST: streamed writes=[0-9]+ bytes=[0-9]+ eagain=[0-9]+' "$SER" | head -1)
echo ""
echo "pass=$PASS skip=$SKIP"
[ -n "$LINE" ] && echo "  $LINE"
if [ "$PASS" = "1" ]; then
  echo "AUDIO-B1-P2: PASS (ring-3 streamed PCM through SYS_AUDIO_STREAM_WRITE -> on_bcis ring -> HDA DMA)"
  exit 0
else
  echo "AUDIO-B1-P2: FAIL (pass=$PASS skip=$SKIP)"
  echo "--- STREAMTEST/AUDIO/HDA lines ---"; grep -aiE 'STREAMTEST|AUDIO|HDA' "$SER" | head -30
  echo "--- tail ---"; tail -15 "$SER"
  exit 1
fi
