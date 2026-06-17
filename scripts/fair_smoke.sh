#!/usr/bin/env bash
# fair_smoke.sh -- SCHED-FAIRNESS-0 proof gate.
#
# Builds the PREEMPTIVE kernel + a DESKTOP-LESS FAIRTEST init (pure
# non-syscalling ring-3 burners + a sleeper) and asserts the sleeper WAKES
# despite the burners. With no compositor and init blocked in waitpid on the
# never-exiting burners, NOTHING offers a cooperative-switch boundary, so
# fairwake's post-sleep RESUME_CRETURN wake can ONLY be dispatched by the
# IRQ-path fairness fix. Under the old scheduler fairwake STARVES (no PASS).
#
# Expected: FAIL before the fix (starvation reproduces), PASS after.
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel

PREEMPT=1 bash scripts/quick_build.sh > /tmp/fair_kbuild.out 2>&1 || { echo "FAIRNESS: FAIL stage=kbuild"; exit 1; }
cp build/kernel-preempt.elf build/kernel.elf
FAIRTEST=1 DESKTOP_MINIMAL=1 bash scripts/build_all.sh > /tmp/fair_build.out 2>&1 || { echo "FAIRNESS: FAIL stage=build"; exit 1; }

LOG=/tmp/fair_boot.log; rm -f "$LOG"
timeout 25 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -serial "file:$LOG" -display none -no-reboot >/dev/null 2>&1 || true
sleep 1

if [[ ! -s "$LOG" ]]; then echo "FAIRNESS: FAIL stage=no_serial"; exit 1; fi
fairstart=$(grep -cF 'FAIRWAKE: start' "$LOG")
fairpass=$(grep -cF 'FAIRWAKE: PASS' "$LOG")
burner=$(grep -ciE 'FAIRTEST:|pureburn' "$LOG")
panic=$(grep -ciE 'KERNEL PANIC|triple fault|double fault' "$LOG")
# The discriminator: the IRQ path actually HANDED the CPU to a RESUME_CRETURN task
# (the fix's one-shot announce). ABSENT on the old code (which rejected such tasks),
# PRESENT after the fix. NB: a busy desktop masks raw starvation by providing
# cooperative handoffs, so fairwake wakes either way -- this print is what proves
# the IRQ-dispatch path is live, and a clean boot proves it is stable.
fairfix=$(grep -cE 'FAIRNESS: IRQ-dispatched RESUME_CRETURN' "$LOG")

echo "--- fair_smoke ---"
echo "fairwake_started=$fairstart fairwake_woke=$fairpass irq_creturn_dispatch_live=$fairfix burners_spawned=$burner panic=$panic lines=$(wc -l < "$LOG")"
echo "--- FAIRWAKE + fix lines ---"; grep -E 'FAIRWAKE|FAIRNESS: IRQ-dispatched' "$LOG" || echo "(none)"

if [[ "$fairpass" -ge 1 && "$fairfix" -ge 1 && "$burner" -ge 1 && "$panic" -eq 0 ]]; then
    echo "FAIRNESS: PASS irq_dispatches_resume_creturn=1 woke=1 stable=1"
    exit 0
else
    echo "FAIRNESS: FAIL (fix path not exercised, or fairwake starved, or panic)"
    exit 1
fi
