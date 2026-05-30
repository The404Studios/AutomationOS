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
MAX_FREEING=180

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
    if grep -qF '[KERNEL]' "$LOG"; then
        pass "Kernel started ([KERNEL] tag present)"
        return 0
    else
        fail "Kernel did NOT start — no [KERNEL] tag found in log"
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
    if grep -qF 'CPU EXCEPTION' "$LOG"; then
        local count
        count="$(grep -cF 'CPU EXCEPTION' "$LOG" || true)"
        fail "CPU EXCEPTION detected: ${count} occurrence(s)"
        grep -F 'CPU EXCEPTION' "$LOG" | head -5 | while IFS= read -r l; do
            printf "       %s\n" "$l"
        done
        return 1
    else
        pass "No CPU EXCEPTION"
        return 0
    fi
}

check_no_page_fault() {
    if grep -qiF 'page fault' "$LOG"; then
        local count
        count="$(grep -ciF 'page fault' "$LOG" || true)"
        fail "Page fault(s) detected: ${count} occurrence(s)"
        grep -iF 'page fault' "$LOG" | head -5 | while IFS= read -r l; do
            printf "       %s\n" "$l"
        done
        return 1
    else
        pass "No page fault"
        return 0
    fi
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
        fail "slab self-test did not report (slab_selftest absent/hung)"
        return 1
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
    for t in GREP HEAD TAIL SORT UNIQ CUT TR NL DU TOUCH BASENAME DIRNAME LESS HEXDUMP; do
        grep -qF "$t SELFTEST: PASS" "$LOG" || missing="$missing $t"
    done
    if [[ -z "$missing" ]]; then
        pass "coreutils self-tests pass (grep/head/tail/sort/uniq/cut/tr/nl/du/touch/basename/dirname/less/hexdump)"
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
    grep -qF '[BROWSER] window created' "$LOG" || f="$f browser"
    if [[ -z "$f" ]]; then
        pass "web stack verified (wget self-test + netman + browser windows)"
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
        fail "heap growth self-test did not report (heap_selftest hung or absent)"
        return 1
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
        check_fork_cow
        check_heap_extend
        check_aibroker
        check_tools
        check_slab
        check_argv
        check_floattest
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
