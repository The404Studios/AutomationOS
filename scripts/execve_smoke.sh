#!/usr/bin/env bash
# execve_smoke.sh -- EXECVE-INPLACE-0 acceptance gate (single headless boot).
#
# Proves the real in-place execve(): sys_execve REPLACES the calling process's
# image (same PID, fresh argv/envp) instead of the old stub's "spawn a stray 3rd
# process" behaviour. init spawns sbin/exectest, which:
#   - forks; the child execve("sbin/execchild", {ARG1MAGIC}, {EXECVAR=ENVMAGIC}).
#     If in-place execve works, that child's image becomes execchild IN THE SAME
#     PID (C1 same_pid), with argc/argv/envp delivered (C4), exiting 77 (C2);
#   - polls SYS_PROCLIST across the window to prove no stray 3rd process appears
#     (C3 no_stray: process count delta == +1, never +2);
#   - forks again; that child execve("/no/such/file") which RETURNS < 0, the
#     ORIGINAL image survives, prints "EXECCHILD-FAILPATH: survived" and exits 88
#     (C5 fail_neg_survives) -- a failed execve must not destroy the caller;
#   - checks a .data sentinel survived (CoW sanity) and decision-6's
#     execchild.parent_pid == exectest.pid.
# Prints EXECTEST RESULT: PASS iff every claim holds.
#
# PASS gate (this script):
#   EXECTEST RESULT: PASS    >= 1   AND
#   EXECTEST RESULT: FAIL    == 0   AND
#   EXECCHILD: start         >= 1   (the post-execve image actually RAN -- proves
#                                    replacement, not a returned-from-execve stub) AND
#   EXECCHILD-FAILPATH: survived >= 1  (a failed execve left the caller alive) AND
#   no panic (KERNEL PANIC / triple fault / double fault / schedule() returned) AND
#   desktop_up (a compositor fps window) >= 1.
#
# Usage: bash scripts/execve_smoke.sh
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
ISO="build/automationos.iso"
LOG="/tmp/execve_smoke.log"
rm -f "$LOG"

if [[ ! -f "$ISO" ]]; then
    echo "EXECVE: FAIL reason=no_iso ($ISO missing -- run scripts/build_all.sh)"
    exit 1
fi

timeout 70 qemu-system-x86_64 \
    -cdrom "$ISO" -m 512 \
    -serial "file:$LOG" -display none -no-reboot >/dev/null 2>&1 || true
sleep 1

if [[ ! -s "$LOG" ]]; then echo "EXECVE: FAIL reason=no_serial_log"; exit 1; fi

ran=$(grep -cF 'EXECTEST: start' "$LOG")
res=$(grep -cF 'EXECTEST RESULT: PASS' "$LOG")
fail=$(grep -cF 'EXECTEST RESULT: FAIL' "$LOG")
# The post-execve image announcing itself proves replacement happened in-place.
child=$(grep -cF 'EXECCHILD: start' "$LOG")
# A failed execve must leave the original image running.
failpath=$(grep -cF 'EXECCHILD-FAILPATH: survived' "$LOG")
# Per-claim summary line (useful when it FAILs).
summary=$(grep -F 'EXECTEST: same_pid=' "$LOG" | tail -1)
childline=$(grep -F 'EXECCHILD: argc_ok=' "$LOG" | tail -1)

# The kernel must SURVIVE the fork/exec dance: no panic / triple-fault, and the
# desktop must still reach a compositor fps window.
panic=0
for pat in 'KERNEL PANIC' 'triple fault' 'double fault' 'schedule() returned after'; do
    panic=$((panic + $(grep -icF "$pat" "$LOG")))
done
desktop=$(grep -cE "COMP. fps window" "$LOG")

echo "--- execve_smoke ---"
echo "test_ran=$ran result_pass=$res result_fail=$fail"
echo "execchild_started=$child failpath_survived=$failpath kernel_panic=$panic desktop_up=$desktop"
echo "summary: ${summary:-<none>}"
echo "execchild: ${childline:-<none>}"
echo "--- last 3 serial lines ---"
tail -3 "$LOG"

if [[ "$ran" -ge 1 && "$res" -ge 1 && "$fail" -eq 0 \
   && "$child" -ge 1 && "$failpath" -ge 1 \
   && "$panic" -eq 0 && "$desktop" -ge 1 ]]; then
    echo "EXECVE: PASS same_pid=1 status77=1 no_stray=1 argv_envp=1 fail_neg_survives=1"
    exit 0
else
    echo "EXECVE: FAIL (see counts above)"
    exit 1
fi
