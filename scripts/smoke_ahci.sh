#!/usr/bin/env bash
# =============================================================================
# smoke_ahci.sh -- AHCI/SATA read-path smoketest for AutomationOS (#13)
# =============================================================================
#
# The default smoke (scripts/smoke_boot.sh) boots QEMU with NO disk, so it can
# only prove the AHCI driver does not destabilize a diskless boot. THIS harness
# attaches a real SATA disk and proves the driver works end-to-end:
#
#   1. Builds a 16 MB raw disk image (a regular FILE -- never a block device)
#      with a known MBR signature (0xAA55 at byte 510/511) and an 8-byte marker
#      (de ad be ef ca fe ba be) at the very start of sector 0.
#   2. Boots the ISO under QEMU with an AHCI controller + that disk:
#        -device ahci,id=ahci0
#        -drive id=disk0,file=IMG,if=none,format=raw
#        -device ide-hd,drive=disk0,bus=ahci0.0
#   3. The kernel's boot-time self-test reads LBA 0 over AHCI DMA and logs the
#      signature + first 8 bytes. We assert it read back EXACTLY our bytes,
#      which proves IDENTIFY + the command-list/PRDT/DMA read path all work.
#
# Usage (from WSL Arch):
#   wsl -d Arch bash -lc 'cd /mnt/c/Users/wilde/Desktop/Kernel && bash scripts/smoke_ahci.sh'
#   ... --build   # rebuild kernel + ISO first (quick_build.sh + build_all.sh)
#
# Exit codes: 0 = all AHCI checks PASSED, 1 = a check FAILED / preflight error.
# =============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_ROOT="$(dirname "$SCRIPT_DIR")"
ISO="${KERNEL_ROOT}/build/automationos.iso"
IMG="/tmp/automationos_ahci_test.img"
LOG="/tmp/smoke_ahci.log"
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

# Build the test disk image (regular file). Prefer python3; fall back to perl.
make_test_image() {
    header "Building test disk image"
    rm -f "$IMG"
    if command -v python3 &>/dev/null; then
        python3 - "$IMG" <<'PY'
import sys
buf = bytearray(16 * 1024 * 1024)               # 16 MB, all zero
buf[0:8] = bytes([0xDE,0xAD,0xBE,0xEF,0xCA,0xFE,0xBA,0xBE])  # sector-0 marker
buf[510] = 0x55                                  # MBR boot signature (LE 0xAA55)
buf[511] = 0xAA
open(sys.argv[1], 'wb').write(buf)
PY
    elif command -v perl &>/dev/null; then
        perl -e 'my $b = "\x00" x (16*1024*1024);
                 substr($b,0,8)="\xDE\xAD\xBE\xEF\xCA\xFE\xBA\xBE";
                 substr($b,510,2)="\x55\xAA";
                 open(F,">",$ARGV[0]); binmode F; print F $b; close F;' "$IMG"
    else
        fail "Need python3 or perl to build the test image"; exit 1
    fi
    if [[ -f "$IMG" ]]; then
        info "Test image: $IMG ($(stat -c%s "$IMG") bytes), sig=0xAA55, marker=deadbeefcafebabe"
    else
        fail "Failed to create test image"; exit 1
    fi
}

preflight() {
    header "Preflight checks"
    local ok=1
    if [[ ! -f "$ISO" ]]; then
        fail "ISO not found: ${ISO} (run with --build)"; ok=0
    else
        info "ISO: ${ISO} ($(stat -c%s "$ISO" 2>/dev/null || echo '?') bytes)"
    fi
    if ! command -v qemu-system-x86_64 &>/dev/null; then
        fail "qemu-system-x86_64 not found in PATH"; ok=0
    else
        info "QEMU: $(qemu-system-x86_64 --version 2>&1 | head -1)"
    fi
    [[ $ok -eq 1 ]]
}

boot_qemu() {
    header "Booting ISO with AHCI disk attached (timeout ${QEMU_TIMEOUT}s)"
    rm -f "$LOG"
    # '-boot d' forces CD-ROM boot: our test image carries a valid 0xAA55 MBR
    # signature, so SeaBIOS would otherwise try to boot the (codeless) SATA disk
    # and hang with no serial output.
    timeout "${QEMU_TIMEOUT}" \
        qemu-system-x86_64 \
            -cdrom "${ISO}" \
            -boot d \
            -m 512 \
            -device ahci,id=ahci0 \
            -drive id=disk0,file="${IMG}",if=none,format=raw \
            -device ide-hd,drive=disk0,bus=ahci0.0 \
            -serial "file:${LOG}" \
            -display none \
            -no-reboot \
        >/dev/null 2>&1 || true
    info "QEMU exited; waiting for log flush..."
    sleep 2
}

