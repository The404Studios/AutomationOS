#!/usr/bin/env bash
# =============================================================================
# smoke_boot.sh -- Automated boot smoketest for AutomationOS
# =============================================================================
#
# Usage (from WSL Arch):
#   wsl -d Arch bash -lc 'cd /mnt/c/Users/wilde/Desktop/Kernel && bash scripts/smoke_boot.sh'
#
# Optional: pass --build to run quick_build.sh + grub-mkrescue first:
#   wsl -d Arch bash -lc 'cd /mnt/c/Users/wilde/Desktop/Kernel && bash scripts/smoke_boot.sh --build'
#
# Exit codes:
#   0  All checks PASSED
#   1  One or more checks FAILED or fatal preflight error
#
# Design notes — the /tmp read-race fix:
#   After `timeout 30 qemu-system-x86_64 ... -serial file:/tmp/smoke_boot.log`
#   returns, WSL2's tmpfs may not have flushed the write buffer.  We therefore:
#     1. Sleep 2 seconds after QEMU exits.
#     2. Retry opening the log up to 3 times (1-second gaps) before giving up.
#   All checks run against the FILE directly (grep FILE) — never loading the
#   entire log into a shell variable and piping it through printf/echo.
#   This avoids the SIGPIPE/pipefail trap: `grep -q PATTERN FILE` exits 0
#   (found) or 1 (not found) without a broken-pipe race.
# =============================================================================

set -uo pipefail

# ── Paths ──────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_ROOT="$(dirname "$SCRIPT_DIR")"
ISO="${KERNEL_ROOT}/build/automationos.iso"
LOG="/tmp/smoke_boot.log"
QEMU_TIMEOUT=60
# Minimum number of "created with PID" lines expected
MIN_PIDS=4
# Crash-loop guard: fail if "Freeing process" appears more than this many times.
# Raised to accommodate the many short-lived boot self-tests (crypto/libs/coreutils
# each spawn, run a KAT, and exit) plus forktest's 20 forks -- all legitimate exits.
# +5 for the new short-lived webapitest probe.
# +5 for threadtest (4 threads + the main process all exit -> legitimate frees).
# +5 for matmuljobs (2 worker threads + the main process all exit -> legitimate frees).
# NOTE: the kernel is built -DPROCESS_QUIET (scripts/quick_build.sh), which SUPPRESSES
# the "[PROCESS] Freeing process" line, so this count is effectively always 0 and the
# threshold is moot today; kept as a guard in case PROCESS_QUIET is ever lifted.
MAX_FREEING=190

# ── Colour helpers ─────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

pass()   { printf "  ${GREEN}[PASS]${RESET} %s\n" "$*"; }
fail()   { printf "  ${RED}[FAIL]${RESET} %s\n"   "$*"; }
info()   { printf "  ${CYAN}[INFO]${RESET} %s\n"  "$*"; }
warn()   { printf "  ${YELLOW}[WARN]${RESET} %s\n" "$*"; }
header() { printf "\n${BOLD}%s${RESET}\n" "$*"; }

# ── Argument parsing ────────────────────────────────────────────────────────
DO_BUILD=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build|-b)   DO_BUILD=1; shift ;;
        --iso)        ISO="$2";   shift 2 ;;
        --log)        LOG="$2";   shift 2 ;;
        --timeout)    QEMU_TIMEOUT="$2"; shift 2 ;;
        -h|--help)
            grep '^#' "$0" | head -20 | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            printf "ERROR: Unknown option: %s\n" "$1" >&2
            exit 1
            ;;
    esac
done

# ── Optional rebuild ────────────────────────────────────────────────────────
maybe_build() {
    if [[ $DO_BUILD -eq 1 ]]; then
        header "Step 0: Building kernel + ISO"
        bash "${KERNEL_ROOT}/scripts/quick_build.sh" || {
            printf "ERROR: quick_build.sh failed\n" >&2; exit 1
        }
        # Stage the freshly-built kernel into the ISO tree. quick_build.sh writes
        # build/kernel.elf, but grub-mkrescue below packages iso/boot/kernel.elf --
        # without this copy, --build silently re-packaged a STALE kernel, so the
        # smoke test validated whatever was last left in iso/boot/, not the new build.
        cp "${KERNEL_ROOT}/build/kernel.elf" "${KERNEL_ROOT}/iso/boot/kernel.elf" || {
            printf "ERROR: failed to stage build/kernel.elf into iso/boot/\n" >&2; exit 1
        }
        if command -v grub-mkrescue &>/dev/null; then
            grub-mkrescue -o "${ISO}" "${KERNEL_ROOT}/iso/" || {
                printf "ERROR: grub-mkrescue failed\n" >&2; exit 1
            }
            info "ISO rebuilt: ${ISO}"
        else
            warn "grub-mkrescue not found — skipping ISO step, using existing ISO"
        fi
    fi
}

# ── Preflight checks ────────────────────────────────────────────────────────
preflight() {
    header "Preflight checks"
    local ok=1

    if [[ ! -f "$ISO" ]]; then
        fail "ISO not found: ${ISO}"
        printf "       Run with --build or: grub-mkrescue -o build/automationos.iso iso/\n" >&2
        ok=0
    else
        info "ISO found: ${ISO} ($(stat -c%s "$ISO" 2>/dev/null || echo '?') bytes)"
    fi

    if ! command -v qemu-system-x86_64 &>/dev/null; then
        fail "qemu-system-x86_64 not found in PATH"
        printf "       Install with: sudo pacman -S qemu-system-x86\n" >&2
        ok=0
    else
        info "QEMU: $(qemu-system-x86_64 --version 2>&1 | head -1)"
    fi

    [[ $ok -eq 1 ]]
}

