#!/bin/bash
# SOUNDMAN verify: the Sound Manager GUI builds, opens, and reads the live audio
# device via SYS_AUDIO_STATUS. Build HDA_ENABLE=1 (codec present) and boot with
# an HDA output codec. Run: wsl -d Arch bash build_test/soundman_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[snd] quick_build HDA_ENABLE=1..."
HDA_ENABLE=1 bash scripts/quick_build.sh > /tmp/snd_qb.log 2>&1
if grep -qiE 'error:|^FAIL:|undefined reference' /tmp/snd_qb.log; then
  echo "KERNEL BUILD ERRORS:"; grep -iE 'error:|^FAIL:|undefined reference' /tmp/snd_qb.log | head; exit 1
fi
grep -q SUCCESS /tmp/snd_qb.log || { echo "kernel no SUCCESS"; tail -6 /tmp/snd_qb.log; exit 1; }

echo "[snd] build_all (soundman app + dock)..."
bash scripts/build_all.sh > /tmp/snd_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/snd_ba.log; then
  echo "USERSPACE BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/snd_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }
grep -qF 'soundman' iso/boot/initrd.img || { echo "soundman NOT in initrd"; exit 1; }

SER=build_test/snd_ser.log; rm -f "$SER"
echo "[snd] booting (60s) with an HDA output codec..."
timeout 60 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -audiodev none,id=snd0 -device intel-hda -device hda-output,audiodev=snd0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== SOUNDMAN / HDA markers ==="
grep -aE '\[SOUNDMAN\]|HDA: Initialization complete' "$SER" | head
WIN=0;     grep -qaE '\[SOUNDMAN\] window created' "$SER" && WIN=1
PRESENT=0; grep -qaE '\[SOUNDMAN\] audio present=1' "$SER" && PRESENT=1
HDAOK=0;   grep -qaE 'HDA: Initialization complete' "$SER" && HDAOK=1
BOOT=0;    grep -qaE 'All services started' "$SER" && BOOT=1
echo ""
echo "soundman_window=$WIN audio_present=$PRESENT hda_init=$HDAOK desktop=$BOOT"
if [ "$WIN" = "1" ] && [ "$PRESENT" = "1" ] && [ "$BOOT" = "1" ]; then
  echo "SOUNDMAN: PASS (window up, audio device present via SYS_AUDIO_STATUS, desktop up)"
  exit 0
else
  echo "SOUNDMAN: FAIL (win=$WIN present=$PRESENT hda=$HDAOK boot=$BOOT)"
  echo "--- tail ---"; tail -20 "$SER"; exit 1
fi
