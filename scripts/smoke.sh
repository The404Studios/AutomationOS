#!/usr/bin/env bash
# =============================================================================
# smoke.sh -- Headless boot + syscall smoke test runner
# =============================================================================
#
# Usage (from project root inside WSL Arch):
#   bash scripts/smoke.sh [--iso PATH] [--timeout SECONDS] [--log PATH]
#
# Defaults:
#   --iso       build/automationos.iso
#   --timeout   60
#   --log       /tmp/smoke_serial.log
#
# Exit codes:
#   0  All [SMOKE] checks passed and DONE marker present
#   1  One or more checks FAILED, or DONE marker absent
#   2  Usage / setup error
#
# =============================================================================
#
# INTEGRATOR INSTRUCTIONS
# -----------------------
# Before running smoke.sh you must add sbin/smoketest to the initrd and
# arrange for init to spawn it.  Two options:
#
# Option A – direct init spawn (recommended)
#   1. Build smoketest.elf from your project root (WSL Arch):
#        gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
#            -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
#            -c userspace/apps/smoketest/smoketest.c -o /tmp/smoke.o
#        ld -nostdlib -static -n -no-pie -e _start \
#            -T userspace/userspace.ld /tmp/smoke.o -o build/smoketest.elf
#        # Confirm no stack canary:
#        objdump -d build/smoketest.elf | grep fs:0x28   # must be empty
#
#   2. Rebuild the initrd to include it:
#        # Extract existing initrd
#        mkdir -p /tmp/initrd_extract
#        cd /tmp/initrd_extract && tar xf /mnt/c/Users/wilde/Desktop/Kernel/iso/boot/initrd.img
#        # Copy smoketest binary
#        cp /mnt/c/Users/wilde/Desktop/Kernel/build/smoketest.elf sbin/smoketest
#        chmod +x sbin/smoketest
#        # Re-pack
#        tar --format=ustar -cf /mnt/c/Users/wilde/Desktop/Kernel/iso/boot/initrd.img .
#        cd /mnt/c/Users/wilde/Desktop/Kernel
#
#   3. Edit userspace/init/init.c (or the live init ELF in the initrd) to
#      add a spawn/execve of "/sbin/smoketest" at startup, then rebuild.
#      If using the minimal inline-syscall init, add:
#        sc(SYS_SPAWN, (long)"/sbin/smoketest", 0, 0);
#      after it spawns the shell.
#
#   4. Rebuild the ISO:
#        grub-mkrescue -o build/automationos.iso iso/
#
# Option B – run from the terminal's `run` command (no rebuild needed):
#   Boot normally, then at the terminal prompt type:
#     run /sbin/smoketest
#   and watch the serial log (the terminal echoes serial output).
#
# =============================================================================

set -euo pipefail

# ── Default values ─────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_ROOT="$(dirname "$SCRIPT_DIR")"
ISO="${KERNEL_ROOT}/build/automationos.iso"
TIMEOUT=60
LOG="/tmp/smoke_serial.log"

# ── Argument parsing ────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --iso)      ISO="$2";     shift 2 ;;
        --timeout)  TIMEOUT="$2"; shift 2 ;;
        --log)      LOG="$2";     shift 2 ;;
        -h|--help)
            sed -n '2,60p' "$0" | grep '^#' | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "ERROR: Unknown option: $1" >&2
            exit 2
            ;;
    esac
done

# ── Pre-flight checks ───────────────────────────────────────────────────────
if [[ ! -f "$ISO" ]]; then
    echo "ERROR: ISO not found: $ISO" >&2
    echo "       Run 'wsl -d Arch bash -lc \"cd /mnt/c/Users/wilde/Desktop/Kernel && bash scripts/quick_build.sh\"'" >&2
    echo "       then rebuild the ISO with: grub-mkrescue -o build/automationos.iso iso/" >&2
    exit 2
fi

if ! command -v qemu-system-x86_64 &>/dev/null; then
    echo "ERROR: qemu-system-x86_64 not found in PATH" >&2
    exit 2
fi

