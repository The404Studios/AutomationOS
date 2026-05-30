#!/bin/bash
# =============================================================================
# stress_preempt.sh -- end-to-end stress test for the GATED preemptive scheduler.
#
# The decisive proof that preemption works: spawn six CPU burners that NEVER
# yield (sbin/cpuburn) plus three SSE floattest runs, boot them on the PREEMPTIVE
# kernel, and confirm (a) no faults and (b) ALL six burners make progress
# (interleaved heartbeats == fair time-slicing of ring 3). On the cooperative
# kernel the first burner would hang the box -- which is why the workload is
# gated behind -DPREEMPT_STRESS (STRESS=1) and only ever runs here.
#
# Steps:
#   1. PREEMPT=1 quick_build.sh    -> build/kernel-preempt.elf
#   2. cp that over build/kernel.elf so build_all packages the PREEMPTIVE kernel,
#      then STRESS=1 build_all.sh   -> ISO with cpuburners spawned by init
#   3. boot ~60s, capture serial to /tmp/stress.log
#   4. ALWAYS restore the cooperative kernel (quick_build.sh) so the tree is left
#      in its default cooperative state regardless of how the run ended.
#
# Run via:  wsl -d Arch bash -lc 'cd /mnt/c/Users/wilde/Desktop/Kernel && bash scripts/stress_preempt.sh'
# =============================================================================
set -e
cd "$(dirname "$0")/.."

LOG="/tmp/stress.log"
BOOT_SECS="${BOOT_SECS:-60}"

# --- restore-the-cooperative-kernel trap -----------------------------------
# No matter how this script exits (success, fault, ^C, build error), rebuild the
# DEFAULT cooperative kernel so the tree is never left preemptive. We also
# re-stamp build/kernel.elf in case step 2 clobbered it with the preempt kernel.
restore_cooperative() {
    echo ""
    echo "[stress] ===== restoring cooperative kernel (default build) ====="
    bash scripts/quick_build.sh >/tmp/stress_restore.log 2>&1 || {
        echo "[stress] WARNING: cooperative restore build failed; see /tmp/stress_restore.log"
        return 0
    }
    echo "[stress] cooperative build/kernel.elf restored ($(stat -c%s build/kernel.elf 2>/dev/null || echo '?') bytes)"
}
trap restore_cooperative EXIT

# --- step 1: build the PREEMPTIVE kernel -----------------------------------
echo "[stress] ===== step 1: PREEMPT=1 quick_build.sh ====="
PREEMPT=1 bash scripts/quick_build.sh
if [ ! -f build/kernel-preempt.elf ]; then
    echo "[stress] FATAL: build/kernel-preempt.elf was not produced" >&2
    exit 1
fi
echo "[stress] preempt kernel: $(stat -c%s build/kernel-preempt.elf) bytes"

# --- step 2: package the preempt kernel into the ISO with the stress init ---
echo "[stress] ===== step 2: cp preempt kernel over kernel.elf + STRESS=1 build_all.sh ====="
# build_all.sh installs build/kernel.elf into iso/boot/kernel.elf; the simplest
# way to make the ISO use the preemptive kernel is to overwrite kernel.elf with
# the preempt build for the packaging step. (The EXIT trap rebuilds the real
# cooperative kernel.elf afterward.)
cp build/kernel-preempt.elf build/kernel.elf
STRESS=1 bash scripts/build_all.sh
if [ ! -f build/automationos.iso ]; then
    echo "[stress] FATAL: build/automationos.iso was not produced" >&2
    exit 1
fi
echo "[stress] stress ISO: $(stat -c%s build/automationos.iso) bytes"

# --- step 3: boot ~BOOT_SECS with serial capture ---------------------------
echo "[stress] ===== step 3: booting ${BOOT_SECS}s (serial -> ${LOG}) ====="
rm -f "$LOG"
# `timeout` ends the run; -no-reboot makes a triple-fault HALT (so a reboot loop
# shows as a single boot that stops) rather than silently restarting.
timeout "${BOOT_SECS}" qemu-system-x86_64 \
    -cdrom build/automationos.iso \
    -m 512 \
    -serial "file:${LOG}" \
    -display none \
    -no-reboot \
    -netdev user,id=n0 -device e1000,netdev=n0 || true
echo "[stress] boot finished; serial log: $(stat -c%s "$LOG" 2>/dev/null || echo 0) bytes"

# --- step 4 (restore) runs automatically via the EXIT trap -----------------

# --- quick inline summary (full verification is done by the caller) --------
echo ""
echo "[stress] ===== quick log summary (${LOG}) ====="
# grep -c always prints a number (0 on no match) and we don't want the OR-branch
# to tack on a second line, so we guard the whole pipeline's exit, not grep's.
cnt() { local n; n=$(grep -c "$1" "$LOG" 2>/dev/null); echo "${n:-0}"; }
for pat in "PANIC" "CPU EXCEPTION" "page fault" "GENERAL PROTECTION" "DOUBLE FAULT"; do
    printf '  %-20s : %s\n' "$pat" "$(cnt "$pat")"
done
printf '  %-20s : %s\n' "CPUBURN beats"  "$(cnt '\[CPUBURN id=.*beat')"
printf '  %-20s : %s\n' "FLOATTEST PASS" "$(cnt 'FLOATTEST: PASS')"
echo "[stress] distinct cpuburn ids that printed a beat:"
grep -oE '\[CPUBURN id=[0-9?]+\] beat' "$LOG" 2>/dev/null | grep -oE 'id=[0-9?]+' | sort -u | sed 's/^/  /' || echo "  (none)"
echo "[stress] done."
