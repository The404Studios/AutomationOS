#!/usr/bin/env bash
# Boot the ISO, then verify: clean boot, on-device cc selftest still passes,
# and derby is present both as /sbin/derby and the IDE sample (for the prebuilt
# Build/Run fallback).
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
LOG=/tmp/agents9_boot.log
rm -f "$LOG"
timeout 40 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$LOG" -display none -no-reboot >/dev/null 2>&1 || true
sleep 2
crash=0
for p in 'PANIC' 'CPU EXCEPTION' 'page fault'; do crash=$((crash + $(grep -icF "$p" "$LOG"))); done
echo "CRASH=$crash"
echo "IDE_AUTOSTART=$(grep -cF 'IDE_AUTOSTART' "$LOG")"
echo "CC_SELFTEST=$(grep -F 'CC SELFTEST' "$LOG" | head -1)"
echo "derby_in_initrd=$(grep -cF 'sbin/derby' "$LOG")"
echo "derby_src_in_initrd=$(grep -cF 'usr/src/derby' "$LOG")"