# ── Boot the ISO headlessly ─────────────────────────────────────────────────
echo "================================================"
echo "  AutomationOS Smoke Test"
echo "================================================"
echo "  ISO:     $ISO"
echo "  Timeout: ${TIMEOUT}s"
echo "  Log:     $LOG"
echo ""

rm -f "$LOG"

echo "[smoke] Booting ISO (timeout ${TIMEOUT}s)..."

# Run QEMU in background; kill it after TIMEOUT or when DONE marker appears
qemu-system-x86_64 \
    -cdrom "$ISO" \
    -m 512 \
    -serial "file:${LOG}" \
    -display none \
    -no-reboot \
    -no-shutdown \
    -device intel-hda -device hda-duplex \
    &
QEMU_PID=$!

# Wait for [SMOKE] DONE marker or timeout
ELAPSED=0
FOUND_DONE=0
while [[ $ELAPSED -lt $TIMEOUT ]]; do
    sleep 1
    ELAPSED=$((ELAPSED + 1))
    # Check if QEMU already exited
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "[smoke] QEMU exited after ${ELAPSED}s"
        FOUND_DONE=1
        break
    fi
    # Check if DONE marker has appeared in the log
    if [[ -f "$LOG" ]] && grep -q '\[SMOKE\] DONE:' "$LOG" 2>/dev/null; then
        echo "[smoke] DONE marker found after ${ELAPSED}s"
        FOUND_DONE=1
        # Give QEMU a moment to flush, then kill
        sleep 1
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
        break
    fi
done

# If still running, kill it
if kill -0 "$QEMU_PID" 2>/dev/null; then
    echo "[smoke] Timeout reached (${TIMEOUT}s); killing QEMU"
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
fi

echo ""

# ── Parse results ───────────────────────────────────────────────────────────
if [[ ! -f "$LOG" ]]; then
    echo "FAIL: Serial log not created (QEMU may not have started)" >&2
    exit 1
fi

echo "── [SMOKE] lines from serial log ──────────────────"
grep '\[SMOKE\]' "$LOG" || true
echo "────────────────────────────────────────────────────"
echo ""

FAIL_COUNT=0
PASS_COUNT=0
TOTAL_COUNT=0

while IFS= read -r line; do
    if [[ "$line" == *"[SMOKE]"* ]]; then
        if [[ "$line" == *": PASS"* ]]; then
            PASS_COUNT=$((PASS_COUNT + 1))
            TOTAL_COUNT=$((TOTAL_COUNT + 1))
        elif [[ "$line" == *": FAIL"* ]]; then
            FAIL_COUNT=$((FAIL_COUNT + 1))
            TOTAL_COUNT=$((TOTAL_COUNT + 1))
        fi
    fi
done < "$LOG"

# Check for DONE marker
DONE_PRESENT=0
if grep -q '\[SMOKE\] DONE:' "$LOG" 2>/dev/null; then
    DONE_PRESENT=1
fi

echo "── Summary ─────────────────────────────────────────"
echo "  Checks PASSED:  $PASS_COUNT"
echo "  Checks FAILED:  $FAIL_COUNT"
echo "  DONE marker:    $([ $DONE_PRESENT -eq 1 ] && echo 'YES' || echo 'NO (timeout or crash)')"
echo "────────────────────────────────────────────────────"

if [[ $DONE_PRESENT -eq 0 ]]; then
    echo ""
    echo "RESULT: FAIL -- [SMOKE] DONE marker not found (kernel may have crashed or smoketest not running)"
    echo "        Check full log: $LOG"
    exit 1
fi

if [[ $FAIL_COUNT -gt 0 ]]; then
    echo ""
    echo "RESULT: FAIL -- $FAIL_COUNT check(s) failed"
    exit 1
fi

if [[ $PASS_COUNT -eq 0 ]]; then
    echo ""
    echo "RESULT: FAIL -- no PASS lines found (smoketest may not have run)"
    exit 1
fi

echo ""
echo "RESULT: PASS -- all $PASS_COUNT check(s) passed"
exit 0