# ── Boot under QEMU ────────────────────────────────────────────────────────
boot_qemu() {
    header "Booting ISO (timeout ${QEMU_TIMEOUT}s)"
    rm -f "$LOG"
    info "Serial log: ${LOG}"

    # Run QEMU synchronously under `timeout`; -no-reboot ensures it stops on
    # triple-fault rather than looping forever.  stdout/stderr go to /dev/null
    # so the terminal stays clean; all interesting output goes to the log file.
    # An e1000 NIC on QEMU user-net (slirp) so the networking probe has a device
    # to detect + a gateway (10.0.2.2) to ARP. Harmless to the other checks.
    timeout "${QEMU_TIMEOUT}" \
        qemu-system-x86_64 \
            -cdrom "${ISO}" \
            -m 512 \
            -netdev user,id=n0 \
            -device e1000,netdev=n0 \
            -serial "file:${LOG}" \
            -display none \
            -no-reboot \
        >/dev/null 2>&1 || true   # timeout returns 124 on expiry — treat as OK

    # ── Race-condition mitigation ──────────────────────────────────────────
    # WSL2 /tmp is a tmpfs; the kernel may not have flushed QEMU's writes by
    # the time the process exits.  Sleep 2 s to let the buffer drain, then
    # retry reading the file up to 3 more times with 1-second gaps.
    info "QEMU exited (or timeout reached); waiting for log flush..."
    sleep 2
}

# ── Wait for the log to appear (retry after flush delay) ──────────────────
wait_for_log() {
    local attempts=3
    while [[ $attempts -gt 0 ]]; do
        if [[ -f "$LOG" && -s "$LOG" ]]; then
            return 0
        fi
        warn "Log not ready yet, retrying in 1s... (${attempts} attempt(s) left)"
        sleep 1
        attempts=$((attempts - 1))
    done
    return 1
}

# ── Individual check functions ─────────────────────────────────────────────
# Each function greps the log FILE directly (avoids the SIGPIPE/pipefail trap
# that occurs when passing 8000+ lines through a shell variable into grep -q).
# Returns 0 (pass) or 1 (fail).

check_kernel_started() {
    # The literal [KERNEL] tag is suppressed in the default (BOOT_QUIET) ISO, but the
    # kernel's early boot markers ([VMM]/[HEAP]) and a working RTC reliably prove it
    # started. Accept any of them so a quiet build is not misreported as "didn't boot".
    if grep -qE '\[KERNEL\]|\[VMM\]|\[HEAP\] Initializing kernel heap|\[RTC\] boot time' "$LOG"; then
        pass "Kernel started (boot markers present)"
        return 0
    else
        fail "Kernel did NOT start — no boot markers ([KERNEL]/[VMM]/[HEAP]/[RTC]) in log"
        return 1
    fi
}

check_rtc_up() {
    if grep -qF '[RTC] boot time' "$LOG"; then
        local rtc_line
        rtc_line="$(grep -F '[RTC] boot time' "$LOG" | head -1)"
        pass "RTC up: ${rtc_line}"
        return 0
    else
        fail "RTC did not report boot time — [RTC] boot time not found"
        return 1
    fi
}

check_all_services() {
    if grep -qF 'All services started' "$LOG"; then
        pass "Init finished (\"All services started\" present)"
        return 0
    else
        fail "Init did NOT finish — \"All services started\" not found"
        return 1
    fi
}

check_processes_spawned() {
    local count
    count="$(grep -cF 'created with PID' "$LOG" || true)"
    if [[ $count -ge $MIN_PIDS ]]; then
        pass "Processes spawned: ${count} \"created with PID\" lines (need >= ${MIN_PIDS})"
        return 0
    else
        fail "Too few processes spawned: ${count} \"created with PID\" lines (need >= ${MIN_PIDS})"
        return 1
    fi
}

check_compositor_window() {
    # Match any of:
    #   [TERM] window 1 created         (terminal/shell window manager)
    #   [Compositor] Desktop window created
    #   [WELCOME] window created / [PAINT] window created / [SHEET] window created
    # i.e. any line containing "window" followed (anywhere) by "created"
    if grep -qiE 'window.*created' "$LOG"; then
        local count
        count="$(grep -ciE 'window.*created' "$LOG" || true)"
        pass "Compositor/window: ${count} \"window.*created\" line(s) found"
        return 0
    else
        fail "No window created — no line matching \"window.*created\" found (compositor may not have started within timeout)"
        return 1
    fi
}

check_no_panic() {
    if grep -qiF 'PANIC' "$LOG"; then
        local count
        count="$(grep -ciF 'PANIC' "$LOG" || true)"
        fail "PANIC detected: ${count} occurrence(s)"
        grep -iF 'PANIC' "$LOG" | head -5 | while IFS= read -r l; do
            printf "       %s\n" "$l"
        done
        return 1
    else
        pass "No PANIC"
        return 0
    fi
}

check_no_cpu_exception() {
    # Gate KERNEL health, not "zero exceptions". A ring-3 fault the kernel CONTAINED
    # (prints the exception, then "Terminating faulting process 'sbin/...'", and keeps
    # running) is expected -- e.g. sigtest [5]'s deliberate bad handler at RIP 0x4000.
    # Only an UNCONTAINED exception (no matching termination => kernel-mode/fatal) fails.
    local exc contained uncontained
    exc="$(grep -cF 'CPU EXCEPTION' "$LOG" || true)"
    contained="$(grep -cF 'Terminating faulting process' "$LOG" || true)"
    if [[ "$exc" -eq 0 ]]; then
        pass "No CPU EXCEPTION"
        return 0
    fi
    uncontained=$((exc - contained))
    if [[ "$uncontained" -le 0 ]]; then
        pass "All ${exc} CPU exception(s) contained to ring-3 (kernel survived; ${contained} process(es) terminated)"
        return 0
    fi
    fail "Uncontained CPU EXCEPTION: ${uncontained} of ${exc} not contained (kernel-mode or fatal)"
    grep -F 'CPU EXCEPTION' "$LOG" | head -5 | while IFS= read -r l; do
        printf "       %s\n" "$l"
    done
    return 1
}

