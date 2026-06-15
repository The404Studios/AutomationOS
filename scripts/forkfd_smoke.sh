#!/usr/bin/env bash
# forkfd_smoke.sh -- fork-fd-table inheritance proof gate.
#
# Boots the default ISO headless and asserts, end to end on a real boot, that
# fork() now deep-copies the parent's regular-file fd table into the child via
# sbin/forkfdtest (spawned by init). The test runs 100 open/fork/use/close
# cycles and prints FORKFDTEST RESULT: PASS iff:
#   * the child can WRITE through an inherited ramfs fd (fails as exit 2 on a
#     kernel that does not inherit fds),
#   * the parent's fd is still live after the child closes + exits (no poison),
#   * the parent's close after the child's close does not double-free,
#   * 100 cycles do not corrupt the shared-inode refcount.
#
# Also asserts the conservative fail-closed policy did NOT fire during a normal
# boot: no live forker held a device/private/dentry-backed fd (the kernel logs
# "fd N not inheritable" and fails that fork). A nonzero count here is a real
# regression -- some real process forks while holding an unsafe fd.
#
# Usage: bash scripts/forkfd_smoke.sh
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
ISO="build/automationos.iso"
LOG="/tmp/forkfd_smoke.log"
rm -f "$LOG"

if [[ ! -f "$ISO" ]]; then
    echo "FORKFD: FAIL reason=no_iso ($ISO missing -- run scripts/build_all.sh)"
    exit 1
fi

timeout 60 qemu-system-x86_64 \
    -cdrom "$ISO" -m 512 \
    -serial "file:$LOG" -display none -no-reboot >/dev/null 2>&1 || true
sleep 1

if [[ ! -s "$LOG" ]]; then echo "FORKFD: FAIL reason=no_serial_log"; exit 1; fi

ran=$(grep -cF 'FORKFDTEST: start' "$LOG")
res=$(grep -cF 'FORKFDTEST RESULT: PASS' "$LOG")
fail=$(grep -cF 'FORKFDTEST RESULT: FAIL' "$LOG")
# The summary line carries the per-property flags (useful when it FAILs).
summary=$(grep -F 'FORKFDTEST: iters=' "$LOG" | tail -1)

# Fail-closed regression guard: this string is only logged when fork() refuses
# to inherit an unsafe fd. During a normal boot it must never appear.
failclosed=$(grep -cF 'not inheritable' "$LOG")

# The kernel must SURVIVE the 100 fork cycles: no panic / triple-fault, and the
# desktop must still reach a compositor fps window.
panic=0
for pat in 'KERNEL PANIC' 'triple fault' 'double fault' 'schedule() returned after'; do
    panic=$((panic + $(grep -icF "$pat" "$LOG")))
done
desktop=$(grep -cE "COMP. fps window" "$LOG")

echo "--- forkfd_smoke ---"
echo "test_ran=$ran result_pass=$res result_fail=$fail"
echo "failclosed_fired=$failclosed kernel_panic=$panic desktop_up=$desktop"
echo "summary: ${summary:-<none>}"
echo "--- last 3 serial lines ---"
tail -3 "$LOG"

if [[ "$ran" -ge 1 && "$res" -ge 1 && "$fail" -eq 0 \
   && "$failclosed" -eq 0 && "$panic" -eq 0 && "$desktop" -ge 1 ]]; then
    echo "FORKFD: PASS child_inherits_regular_fd=1 parent_unpoisoned=1 no_double_free=1 failclosed_quiet=1"
    exit 0
else
    echo "FORKFD: FAIL (see counts above)"
    exit 1
fi
