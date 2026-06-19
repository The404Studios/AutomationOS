#!/bin/bash
# Diagnose why the AUDIO_SELFTEST boot tone produces no AUDIO:/HDA: serial.
# Boots the CURRENT iso (already an AUDIO_SELFTEST+WIFI_SIM build) with an HDA
# output codec and captures BOTH serial and QEMU stderr.
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
SER=build_test/audio_diag_ser.log
ERR=build_test/audio_diag_err.log
rm -f "$SER" "$ERR"
echo "[diag] booting current ISO with HDA devices (45s)..."
timeout 45 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -audiodev none,id=snd0 -device intel-hda -device hda-output,audiodev=snd0 \
  -serial "file:$SER" -display none -no-reboot 2>"$ERR"
echo "=== QEMU stderr (first 15) ==="
head -15 "$ERR" 2>/dev/null
echo "=== AUDIO:/HDA serial (audio_play_tone failure paths print these) ==="
grep -aiE 'AUDIO:|HDA' "$SER" 2>/dev/null | head -25
echo "=== PCI / intel-hda detection lines ==="
grep -aiE 'pci|8086|hda|audio|class' "$SER" 2>/dev/null | grep -aiE 'hda|audio|0403|8086:2668|8086:293e' | head -15
echo "=== serial size + tail ==="
wc -c "$SER" 2>/dev/null
tail -8 "$SER" 2>/dev/null