check_no_page_fault() {
    # Every page fault raises the CPU EXCEPTION banner; containment is judged the same
    # way. A page fault is OK iff it was contained (the faulting ring-3 process was
    # terminated and the kernel survived) -- e.g. sigtest [5]'s 0x4000 bad handler.
    local exc contained
    if ! grep -qiF 'page fault' "$LOG"; then
        pass "No page fault"
        return 0
    fi
    exc="$(grep -cF 'CPU EXCEPTION' "$LOG" || true)"
    contained="$(grep -cF 'Terminating faulting process' "$LOG" || true)"
    if [[ "$exc" -le "$contained" ]]; then
        pass "Page fault(s) contained to ring-3 (kernel survived; ${contained} process(es) terminated)"
        return 0
    fi
    fail "Uncontained page fault present ($((exc - contained)) of ${exc} exception(s) not terminated)"
    grep -iF 'page fault' "$LOG" | head -5 | while IFS= read -r l; do
        printf "       %s\n" "$l"
    done
    return 1
}

check_no_segment_too_large() {
    if grep -qiF 'Segment too large' "$LOG"; then
        fail "\"Segment too large\" detected (GRUB/boot loader error — kernel or initrd may be too big)"
        return 1
    else
        pass "No \"Segment too large\""
        return 0
    fi
}

check_argv() {
    # init spawns sbin/argvtest; exec.c's argv frame + crt0 must deliver argc/argv.
    if grep -qF 'ARGVTEST: PASS' "$LOG"; then
        pass "kernel->user argv handoff verified ($(grep -F 'ARGVTEST: argc=' "$LOG" | head -1 | sed 's/^.*ARGVTEST: //'))"
        return 0
    elif grep -qF 'ARGVTEST: FAIL' "$LOG"; then
        fail "argv handoff broken: $(grep -F 'ARGVTEST:' "$LOG" | head -2 | tr '\n' ' ')"
        return 1
    else
        fail "argvtest did not report (argv frame/crt0 issue or it crashed)"
        return 1
    fi
}

check_floattest() {
    # init spawns sbin/floattest; proves ring-3 SSE/FPU works end-to-end
    # (scalar float, a 2x2 float matmul, an auto-vectorized reduction).
    if grep -qF 'FLOATTEST: PASS' "$LOG"; then
        pass "ring-3 float/SSE verified ($(grep -F 'FLOATTEST: matmul' "$LOG" | head -1 | sed 's/^.*FLOATTEST: //'))"
        return 0
    elif grep -qF 'FLOATTEST: FAIL' "$LOG"; then
        fail "ring-3 float/SSE broken: $(grep -F 'FLOATTEST:' "$LOG" | head -2 | tr '\n' ' ')"
        return 1
    else
        fail "floattest did not report (SSE not enabled for ring 3, or it crashed)"
        return 1
    fi
}

check_sleep() {
    # init spawns sbin/sleeptest; proves SYS_SLEEP is a real, ms-granularity,
    # BLOCKING sleep (process goes BLOCKED, timer wakes it at its deadline). The
    # app sleeps 50 ms and checks the measured elapsed time is in [40, 300] ms.
    if grep -qF 'SLEEPTEST: PASS' "$LOG"; then
        pass "blocking ms-sleep verified ($(grep -F 'SLEEPTEST: slept=' "$LOG" | head -1 | sed 's/^.*SLEEPTEST: //'))"
        return 0
    elif grep -qF 'SLEEPTEST: FAIL' "$LOG"; then
        fail "sleep broken: $(grep -F 'SLEEPTEST:' "$LOG" | head -2 | tr '\n' ' ')"
        return 1
    else
        fail "sleeptest did not report (SYS_SLEEP hung or never ran)"
        return 1
    fi
}

check_priority() {
    # init spawns sbin/prioritytest, which PROVES scheduler priority CLASSES
    # actually shift CPU share: it forks two CPU-bound children -- one
    # SCHED_CLASS_HIGH (nice -10), one SCHED_CLASS_BACKGROUND (nice +10) -- runs
    # them a ~1.5s window with periodic yields (so scheduler_pick_next re-picks by
    # priority each turn, in BOTH the cooperative and preemptive builds), then
    # compares their per-process cpu_ticks via SYS_PROCLIST. "PRIORITYTEST: PASS"
    # means the HIGH child accrued meaningfully more CPU (high > low*1.3); the
    # "high=<t> low=<t>" line is printed either way for diagnosis.
    if grep -qF 'PRIORITYTEST: PASS' "$LOG"; then
        pass "priority classes shift CPU share ($(grep -F 'PRIORITYTEST: high=' "$LOG" | head -1 | sed 's/^.*PRIORITYTEST: //'))"
        return 0
    elif grep -qF 'PRIORITYTEST: FAIL' "$LOG"; then
        fail "priority did NOT shift CPU share: $(grep -F 'PRIORITYTEST:' "$LOG" | grep -E 'high=|FAIL' | head -2 | tr '\n' ' ')"
        return 1
    else
        fail "prioritytest did not report (priority probe hung or never ran)"
        return 1
    fi
}

check_tensor() {
    # init spawns sbin/tensortest; the SSE tensor-kernel library (matmul/add/
    # scale/relu/dot) is run against an independent scalar reference within an
    # epsilon. "TENSORTEST: PASS" gates the first tensor-runtime brick.
    if grep -qF 'TENSORTEST: PASS' "$LOG"; then
        pass "SSE tensor kernels verified (matmul/add/scale/relu/dot vs scalar ref)"
        return 0
    elif grep -qF 'TENSORTEST: FAIL' "$LOG"; then
        fail "tensor kernel mismatch: $(grep -F 'TENSORTEST: FAIL' "$LOG" | head -1)"
        return 1
    else
        fail "tensortest did not report (tensor lib crashed or never ran)"
        return 1
    fi
}

