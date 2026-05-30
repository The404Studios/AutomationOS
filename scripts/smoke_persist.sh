#!/usr/bin/env bash
# =============================================================================
# smoke_persist.sh -- cross-reboot persistence smoketest for AutomationOS (#57)
# =============================================================================
#
# Proves diskfs (kernel/fs/diskfs.c) writes survive a power cycle. Strategy:
#
#   1. Create ONE fresh 16 MB raw disk image (all zero -- no MBR signature, so
#      SeaBIOS won't try to boot it; we also pass -boot d to force CD boot).
#   2. Boot the SAME image TWICE in a row over an AHCI controller:
#        boot 1: diskfs finds no superblock -> formats -> boot #1
#        boot 2: diskfs finds the superblock from boot 1 -> boot #2
#   3. Assert boot 1 logged "boot #1 (freshly formatted)" and boot 2 logged
#      "boot #2 (existing fs)". The counter advancing across a full reboot is
#      the proof that the write from boot 1 reached durable storage.
#
# Usage (from WSL Arch):
#   wsl -d Arch bash -lc 'cd /mnt/c/Users/wilde/Desktop/Kernel && bash scripts/smoke_persist.sh'
#   ... --build   # rebuild kernel + ISO first (quick_build.sh + build_all.sh)
#
# Exit codes: 0 = persistence verified, 1 = a check FAILED / preflight error.
# =============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_ROOT="$(dirname "$SCRIPT_DIR")"
ISO="${KERNEL_ROOT}/build/automationos.iso"
IMG="/tmp/automationos_persist.img"
LOG1="/tmp/smoke_persist_boot1.log"
LOG2="/tmp/smoke_persist_boot2.log"
QEMU_TIMEOUT=30

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'
pass()   { printf "  ${GREEN}[PASS]${RESET} %s\n" "$*"; }
fail()   { printf "  ${RED}[FAIL]${RESET} %s\n"   "$*"; }
info()   { printf "  ${CYAN}[INFO]${RESET} %s\n"  "$*"; }
warn()   { printf "  ${YELLOW}[WARN]${RESET} %s\n" "$*"; }
header() { printf "\n${BOLD}%s${RESET}\n" "$*"; }

DO_BUILD=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build|-b) DO_BUILD=1; shift ;;
        --iso)      ISO="$2"; shift 2 ;;
        --timeout)  QEMU_TIMEOUT="$2"; shift 2 ;;
        -h|--help)  grep '^#' "$0" | head -30 | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) printf "ERROR: Unknown option: %s\n" "$1" >&2; exit 1 ;;
    esac
done

maybe_build() {
    [[ $DO_BUILD -eq 1 ]] || return 0
    header "Step 0: Building kernel + ISO"
    bash "${KERNEL_ROOT}/scripts/quick_build.sh" || { echo "quick_build.sh failed" >&2; exit 1; }
    bash "${KERNEL_ROOT}/scripts/build_all.sh"   || { echo "build_all.sh failed"   >&2; exit 1; }
}

make_fresh_image() {
    header "Creating fresh disk image (zeroed, non-bootable)"
    rm -f "$IMG"
    if command -v qemu-img &>/dev/null; then
        qemu-img create -f raw "$IMG" 16M >/dev/null 2>&1
    elif command -v python3 &>/dev/null; then
        python3 - "$IMG" <<'PY'
import sys
open(sys.argv[1], 'wb').write(bytearray(16 * 1024 * 1024))
PY
    else
        fail "Need qemu-img or python3 to create the disk image"; exit 1
    fi
    [[ -f "$IMG" ]] || { fail "Failed to create $IMG"; exit 1; }
    info "Fresh image: $IMG ($(stat -c%s "$IMG") bytes)"
}

preflight() {
    header "Preflight checks"
    local ok=1
    if [[ ! -f "$ISO" ]]; then fail "ISO not found: ${ISO} (run with --build)"; ok=0
    else info "ISO: ${ISO} ($(stat -c%s "$ISO" 2>/dev/null || echo '?') bytes)"; fi
    if ! command -v qemu-system-x86_64 &>/dev/null; then fail "qemu-system-x86_64 not found"; ok=0
    else info "QEMU: $(qemu-system-x86_64 --version 2>&1 | head -1)"; fi
    [[ $ok -eq 1 ]]
}

# Boot the ISO once with the persistent disk attached, capturing serial to $1.
boot_once() {
    local logfile="$1"
    rm -f "$logfile"
    timeout "${QEMU_TIMEOUT}" \
        qemu-system-x86_64 \
            -cdrom "${ISO}" \
            -boot d \
            -m 512 \
            -device ahci,id=ahci0 \
            -drive id=disk0,file="${IMG}",if=none,format=raw \
            -device ide-hd,drive=disk0,bus=ahci0.0 \
            -serial "file:${logfile}" \
            -display none \
            -no-reboot \
        >/dev/null 2>&1 || true
    sleep 2
}

main() {
    printf "\n${BOLD}AutomationOS Persistence Smoketest (2-boot)${RESET}\n"
    printf "  ISO:   %s\n  Image: %s\n" "$ISO" "$IMG"

    maybe_build
    preflight || exit 1
    make_fresh_image

    header "Boot 1 of 2 (expect: format -> boot #1)"
    boot_once "$LOG1"
    [[ -s "$LOG1" ]] || { fail "Boot 1 produced no serial log"; exit 1; }

    header "Boot 2 of 2 (same disk; expect: existing fs -> boot #2)"
    boot_once "$LOG2"
    [[ -s "$LOG2" ]] || { fail "Boot 2 produced no serial log"; exit 1; }

    header "Persistence checks"
    local f=0

    # Boot 1: controller found + fresh format + boot #1.
    if grep -qF '[DISKFS] mounted: boot #1 (freshly formatted' "$LOG1"; then
        pass "Boot 1: formatted fresh disk and wrote boot #1"
    else
        fail "Boot 1: did not format + reach boot #1"
        grep -F '[DISKFS]' "$LOG1" | head -5 | sed 's/^/       /'
        f=1
    fi

    # Boot 2: superblock from boot 1 was found (existing fs) + counter advanced.
    if grep -qF '[DISKFS] mounted: boot #2 (existing fs' "$LOG2"; then
        pass "Boot 2: read boot-1 superblock and advanced to boot #2 -- PERSISTENCE VERIFIED"
    else
        fail "Boot 2: did not see persisted boot #2 (write from boot 1 did not survive)"
        grep -F '[DISKFS]' "$LOG2" | head -5 | sed 's/^/       /'
        f=1
    fi

    # Neither boot may have faulted.
    for L in "$LOG1" "$LOG2"; do
        if grep -qiE 'PANIC|CPU EXCEPTION|page fault' "$L"; then
            fail "Fault detected in $(basename "$L")"; f=1
        fi
    done
    [[ $f -eq 0 ]] && pass "No panic/exception/page-fault across both boots"

    printf '\n%s\n' "$(printf '=%.0s' {1..60})"
    if [[ $f -eq 0 ]]; then
        printf "${GREEN}${BOLD}  RESULT: PASS -- disk writes survive a reboot${RESET}\n\n"
        exit 0
    fi
    printf "${RED}${BOLD}  RESULT: FAIL -- persistence not verified${RESET}\n"
    printf "  Logs: %s , %s\n\n" "$LOG1" "$LOG2"
    exit 1
}

main "$@"
