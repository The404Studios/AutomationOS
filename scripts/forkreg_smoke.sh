#!/usr/bin/env bash
# forkreg_smoke.sh -- FORK-REGS-INHERIT-0 gate (single boot). init spawns
# forkregtest, forktest, sigtest, pollselftest, so one headless boot proves:
#   fork_regs : forkregtest -- child sees the parent's callee-saved registers
#   no regress: forktest (CoW 20/20), sigtest, pollselftest still green
# Plus: no panic, desktop reaches a compositor fps window.
# Usage: bash scripts/forkreg_smoke.sh
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
ISO="build/automationos.iso"
LOG="/tmp/forkreg_smoke.log"
rm -f "$LOG"
if [[ ! -f "$ISO" ]]; then echo "FORKREG: FAIL reason=no_iso ($ISO missing)"; exit 1; fi

timeout 70 qemu-system-x86_64 \
    -cdrom "$ISO" -m 512 \
    -serial "file:$LOG" -display none -no-reboot >/dev/null 2>&1 || true
sleep 1
if [[ ! -s "$LOG" ]]; then echo "FORKREG: FAIL reason=no_serial_log"; exit 1; fi

reg=$(grep -cF 'FORKREGTEST RESULT: PASS' "$LOG")
cow=$(grep -cF 'FORKTEST RESULT: PASS' "$LOG")
sig=$(grep -cF 'SIGTEST RESULT: PASS' "$LOG")
poll=$(grep -cF 'POLLSELFTEST RESULT: PASS' "$LOG")
panic=0
for pat in 'KERNEL PANIC' 'triple fault' 'double fault' 'schedule() returned after'; do
    panic=$((panic + $(grep -icF "$pat" "$LOG")))
done
desktop=$(grep -cE "COMP. fps window" "$LOG")

echo "--- forkreg_smoke ---"
echo "fork_regs=$reg cow=$cow sig=$sig poll=$poll panic=$panic desktop=$desktop"
grep -F 'FORKREGTEST: iters=' "$LOG" | tail -1
echo "--- last 3 serial lines ---"
tail -3 "$LOG"

if [[ "$reg" -ge 1 && "$cow" -ge 1 && "$sig" -ge 1 && "$poll" -ge 1 \
   && "$panic" -eq 0 && "$desktop" -ge 1 ]]; then
    echo "FORKREG: PASS callee_saved_inherited=1 no_regress=1"
    exit 0
else
    echo "FORKREG: FAIL (see counts above)"
    exit 1
fi