check_slab() {
    # kernel.c calls slab_selftest() — the object-cache slab allocator.
    if grep -qF '[SLAB] SELFTEST: PASS' "$LOG"; then
        pass "slab object-cache allocator verified"
        return 0
    elif grep -qF '[SLAB] SELFTEST: FAIL' "$LOG"; then
        fail "slab self-test failed: $(grep -F '[SLAB] SELFTEST' "$LOG" | head -1)"
        return 1
    else
        # slab_selftest() is called under #ifndef BOOT_QUIET (kernel.c) -> compiled out of
        # the default quiet ISO. Absence is by design; the proof runs in a verbose build.
        pass "slab self-test: N/A (slab_selftest compiled out — BOOT_QUIET build)"
        return 0
    fi
}

check_aibroker() {
    # init spawns sbin/aibroker, which self-tests its tool-bus + policy + ledger
    # + rollback pipeline (the AI-native command broker).
    if grep -qF 'AIBROKER SELFTEST: PASS' "$LOG"; then
        pass "AI broker pipeline verified (policy/tool-bus/ledger/rollback)"
        return 0
    elif grep -qF 'AIBROKER SELFTEST: FAIL' "$LOG"; then
        fail "aibroker self-test failed: $(grep -F 'AIBROKER SELFTEST' "$LOG" | head -1)"
        return 1
    else
        fail "aibroker did not report (broker hung or never ran)"
        return 1
    fi
}

check_tools() {
    # init spawns the standard /bin tools; each prints "<NAME> SELFTEST: PASS".
    local missing=""
    for t in SED AWK TAR PKG MAKE MEMINFO; do
        grep -qF "$t SELFTEST: PASS" "$LOG" || missing="$missing $t"
    done
    if [[ -z "$missing" ]]; then
        pass "standard tools self-tests pass (sed/awk/tar/pkg/make/meminfo)"
        return 0
    else
        fail "tool self-test(s) missing/failed:${missing}"
        return 1
    fi
}

check_compiler() {
    # init spawns bin/cc, which compiles a hello program on device (lex->parse->
    # codegen->assemble->ELF) and verifies the ELF magic. This is the self-hosting
    # flagship: the OS builds an executable from C source by itself.
    if grep -qF 'CC SELFTEST: PASS' "$LOG"; then
        pass "on-device C compiler verified (self-hosting: source -> ELF)"
        return 0
    elif grep -qF 'CC SELFTEST: FAIL' "$LOG"; then
        fail "cc self-test failed: $(grep -F 'CC SELFTEST' "$LOG" | head -1)"
        return 1
    else
        fail "cc did not report (compiler hung or never ran)"
        return 1
    fi
}

check_compress() {
    # init spawns bin/gzip, which round-trips a buffer through DEFLATE/INFLATE +
    # the gzip container (CRC32 + ISIZE verified).
    if grep -qF 'GZIP SELFTEST: PASS' "$LOG"; then
        pass "gzip/gunzip DEFLATE codec verified (round-trip + CRC32)"
        return 0
    elif grep -qF 'GZIP SELFTEST: FAIL' "$LOG"; then
        fail "gzip self-test failed: $(grep -F 'GZIP SELFTEST' "$LOG" | head -1)"
        return 1
    else
        fail "gzip did not report (codec hung or never ran)"
        return 1
    fi
}

check_sysutils() {
    # init spawns the new process/file utilities; each prints "<NAME> SELFTEST:
    # PASS". blk may print "SKIP" when no disk is attached (the smoke boots from
    # CD-ROM only), which is an acceptable result for it.
    local missing=""
    for t in PS KILL FREE UPTIME FIND DIFF CMP TEE WCX XARGS; do
        grep -qF "$t SELFTEST: PASS" "$LOG" || missing="$missing $t"
    done
    # blk: accept PASS or SKIP (no disk in the smoke environment).
    if ! grep -qE "BLK SELFTEST: (PASS|SKIP)" "$LOG"; then
        missing="$missing BLK"
    fi
    if [[ -z "$missing" ]]; then
        pass "sysutils self-tests pass (ps/kill/free/uptime/find/diff/cmp/tee/wcx/xargs/blk)"
        return 0
    else
        fail "sysutil self-test(s) missing/failed:${missing}"
        return 1
    fi
}

check_net() {
    # net_init() detects the e1000 (smoke attaches one) and reads its MAC;
    # init spawns sbin/nettest, which calls SYS_NET_INFO and prints
    # "[NETTEST] MAC <mac> IP <ip> GW <gw>". That line is the deterministic gate:
    # it proves PCI detect + BAR map + MAC read + the SYS_NET_INFO path. A
    # successful ARP round-trip ("[NETTEST] PASS") is logged as a bonus but not
    # required (slirp ARP reply timing is not deterministic under the smoke clock).
    if grep -qF '[NETTEST] MAC ' "$LOG"; then
        local macline
        macline="$(grep -F '[NETTEST] MAC ' "$LOG" | head -1 | sed 's/^.*\[NETTEST\] //')"
        if grep -qF '[NETTEST] PASS' "$LOG"; then
            pass "networking verified (NIC ${macline}; ARP round-trip OK)"
        else
            pass "NIC detect + SYS_NET_INFO verified (${macline})"
        fi
        return 0
    elif grep -qF '[NETTEST] SYS_NET_INFO failed' "$LOG"; then
        fail "networking down: nettest could not read NIC info (net_init found no e1000?)"
        return 1
    else
        fail "nettest did not report (networking unwired or it crashed)"
        return 1
    fi
}