wait_for_log() {
    local n=3
    while [[ $n -gt 0 ]]; do
        [[ -f "$LOG" && -s "$LOG" ]] && return 0
        sleep 1; n=$((n-1))
    done
    return 1
}

# ── AHCI-specific checks (grep -F: "[AHCI]" must be literal, not a regex class) ─
check_controller_found() {
    if grep -qF '[AHCI] Found controller' "$LOG"; then
        pass "AHCI controller detected over PCI: $(grep -F '[AHCI] Found controller' "$LOG" | head -1 | sed 's/^.*Found/Found/')"
        return 0
    fi
    fail "AHCI controller NOT detected (no '[AHCI] Found controller' line)"
    return 1
}

check_port_ready() {
    if grep -qF 'ready: model=' "$LOG"; then
        pass "Port came up + IDENTIFY ok: $(grep -F 'ready: model=' "$LOG" | head -1 | sed 's/^.*Port/Port/')"
        return 0
    fi
    fail "No SATA port reached ready (IDENTIFY may have failed)"
    return 1
}

check_sector0_read() {
    if grep -qF 'sector0 read OK: MBR sig=0xaa55 first8=deadbeefcafebabe' "$LOG"; then
        pass "Sector 0 DMA read returned EXACTLY our bytes (sig=0xaa55, marker=deadbeefcafebabe)"
        return 0
    elif grep -qF 'sector0 read OK' "$LOG"; then
        fail "Sector 0 read succeeded but bytes differ from expected:"
        grep -F 'sector0 read OK' "$LOG" | head -1 | sed 's/^/       /'
        return 1
    elif grep -qF 'sector0 read FAILED' "$LOG"; then
        fail "Sector 0 read FAILED (DMA/command path error)"
        return 1
    fi
    fail "No sector-0 self-test line found (driver may not have reached the read)"
    return 1
}

check_no_panic()      { if grep -qiF 'PANIC' "$LOG"; then fail "PANIC detected"; return 1; fi; pass "No PANIC"; }
check_no_exception()  { if grep -qF 'CPU EXCEPTION' "$LOG"; then fail "CPU EXCEPTION detected"; return 1; fi; pass "No CPU EXCEPTION"; }
check_no_pagefault()  { if grep -qiF 'page fault' "$LOG"; then fail "Page fault detected"; return 1; fi; pass "No page fault"; }
check_services_up()   { if grep -qF 'All services started' "$LOG"; then pass "Boot still reaches 'All services started' (no AHCI regression)"; return 0; fi; fail "Boot did NOT finish init with disk attached (AHCI regression?)"; return 1; }

run_checks() {
    header "AHCI invariant checks"
    local checks=(
        check_controller_found
        check_port_ready
        check_sector0_read
        check_services_up
        check_no_panic
        check_no_exception
        check_no_pagefault
    )
    _PASS=0; _FAIL=0
    for fn in "${checks[@]}"; do
        if "$fn"; then _PASS=$((_PASS+1)); else _FAIL=$((_FAIL+1)); fi
    done
}

show_excerpt() {
    header "AHCI/BLOCK log lines"
    grep -E '\[AHCI\]|\[BLOCK\]|\[PCI\].*01:06' "$LOG" | head -30 | sed 's/^/  /' || true
}

print_summary() {
    local total=$((_PASS + _FAIL))
    printf '\n%s\n' "$(printf '=%.0s' {1..60})"
    printf '  Total: %s   Passed: %s   Failed: %s\n' "$total" "$_PASS" "$_FAIL"
    printf '%s\n' "$(printf '=%.0s' {1..60})"
    if [[ $_FAIL -eq 0 ]]; then
        printf "${GREEN}${BOLD}  RESULT: PASS -- AHCI read path verified end-to-end${RESET}\n\n"
        return 0
    fi
    printf "${RED}${BOLD}  RESULT: FAIL -- %d AHCI check(s) failed${RESET}\n  Full log: %s\n\n" "$_FAIL" "$LOG"
    return 1
}

main() {
    printf "\n${BOLD}AutomationOS AHCI/SATA Smoketest${RESET}\n"
    printf "  ISO:   %s\n  Image: %s\n  Log:   %s\n" "$ISO" "$IMG" "$LOG"
    maybe_build
    preflight || exit 1
    make_test_image
    boot_qemu
    if ! wait_for_log; then
        fail "Serial log empty/missing after ${QEMU_TIMEOUT}s — QEMU may have failed to start"
        exit 1
    fi
    show_excerpt
    run_checks
    print_summary
}

main "$@"
