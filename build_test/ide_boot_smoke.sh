#!/usr/bin/env bash
# Minimal headless boot-smoke for the IDE minimal-profile ISO.
# Usage: bash ide_boot_smoke.sh [logname]
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
LOG="/tmp/${1:-ide_boot.log}"
rm -f "$LOG"
timeout 35 qemu-system-x86_64 \
    -cdrom build/automationos.iso -m 512 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$LOG" -display none -no-reboot >/dev/null 2>&1 || true
sleep 2
if [[ ! -s "$LOG" ]]; then echo "RESULT=NO_LOG"; exit 1; fi
crash=0
for pat in 'PANIC' 'CPU EXCEPTION' 'page fault' 'triple fault' 'double fault' 'GENERAL PROTECTION'; do
    c=$(grep -icF "$pat" "$LOG")
    crash=$((crash + c))
done
echo "LINES=$(wc -l < "$LOG")"
echo "CRASH=$crash"
echo "AUTOSTART=$(grep -cF 'IDE_AUTOSTART' "$LOG")"
echo "PID9_SPAWN=$(grep -cF 'created with PID 9' "$LOG")"
echo "IDE_EXIT=$(grep -cE 'Process 9 .*exit' "$LOG")"
echo "WINDOWS=$(grep -ciE 'window.*created' "$LOG")"
echo "--- tail 5 ---"
tail -5 "$LOG"
if [[ "$crash" -eq 0 ]] && [[ "$(grep -cF 'IDE_AUTOSTART' "$LOG")" -ge 1 ]]; then
    echo "RESULT=PASS"
else
    echo "RESULT=FAIL"
fi