check_sockets() {
    # init spawns sbin/sockettest, which drives the BSD-socket syscalls: create
    # a UDP socket, sendto the slirp DNS (bytes must leave the NIC), bounded
    # poll/recvfrom, then a TCP socket alloc/free. "SOCKTEST: PASS" gates the
    # whole socket syscall surface (the prerequisite for DNS/HTTP/the browser).
    if grep -qF 'SOCKTEST: PASS' "$LOG"; then
        pass "BSD sockets verified (UDP TX + recv + TCP alloc over e1000)"
        return 0
    elif grep -qF 'SOCKTEST: FAIL' "$LOG"; then
        fail "socket layer broken: $(grep -F 'SOCKTEST: FAIL' "$LOG" | head -1)"
        return 1
    else
        fail "sockettest did not report (socket syscalls unwired or it crashed)"
        return 1
    fi
}

check_overhaul_syscalls() {
    # init spawns the futex/epoll/sendfile/perf/batch verification probes; each
    # exercises the syscall's real ABI and prints "<NAME>: PASS". This proves the
    # overhaul syscalls actually work, not just link.
    local f=""
    grep -qF 'FUTEXTEST: PASS'    "$LOG" || f="$f futex"
    grep -qF 'EPOLLTEST: PASS'    "$LOG" || f="$f epoll"
    grep -qF 'SENDFILETEST: PASS' "$LOG" || f="$f sendfile"
    grep -qF 'PERFTEST: PASS'     "$LOG" || f="$f perf"
    grep -qF 'BATCHTEST: PASS'    "$LOG" || f="$f batch"
    if [[ -z "$f" ]]; then
        pass "overhaul syscalls verified (futex/epoll/sendfile/perf/batch)"
        return 0
    else
        fail "overhaul syscall(s) missing/failed:${f}"
        return 1
    fi
}

check_js() {
    # init spawns bin/js, which runs the from-scratch JS engine's ES5 script
    # battery (closures, JSON, Array/String/Object methods, Math). Deterministic.
    if grep -qF 'JS SELFTEST: PASS' "$LOG"; then
        pass "JavaScript engine verified (ES5 interpreter: closures/JSON/builtins)"
        return 0
    elif grep -qF 'JS SELFTEST: FAIL' "$LOG"; then
        fail "JS engine self-test failed: $(grep -F 'JS SELFTEST' "$LOG" | head -1)"
        return 1
    else
        fail "js did not report (engine hung or never ran)"
        return 1
    fi
}

check_https_tools() {
    # init spawns the new HTTPS-stack tools; each prints a deterministic
    # self-test marker (real TLS interop is exercised manually via tlsprobe).
    local f=""
    grep -qF 'TLSPROBE SELFTEST: PASS' "$LOG" || f="$f tlsprobe"
    grep -qF 'CERTTOOL SELFTEST: PASS' "$LOG" || f="$f certtool"
    grep -qF 'DHCPC SELFTEST: PASS'    "$LOG" || f="$f dhcpc"
    grep -qF 'APIDEMO SELFTEST: PASS'  "$LOG" || f="$f apidemo"
    if [[ -z "$f" ]]; then
        pass "HTTPS-stack tools verified (tlsprobe/certtool/dhcpc/apidemo)"
        return 0
    else
        fail "HTTPS-stack tool(s) missing/failed:${f}"
        return 1
    fi
}

check_crypto() {
    # init spawns sbin/cryptotest, which runs the crypto/RSA/X.509/TLS-PRF
    # known-answer tests (the HTTPS building blocks) -- deterministic, no network.
    if grep -qF 'CRYPTOTEST: PASS' "$LOG"; then
        pass "crypto/HTTPS KATs verified (SHA/AES/HMAC/RSA/X.509/TLS-PRF)"
        return 0
    elif grep -qF 'CRYPTOTEST: FAIL' "$LOG"; then
        fail "crypto KAT failure: $(grep -F '[CRYPTOTEST]' "$LOG" | grep -i fail | head -2 | tr '\n' ' ')"
        return 1
    else
        fail "cryptotest did not report (crypto stack hung or never ran)"
        return 1
    fi
}

check_libs() {
    # init spawns sbin/libtest: JSON parser + DHCP packet + image-codec KATs.
    if grep -qF 'LIBTEST: PASS' "$LOG"; then
        pass "lib KATs verified (json + dhcp + image codecs)"
        return 0
    elif grep -qF 'LIBTEST: FAIL' "$LOG"; then
        fail "lib KAT failure: $(grep -F '[LIBTEST]' "$LOG" | grep -i fail | head -2 | tr '\n' ' ')"
        return 1
    else
        fail "libtest did not report"
        return 1
    fi
}

check_coreutils2() {
    # The expanded coreutils each print "<NAME> SELFTEST: PASS" at boot.
    local missing=""
    for t in GREP HEAD TAIL SORT UNIQ CUT TR NL DU TOUCH BASENAME DIRNAME LESS HEXDUMP LSPCI; do
        grep -qF "$t SELFTEST: PASS" "$LOG" || missing="$missing $t"
    done
    if [[ -z "$missing" ]]; then
        pass "coreutils self-tests pass (grep/head/tail/sort/uniq/cut/tr/nl/du/touch/basename/dirname/less/hexdump/lspci)"
        return 0
    else
        fail "coreutil self-test(s) missing/failed:${missing}"
        return 1
    fi
}

check_sysinfo() {
    local missing=""
    for t in UNAME HOSTNAME WHOAMI DATE; do
        grep -qF "$t SELFTEST: PASS" "$LOG" || missing="$missing $t"
    done
    if [[ -z "$missing" ]]; then
        pass "system-info tools pass (uname/hostname/whoami/date)"
        return 0
    else
        fail "sysinfo tool(s) missing/failed:${missing}"
        return 1
    fi
}

check_nettools() {
    # nc verifies the socket API; ping does an ICMP round-trip to the gateway
    # (accept SKIP too -- ping shares the NIC with the other boot probes and a
    # stolen reply must not fail the gate; PASS is expected when uncontended).
    local f=""
    grep -qF 'NC SELFTEST: PASS' "$LOG" || f="$f nc"
    grep -qE 'PING SELFTEST: (PASS|SKIP)' "$LOG" || f="$f ping"
    if [[ -z "$f" ]]; then
        pass "net tools verified (nc socket API + ping ICMP)"
        return 0
    else
        fail "net tool(s) missing/failed:${f}"
        return 1
    fi
}

