#!/usr/bin/env bash
# sig_smoke.sh -- SIG-FULL-0 (B8) proof gate.
#
# Boots the default ISO headless and asserts the full POSIX signal-delivery
# contract via sbin/sigtest (spawned by init), plus that the bad-handler case
# faults GENUINELY at the bogus handler address (RIP=0x4000) and is contained
# to the child (the kernel survives and the desktop comes up).
#
# Proves, end to end on a real boot:
#   [1] a user handler runs, returns cleanly, and execution resumes
#   [2] a BLOCKED signal does not deliver but shows pending
#   [3] unblocking delivers the pending signal
#   [4] the default action (terminate) fires for an uncaught signal
#   [5] a bad handler pointer faults safe (child dies at 0x4000, kernel lives)
#   [6] SIGCHLD is delivered to the parent when a child exits
#
# Usage: bash scripts/sig_smoke.sh
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
ISO="build/automationos.iso"
LOG="/tmp/sig_smoke.log"
rm -f "$LOG"

if [[ ! -f "$ISO" ]]; then
    echo "SIGFULL: FAIL reason=no_iso ($ISO missing -- run scripts/build_all.sh)"
    exit 1
fi

timeout 50 qemu-system-x86_64 \
    -cdrom "$ISO" -m 512 \
    -serial "file:$LOG" -display none -no-reboot >/dev/null 2>&1 || true
sleep 1

if [[ ! -s "$LOG" ]]; then echo "SIGFULL: FAIL reason=no_serial_log"; exit 1; fi

# Per-check PASS markers emitted by sbin/sigtest.
c1=$(grep -cF '[1] PASS handler_ran=1 resumed=1'              "$LOG")
c2=$(grep -cF '[2] PASS blocked_not_delivered=1 pending=1'    "$LOG")
c3=$(grep -cF '[3] PASS delivered_after_unblock=1'            "$LOG")
c4=$(grep -cF '[4] PASS default_terminate_child=1'            "$LOG")
c5=$(grep -cF '[5] PASS bad_handler_killed_child_kernel_alive=1' "$LOG")
c6=$(grep -cF '[6] PASS sigchld_on_child_exit=1'              "$LOG")
c7=$(grep -cF '[7] PASS noncanonical_handler_rejected=1'      "$LOG")
res=$(grep -cF 'SIGTEST RESULT: PASS'                         "$LOG")

# Check 5 must be a GENUINE bad-address fault, not a copy short-circuit:
# the faulting process is the sigtest child and the fault RIP/CR2 is 0x4000.
badfault=$(grep -cE "Terminating faulting process .sbin/sigtest" "$LOG")
rip4000=$(grep -cE "RIP: 0x0+4000" "$LOG")

# The kernel must SURVIVE: no panic / triple-fault, and the desktop must reach
# a compositor fps window after the test ran.
panic=0
for pat in 'KERNEL PANIC' 'triple fault' 'double fault' 'schedule() returned after'; do
    panic=$((panic + $(grep -icF "$pat" "$LOG")))
done
desktop=$(grep -cE "COMP. fps window" "$LOG")

echo "--- sig_smoke ---"
echo "check1_handler_runs=$c1 check2_mask_pending=$c2 check3_unblock=$c3"
echo "check4_default=$c4 check5_failsafe=$c5 check6_sigchld=$c6 check7_secvalidate=$c7 result_pass=$res"
echo "bad_handler_genuine_fault=$badfault rip_0x4000=$rip4000 kernel_panic=$panic desktop_up=$desktop"
echo "--- last 3 serial lines ---"
tail -3 "$LOG"

if [[ "$c1" -ge 1 && "$c2" -ge 1 && "$c3" -ge 1 && "$c4" -ge 1 && "$c5" -ge 1 \
   && "$c6" -ge 1 && "$c7" -ge 1 && "$res" -ge 1 && "$badfault" -ge 1 && "$rip4000" -ge 1 \
   && "$panic" -eq 0 && "$desktop" -ge 1 ]]; then
    echo "SIGFULL: PASS handler_runs=1 returns_clean=1 mask_blocks=1 pending_then_unblock=1 default_action=1 bad_handler_failsafe=1 sigchld_on_exit=1 noncanonical_handler_rejected=1"
    exit 0
else
    echo "SIGFULL: FAIL (see counts above)"
    exit 1
fi
