#!/usr/bin/env bash
# fork_smoke.sh -- FORK-REGS-INHERIT-0 acceptance gate (single boot).
#
# init spawns forkregtest, forkfdtest, forktest, sigtest, pollselftest, so one
# headless boot exercises the whole fork ABI + the regression suite:
#   fork_regs       : forkregtest  -- child sees parent's callee-saved regs
#   fork_fd_inherit : forkfdtest   -- child can use an inherited fd (needs regs)
#   fork_lazy_fault : forktest     -- existing fork/CoW isolation (20/20)
#   signal regress  : sigtest      -- SIG-FULL-0 still green
#   poll regress    : pollselftest -- POLL-SELECT-0 still green
# Plus: no fail-closed fd drop, no panic, desktop reaches a compositor fps window.
#
# Usage: bash scripts/fork_smoke.sh
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
ISO="build/automationos.iso"
LOG="/tmp/fork_smoke.log"
rm -f "$LOG"

if [[ ! -f "$ISO" ]]; then
    echo "FORK: FAIL reason=no_iso ($ISO missing -- run scripts/build_all.sh)"; exit 1
fi

timeout 70 qemu-system-x86_64 \
    -cdrom "$ISO" -m 512 \
    -serial "file:$LOG" -display none -no-reboot >/dev/null 2>&1 || true
sleep 1
if [[ ! -s "$LOG" ]]; then echo "FORK: FAIL reason=no_serial_log"; exit 1; fi

reg=$(grep -cF 'FORKREGTEST RESULT: PASS' "$LOG")
fd=$(grep -cF 'FORKFDTEST RESULT: PASS' "$LOG")
cow=$(grep -cF 'FORKTEST RESULT: PASS' "$LOG")
sig=$(grep -cF 'SIGTEST RESULT: PASS' "$LOG")
poll=$(grep -cF 'POLLSELFTEST RESULT: PASS' "$LOG")
failclosed=$(grep -cF 'not inheritable' "$LOG")

panic=0
for pat in 'KERNEL PANIC' 'triple fault' 'double fault' 'schedule() returned after'; do
    panic=$((panic + $(grep -icF "$pat" "$LOG")))
done
desktop=$(grep -cE "COMP. fps window" "$LOG")

echo "--- fork_smoke ---"
echo "fork_regs=$reg fork_fd_inherit=$fd fork_lazy_fault(cow)=$cow"
echo "sig_regress=$sig poll_regress=$poll failclosed=$failclosed kernel_panic=$panic desktop_up=$desktop"
echo "--- forkreg/forkfd summary lines ---"
grep -E 'FORK(REG|FD)TEST: iters=' "$LOG" | tail -2
echo "--- last 3 serial lines ---"
tail -3 "$LOG"

if [[ "$reg" -ge 1 && "$fd" -ge 1 && "$cow" -ge 1 && "$sig" -ge 1 && "$poll" -ge 1 \
   && "$failclosed" -eq 0 && "$panic" -eq 0 && "$desktop" -ge 1 ]]; then
    echo "FORK: PASS regs_inherited=1 fd_inherited=1 cow_isolation=1 signal_ok=1 poll_ok=1"
    exit 0
else
    echo "FORK: FAIL (see counts above)"
    exit 1
fi