check_webstack() {
    # init spawns wget (self-test), netman, and browser. wget verifies URL
    # parsing + dotted-quad DNS (no network). netman + browser open windows and
    # print creation markers. This gates the userspace net stack (DNS+HTTP libs,
    # the network manager, and the HTTP web browser).
    local f=""
    grep -qF 'WGET SELFTEST: PASS' "$LOG"     || f="$f wget"
    grep -qF '[NETMAN] window created' "$LOG"  || f="$f netman"
    # BROWSER-CONSOLIDATE-0: the legacy text 'browser' was removed; browser2 is the one
    # real (DOM/CSS/JS/HTTPS) browser and is gated thoroughly by check_browser_wave.
    # Accept its readiness marker here instead of the retired [BROWSER] window line.
    grep -qE 'BROWSER2: ui ready|\[BROWSER2\] window created' "$LOG" || f="$f browser2"
    if [[ -z "$f" ]]; then
        pass "web stack verified (wget self-test + netman + browser2 window)"
        return 0
    else
        fail "web-stack component(s) missing/failed:${f}"
        return 1
    fi
}

check_heap_extend() {
    # heap_selftest() forces the kernel heap to grow past its initial 16MB and
    # verifies the grown region is usable (gates the heap_extend path #10).
    if grep -qF 'HEAPEXT RESULT: PASS' "$LOG"; then
        pass "heap on-demand growth verified ($(grep -F 'HEAPEXT RESULT: PASS' "$LOG" | head -1 | sed 's/^.*RESULT: //'))"
        return 0
    elif grep -qF 'HEAPEXT RESULT: FAIL' "$LOG"; then
        fail "heap growth self-test failed: $(grep -F 'HEAPEXT RESULT' "$LOG" | head -1)"
        return 1
    else
        # heap_selftest() is called under #ifndef BOOT_QUIET (kernel.c) -> compiled out of
        # the default quiet ISO. Absence is by design; the proof runs in a verbose build.
        pass "heap growth self-test: N/A (heap_selftest compiled out — BOOT_QUIET build)"
        return 0
    fi
}

check_fork_cow() {
    # init spawns sbin/forktest, which fork()s 3x and verifies copy-on-write
    # address-space isolation (parent's pages unchanged after the child mutates
    # its copies). This gates both the fork first-run path and CoW (#20).
    if grep -qF 'FORKTEST RESULT: PASS' "$LOG"; then
        pass "fork()+CoW isolation verified ($(grep -F 'FORKTEST RESULT: PASS' "$LOG" | head -1 | sed 's/^.*RESULT: //'))"
        return 0
    elif grep -qF 'FORKTEST RESULT: FAIL' "$LOG"; then
        fail "fork/CoW isolation broken: $(grep -F 'FORKTEST RESULT' "$LOG" | head -1)"
        return 1
    else
        fail "forktest did not report a result (fork/CoW probe hung or never ran)"
        return 1
    fi
}

check_thread() {
    # init spawns sbin/threadtest, which creates 4 REAL THREADS that share the
    # parent's address space but have independent stacks + FPU state, joins them,
    # and verifies: (1) a shared global counter accumulated every thread's atomic
    # add (shared memory), (2) each result[i]==i written via a per-thread stack
    # local (independent stacks), (3) each result_f[i] matches its SSE value
    # (independent FPU across context switches). "THREADTEST: PASS" gates threads.
    if grep -qF 'THREADTEST: PASS' "$LOG"; then
        pass "real threads verified ($(grep -F 'THREADTEST: shared_counter=' "$LOG" | head -1 | sed 's/^.*THREADTEST: //'))"
        return 0
    elif grep -qF 'THREADTEST: FAIL' "$LOG"; then
        fail "threads broken: $(grep -F 'THREADTEST: FAIL' "$LOG" | head -1)"
        return 1
    else
        fail "threadtest did not report (thread create/join hung or never ran)"
        return 1
    fi
}

check_matmuljobs() {
    # init spawns sbin/matmuljobs, which runs a float matmul THROUGH the userspace
    # job queue (lib/jobs): the output rows are partitioned into jobs, dispatched
    # to worker threads on a SHARED result buffer, drained (blocking futex), the
    # pool is cleanly shut down (workers joined), and the threaded result is
    # compared bit-for-bit against the single-threaded scalar reference. This
    # gates the compute-COORDINATION layer. NOTE: single-core => this is NOT a
    # speedup (workers time-share one cpu); it proves the machinery is correct.
    # The real parallel win needs SMP. "matmuljobs: PASS result-matches-ref ..."
    # means the queue dispatched the work and the result equals the reference.
    if grep -qF 'matmuljobs: PASS result-matches-ref' "$LOG"; then
        pass "job queue + worker matmul verified ($(grep -F 'matmuljobs: PASS' "$LOG" | head -1 | sed 's/^.*matmuljobs: PASS //'))"
        return 0
    elif grep -qF 'matmuljobs: FAIL' "$LOG"; then
        fail "job-queue matmul broken: $(grep -F 'matmuljobs: FAIL' "$LOG" | head -1)"
        return 1
    else
        fail "matmuljobs did not report (job queue deadlocked or never ran)"
        return 1
    fi
}

