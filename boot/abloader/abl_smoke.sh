#!/usr/bin/env bash
# =============================================================================
# abl_smoke.sh -- Headless boot smoketest for the ABLoader custom bootloader
# =============================================================================
#
# ABLoader is AutomationOS's own two-stage bootloader (stage1 MBR + stage2
# protected-mode loader) and an ALTERNATIVE to the production GRUB path. It
# loads build/kernel.elf + iso/boot/initrd.img off a flat raw disk image and
# synthesizes a Multiboot-1 handoff (magic in EAX, info struct in EBX with the
# E820 memory map, the initrd as module 0, and a VBE framebuffer) so the exact
# same multiboot kernel boots unmodified.
#
# This harness builds abloader.img with the real kernel, boots it headless in
# QEMU (booting from the raw IDE disk, NOT a CD), and asserts the kernel reaches
# the same milestones the GRUB smoke (scripts/smoke_boot.sh) checks for. It does
# NOT touch the production GRUB ISO -- the verified milestone is unaffected.
#
# Usage (from WSL Arch):
#   wsl -d Arch bash -lc 'cd /mnt/c/Users/wilde/Desktop/Kernel && bash boot/abloader/abl_smoke.sh'
#
# Requires build/kernel.elf and iso/boot/initrd.img to exist (build them first
# with scripts/quick_build.sh && scripts/build_all.sh). Exit 0 = PASS.
# =============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
IMG="${SCRIPT_DIR}/abloader.img"
LOG="/tmp/abl_smoke.log"
QEMU_TIMEOUT=30
MIN_PIDS=4

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'
pass()   { printf "  ${GREEN}[PASS]${RESET} %s\n" "$*"; }
fail()   { printf "  ${RED}[FAIL]${RESET} %s\n"   "$*"; }
info()   { printf "  ${CYAN}[INFO]${RESET} %s\n"  "$*"; }
header() { printf "\n${BOLD}%s${RESET}\n" "$*"; }

header "Step 0: Building abloader.img with the real kernel"
if [[ ! -f "${REPO_ROOT}/build/kernel.elf" ]]; then
    fail "build/kernel.elf missing -- run scripts/quick_build.sh first"; exit 1
fi
bash "${SCRIPT_DIR}/build.sh" >/tmp/abl_build.log 2>&1 || { fail "build.sh failed (see /tmp/abl_build.log)"; tail -20 /tmp/abl_build.log; exit 1; }
info "abloader.img: $(stat -c%s "$IMG" 2>/dev/null || echo '?') bytes"

header "Booting abloader.img headless (timeout ${QEMU_TIMEOUT}s)"
rm -f "$LOG"
# Boot from the raw IDE disk image -- stage1 in sector 0 is the MBR.
timeout "${QEMU_TIMEOUT}" \
    qemu-system-x86_64 \
        -drive format=raw,file="${IMG}",if=ide,index=0 \
        -m 512M \
        -serial "file:${LOG}" \
        -display none \
        -no-reboot \
    >/dev/null 2>&1 || true
sleep 2

if [[ ! -s "$LOG" ]]; then
    fail "Serial log empty after ${QEMU_TIMEOUT}s -- ABLoader may have failed to hand off"
    exit 1
fi

header "Boot invariant checks (ABLoader path)"
P=0; F=0
chk() { if eval "$2"; then pass "$1"; P=$((P+1)); else fail "$1"; F=$((F+1)); fi; }

chk "Stage2 synthesized multiboot handoff"   'grep -qF "Multiboot info at" "$LOG"'
chk "Kernel started ([KERNEL] tag)"          'grep -qF "[KERNEL]" "$LOG"'
chk "Initrd module passed + detected"        'grep -qF "Initrd detected" "$LOG"'
chk "Init finished (All services started)"   'grep -qF "All services started" "$LOG"'
chk "Processes spawned (>= ${MIN_PIDS})"     '[ "$(grep -cF "created with PID" "$LOG")" -ge "$MIN_PIDS" ]'
chk "Compositor/window created"              'grep -qiE "window.*created" "$LOG"'
chk "No PANIC"                               '! grep -qiF "PANIC" "$LOG"'
chk "No CPU EXCEPTION"                       '! grep -qF "CPU EXCEPTION" "$LOG"'
chk "No page fault"                          '! grep -qiF "page fault" "$LOG"'

printf '\n%s\n' "$(printf '=%.0s' {1..56})"
printf '  Total: %s   Passed: %s   Failed: %s\n' "$((P+F))" "$P" "$F"
printf '%s\n' "$(printf '=%.0s' {1..56})"
if [[ $F -eq 0 ]]; then
    printf "${GREEN}${BOLD}  RESULT: PASS -- ABLoader boots the real kernel to desktop${RESET}\n\n"
    exit 0
fi
printf "${RED}${BOLD}  RESULT: FAIL -- %d check(s) failed (full log: %s)${RESET}\n\n" "$F" "$LOG"
exit 1