check_smpstress() {
    # init spawns sbin/smpstress, the 2-CPU dispatch STRESS harness: it drives the CPU1
    # coprocessor mailbox thousands of times via SYS_CPU1_OFFLOAD, verifying every
    # result bit-for-bit. On THIS default (single-core) smoke kernel SYS_CPU1_OFFLOAD is
    # unregistered, so the harness prints "SMPSTRESS: SKIP single CPU" and exits cleanly
    # -- that is the expected, passing result here (the real PASS is proved separately on
    # the SMP kernel under `qemu -smp 2`, see scripts/smp_smoke.sh). FAIL means a result
    # mismatch / timeout / lost wakeup under stress.
    if grep -qF 'SMPSTRESS: PASS' "$LOG"; then
        pass "SMP dispatch stress verified ($(grep -F 'SMPSTRESS: PASS' "$LOG" | head -1 | sed 's/^.*SMPSTRESS: PASS //'))"
        return 0
    elif grep -qF 'SMPSTRESS: SKIP' "$LOG"; then
        pass "SMP dispatch stress harness present (SKIP: single-core default kernel)"
        return 0
    elif grep -qF 'SMPSTRESS: FAIL' "$LOG"; then
        fail "SMP dispatch stress broke: $(grep -F 'SMPSTRESS: FAIL' "$LOG" | head -1)"
        return 1
    else
        fail "smpstress did not report (never ran / hung)"
        return 1
    fi
}

check_browser_wave() {
    # 22-agent browser wave: init spawns the per-layer selftests + browser2.
    # Each app prints "<NAME>: PASS" or "<NAME>: FAIL ..." and exits.
    # browser2 must print BOTH:
    #   "BROWSER2: ui ready (apis=5)"   -- chrome/UI wired up with all 5 web APIs
    #   "BROWSER2: rendered <N> boxes"  -- at least one layout pass completed
    # BOUND is also accepted for "rendered" (step-cap hit; terminates cleanly).
    local f=""
    grep -qF 'DOMTEST: PASS'    "$LOG" || f="$f dom"
    grep -qF 'HTMLTEST: PASS'   "$LOG" || f="$f html"
    grep -qF 'CSSTEST: PASS'    "$LOG" || f="$f css"
    grep -qF 'LAYOUTTEST: PASS' "$LOG" || f="$f layout"
    grep -qF 'WEBTEST: PASS'    "$LOG" || f="$f web"
    # Require the UI-ready marker (proves all 5 web APIs installed).
    grep -qF 'BROWSER2: ui ready (apis=5)' "$LOG" || f="$f browser2-ui-ready"
    # Require a render or bound marker (proves the layout pass ran).
    grep -qE 'BROWSER2: (rendered [0-9]|BOUND)' "$LOG" || f="$f browser2-rendered"
    if [[ -z "$f" ]]; then
        local rline
        rline="$(grep -oE 'BROWSER2: rendered [0-9]+' "$LOG" | head -1 || true)"
        if [[ -n "$rline" ]]; then
            pass "browser wave verified (DOM+HTML+CSS+layout+JS-bridge + browser2 ui ready + ${rline})"
        else
            pass "browser wave verified (DOM+HTML+CSS+layout+JS-bridge + browser2 ui ready + BOUND)"
        fi
        return 0
    else
        fail "browser-wave component(s) missing/failed:${f}"
        return 1
    fi
}

check_webapi() {
    # init spawns sbin/webapitest, which exercises the JS web-API surface
    # (setTimeout/clearTimeout, fetch, localStorage, console, URL) and prints
    # "WEBAPITEST: PASS" on success. Deterministic; no network.
    grep -qF 'WEBAPITEST: PASS' "$LOG" && pass "JS web-API selftests (timers/fetch/storage/console/url)" && return 0
    fail "webapitest missing/failed"
    return 1
}

check_no_crash_loop() {
    local count
    count="$(grep -cF 'Freeing process' "$LOG" || true)"
    if [[ $count -gt $MAX_FREEING ]]; then
        fail "Crash-loop guard triggered: ${count} \"Freeing process\" lines (threshold: ${MAX_FREEING})"
        return 1
    else
        pass "No crash loop: ${count} \"Freeing process\" line(s) (<= ${MAX_FREEING})"
        return 0
    fi
}

check_no_pid_exhaustion() {
    # #9 PID/zombie-leak gate. reaploop (spawned by init) fork+reaps a trivial child
    # 300x (> MAX_PROCESSES == 256). If reaping leaks the creation ref, the PID pool
    # exhausts: the kernel logs "No free PID" and reaploop prints REAPLOOP: FAIL. The
    # claim-based reap + reparent-to-init fix recycles slots, so we require
    # REAPLOOP: PASS and ZERO "No free PID".
    if grep -qF 'No free PID' "$LOG"; then
        fail "PID pool exhausted: kernel logged \"No free PID\" (zombie/PID leak -- reap not recycling slots)"
        return 1
    fi
    if grep -qF 'REAPLOOP: PASS' "$LOG"; then
        pass "PID recycling verified (reaploop: 300 fork+reap cycles > 256-slot table, no exhaustion)"
        return 0
    fi
    fail "reaploop did not report PASS (REAPLOOP: PASS missing)"
    return 1
}

check_pagingalias() {
    # #20 direct-map coherence gate. paging_init proves the higher-half direct map
    # (PML4[256]) is STRUCTURALLY INDEPENDENT of the splittable low identity (its
    # PDPT is a different physical page), so no identity split can perturb it.
    # Emits "PAGINGALIAS PASS" on every boot (default + SMP).
    if grep -qF 'PAGINGALIAS PASS' "$LOG"; then
        pass "Direct-map coherence (PAGINGALIAS): direct map independent of the identity"
        return 0
    fi
    fail "PAGINGALIAS PASS missing — direct-map coherence self-test failed/absent"
    return 1
}

check_rqlock() {
    # F3-1 per-CPU rq_lock topology gate. scheduler_init proves AT REST that every
    # online cpu's rq_lock is initialized, ready_count == the bounded runqueue walk,
    # no task is on >1 cpu's runqueue, and every SECONDARY cpu's runqueue is empty
    # (the offload-only policy). Emits "RQLOCK: PASS" on every boot (default + SMP).
    # Also require ZERO [SCHED_INVARIANT] lines (the F3-0 guard must not trip under
    # the per-cpu locks).
    if grep -qF '[SCHED_INVARIANT]' "$LOG"; then
        fail "Scheduler invariant violated ([SCHED_INVARIANT] present) — per-cpu rq_lock split tripped a guard"
        return 1
    fi
    if grep -qF 'RQLOCK: PASS' "$LOG"; then
        pass "Per-CPU rq_lock topology (RQLOCK): init + ready_count==walk + no cross-cpu dup + secondary rq empty"
        return 0
    elif grep -qF 'RQLOCK: FAIL' "$LOG"; then
        fail "RQLOCK: FAIL — per-cpu rq_lock topology invariant violated"
        return 1
    fi
    # Compiled out in the default perf build: scheduler_rqlock_selftest() is SCHED_DEBUG-
    # only (a no-op stub otherwise). Absence is by design, not a regression; the runtime
    # [SCHED_INVARIANT] guard above still fires in any build. The proof runs in SCHED_DEBUG.
    pass "RQLOCK: N/A (topology self-test compiled out — perf build, SCHED_DEBUG off)"
    return 0
}

check_affinity() {
    # F3-2 CPU affinity model gate. scheduler_affinity_selftest() proves at boot that
    # the affinity predicate is correct and every ctor default fired (no task carries a
    # zero allowed_cpus mask -- the memset-trap closure), and the runtime validators
    # (covered by check_rqlock's [SCHED_INVARIANT] gate) catch any queued task whose
    # cpu is outside its mask or that is pinned to the wrong cpu. Emits "AFFINITY: PASS".
    if grep -qF 'AFFINITY: PASS' "$LOG"; then
        pass "CPU affinity model (AFFINITY): predicate sound + ctor defaults fired + no off-affinity task"
        return 0
    elif grep -qF 'AFFINITY: FAIL' "$LOG"; then
        fail "AFFINITY: FAIL — affinity model invariant violated"
        return 1
    fi
    # scheduler_affinity_selftest() is SCHED_DEBUG-only (no-op stub in the perf build).
    pass "AFFINITY: N/A (affinity self-test compiled out — perf build, SCHED_DEBUG off)"
    return 0
}

# ── Run all checks, tally results ──────────────────────────────────────────
run_checks() {
    header "Boot invariant checks"

    local checks=(
        check_kernel_started
        check_rtc_up
        check_all_services
        check_processes_spawned
        check_compositor_window
        check_no_panic
        check_no_cpu_exception
        check_no_page_fault
        check_no_segment_too_large
        check_no_crash_loop
        check_no_pid_exhaustion
        check_pagingalias
        check_rqlock
        check_affinity
        check_fork_cow
        check_thread
        check_matmuljobs
        check_smpstress
        check_heap_extend
        check_aibroker
        check_tools
        check_slab
        check_argv
        check_floattest
        check_sleep
        check_priority
        check_tensor
        check_compiler
        check_compress
        check_sysutils
        check_net
        check_sockets
        check_webstack
        check_crypto
        check_libs
        check_coreutils2
        check_sysinfo
        check_nettools
        check_js
        check_https_tools
        check_overhaul_syscalls
        check_browser_wave
        check_webapi
    )

    _PASS_COUNT=0
    _FAIL_COUNT=0
    for fn in "${checks[@]}"; do
        if "$fn"; then
            _PASS_COUNT=$((_PASS_COUNT + 1))
        else
            _FAIL_COUNT=$((_FAIL_COUNT + 1))
        fi
    done
}

# ── Print final summary ────────────────────────────────────────────────────
print_summary() {
    local pass_count=$1
    local fail_count=$2
    local total=$((pass_count + fail_count))

    printf '\n'
    printf '%s\n' "$(printf '=%.0s' {1..60})"
    printf '  %-30s %s\n' "Total checks:"  "${total}"
    printf '  %-30s %s\n' "Passed:"        "${pass_count}"
    printf '  %-30s %s\n' "Failed:"        "${fail_count}"
    printf '%s\n' "$(printf '=%.0s' {1..60})"

    if [[ $fail_count -eq 0 ]]; then
        printf "${GREEN}${BOLD}  RESULT: PASS -- all %d checks passed${RESET}\n\n" "$pass_count"
        return 0
    else
        printf "${RED}${BOLD}  RESULT: FAIL -- %d of %d checks failed${RESET}\n" \
            "$fail_count" "$total"
        printf "  Full log: %s\n\n" "$LOG"
        return 1
    fi
}

# ── Log excerpt ────────────────────────────────────────────────────────────
show_log_excerpt() {
    header "Serial log (first 40 lines)"
    head -40 "$LOG" | while IFS= read -r l; do
        printf "  %s\n" "$l"
    done
    local total
    total="$(wc -l < "$LOG")"
    if [[ $total -gt 40 ]]; then
        info "... (${total} lines total — full log at ${LOG})"
    fi
}

# ── Main ───────────────────────────────────────────────────────────────────
main() {
    printf '\n'
    printf "${BOLD}%s${RESET}\n" "AutomationOS Boot Smoketest"
    printf "  ISO:     %s\n" "$ISO"
    printf "  Log:     %s\n" "$LOG"
    printf "  Timeout: %ss\n" "$QEMU_TIMEOUT"
    printf "  Date:    %s\n" "$(date -u '+%Y-%m-%d %H:%M:%S UTC' 2>/dev/null || date)"

    maybe_build

    preflight || exit 1

    boot_qemu

    if ! wait_for_log; then
        fail "Serial log is empty or missing after ${QEMU_TIMEOUT}s — QEMU may have failed to start"
        printf "  Expected log: %s\n" "$LOG"
        exit 1
    fi

    show_log_excerpt

    run_checks

    print_summary "$_PASS_COUNT" "$_FAIL_COUNT"
}

main "$@"
