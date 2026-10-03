#!/bin/bash
# vm_verify.sh -- boot AutomationOS in QEMU and verify the three things that matter:
#
#   1. it BOOTS and the desktop is alive          (single core, T410 profile)
#   2. BOTH CORES work                            (SMP full-stack kernel, -smp 2 -cpu Westmere)
#   3. the INTERNET works                         (real DNS + HTTP + certificate-verified HTTPS,
#                                                  inside the guest, through QEMU's NAT to the host)
#
#   wsl -d Arch bash /mnt/c/Users/wilde/Desktop/Kernel/build_test/vm_verify.sh
#   NOBUILD=1 ... vm_verify.sh      # reuse the images built by a previous run
#
# Both boots use the T410 kernel profile (T410_SAFE=1 SCHED_DEBUG=0) and the FULL userspace (the
# self-test storm) plus sbin/livenet (NET_LIVE=1) so the guest itself reports Internet results.
# Screenshots land in screenshots/vm_<phase>.png. Exit 0 iff every check passes.
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
mkdir -p screenshots build
RESULTS=/tmp/vm_verify_results.txt; : > "$RESULTS"
FAILS=0
ok()   { printf '  [PASS] %s\n' "$*"; echo "PASS $*" >> "$RESULTS"; }
bad()  { printf '  [FAIL] %s\n' "$*"; echo "FAIL $*" >> "$RESULTS"; FAILS=$((FAILS+1)); }
chk()  { # chk <log> <label> <literal>
    if grep -aqF -- "$3" "$1"; then ok "$2"; else bad "$2   (missing: $3)"; fi; }
kernel_fault() { local e c; e=$(grep -acF 'CPU EXCEPTION' "$1" || true); c=$(grep -acF 'Terminating faulting process' "$1" || true)
    grep -aqiE 'PANIC|TRIPLE FAULT' "$1" || [ "${e:-0}" -gt "${c:-0}" ]; }

mkiso() { # mkiso <kernel.elf> <out.iso>
    cp "$1" iso/boot/kernel.elf && grub-mkrescue -o "$2" iso/ > /tmp/vmv_iso.log 2>&1 \
        || { echo "grub-mkrescue failed for $2"; tail -5 /tmp/vmv_iso.log; exit 1; }
}

# boot <iso> <label> <smp> <cpu> <max_secs> <done_regex>
#   Boots, polls the serial log until <done_regex> appears (or max_secs), settles, screendumps, kills.
boot() {
    local iso=$1 label=$2 smp=$3 cpu=$4 max=$5 done_re=$6
    local ser=/tmp/vmv_${label}.log sock=/tmp/vmv_${label}.qmp
    rm -f "$ser" "$sock"
    qemu-system-x86_64 -cdrom "$iso" -m 1024 -smp "$smp" -cpu "$cpu" \
        -netdev user,id=n0 -device e1000,netdev=n0 \
        -display none -serial "file:$ser" -qmp "unix:$sock,server,nowait" -no-reboot >/dev/null 2>&1 &
    local qp=$!
    local t=0
    while [ $t -lt "$max" ]; do
        grep -aqE -- "$done_re" "$ser" 2>/dev/null && break
        kill -0 $qp 2>/dev/null || break
        sleep 2; t=$((t+2))
    done
    sleep 10                                  # let the desktop settle before the screenshot
    python3 - "$sock" "/tmp/vmv_${label}.png" <<'PY'
import socket, json, sys
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(sys.argv[1]); f = s.makefile('rw'); f.readline()
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            l = f.readline()
            if not l or '"return"' in l or '"error"' in l: return
    cmd({"execute": "qmp_capabilities"})
    cmd({"execute": "screendump", "arguments": {"filename": sys.argv[2], "format": "png"}})
except Exception as e:
    print("screendump failed:", e)
PY
    sleep 1; kill $qp 2>/dev/null; wait $qp 2>/dev/null
    [ -f "/tmp/vmv_${label}.png" ] && cp -f "/tmp/vmv_${label}.png" "screenshots/vm_${label}.png" \
        && echo "  screenshot: screenshots/vm_${label}.png"
}

if [ "${NOBUILD:-0}" != "1" ]; then
    echo "=== [build 1/3] userspace: FULL + NET_LIVE + DNS_TEST (lean profile off, self-test storm on) ==="
    T410_SAFE=1 SCHED_DEBUG=0 bash scripts/quick_build.sh > /tmp/vmv_k0.log 2>&1
    grep -qE '^FAIL|error:|undefined reference' /tmp/vmv_k0.log && { grep -E '^FAIL|error:|undefined reference' /tmp/vmv_k0.log | head -5; exit 1; }
    cp build/kernel.elf build/kernel-t410-default.elf
    FULL=1 NET_LIVE=1 DNS_TEST=1 bash scripts/build_all.sh > /tmp/vmv_all.log 2>&1
    grep -qE 'error:|undefined reference' /tmp/vmv_all.log && { grep -E 'error:|undefined reference' /tmp/vmv_all.log | head -5; exit 1; }
    tail -1 /tmp/vmv_all.log
    echo "=== [build 2/3] SMP full-stack kernel (SMP SCHED DISPATCH IPI BKL BATCH RUNMASK DSPLIT) ==="
    T410_SAFE=1 SCHED_DEBUG=0 SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 SMP_BKL=1 SMP_BATCH=1 SMP_RUNMASK=1 SMP_DSPLIT=1 \
        bash scripts/quick_build.sh > /tmp/vmv_k1.log 2>&1
    grep -qF 'Link OK -- no unresolved symbols' /tmp/vmv_k1.log || { echo "SMP kernel LINK FAILED"; grep -E '^FAIL|error:|undefined' /tmp/vmv_k1.log | head; exit 1; }
    cp build/kernel-smp.elf build/kernel-t410-smp.elf
    echo "=== [build 3/3] ISOs ==="
    mkiso build/kernel-t410-default.elf build/vm-single.iso
    mkiso build/kernel-t410-smp.elf     build/vm-smp.iso
    cp build/kernel-t410-default.elf iso/boot/kernel.elf        # leave the iso tree on the default kernel
    ls -la build/vm-single.iso build/vm-smp.iso | awk '{print "  " $5, $9}'
fi

echo
echo "################ BOOT 1: single core, T410 profile ################"
boot build/vm-single.iso single 1 Westmere 240 'LIVENET: (PASS|FAIL)'
L=/tmp/vmv_single.log
chk "$L" "boots to a live desktop (compositor heartbeat healthy)"       'service=compositor event=healthy'
chk "$L" "kernel direct-map paging fix active (PAGINGALIAS)"            'PAGINGALIAS PASS'
chk "$L" "firewall self-test"                                           'FW-SELFTEST: PASS'
chk "$L" "kernel-enforced privilege drop (FWTEST)"                      'FWTEST: PASS'
chk "$L" "agent tool rail + hash-chained ledger"                        'LEDGER: VERIFIED'
kernel_fault "$L" && bad "no kernel fault" || ok "no kernel fault (contained ring-3 faults: $(grep -acF 'Terminating faulting process' "$L"))"
grep -aqF 'out of physical memory' "$L" && bad "no physical-memory exhaustion" || ok "no physical-memory exhaustion"
echo "  --- internet (inside the guest, through QEMU's NAT):"
grep -a 'LIVENET:' "$L" | sed 's/^/     /'
chk "$L" "INTERNET: real DNS + HTTP + certificate-VERIFIED HTTPS"       'LIVENET: PASS'

echo
echo "################ BOOT 2: TWO cores (-smp 2 -cpu Westmere), full SMP stack ################"
boot build/vm-smp.iso smp 2 Westmere 300 'LIVENET: (PASS|FAIL)'
L=/tmp/vmv_smp.log
echo "  --- SMP evidence:"
grep -aE '\[SMP\]|AP (started|online)|CPU1|cpu1|IPILINK|TLBSHOOT|\[DSPLIT\]' "$L" | cut -c1-130 | head -14 | sed 's/^/     /'
chk "$L" "second CPU came online"                                       'CPU1'
chk "$L" "CPU1 ran jobs correctly under stress (SMPSTRESS)"             'SMPSTRESS: PASS'
chk "$L" "ring-3 process ran AND exited on CPU1 (CPU1HELLO)"            'CPU1HELLO: PASS'
chk "$L" "per-CPU runqueue locks (RQLOCK)"                              'RQLOCK: PASS'
chk "$L" "CPU affinity masks (AFFINITY)"                                'AFFINITY: PASS'
chk "$L" "paging/direct-map coherence on SMP"                           'PAGINGALIAS PASS'
chk "$L" "kernel-enforced privilege drop on SMP (FWTEST)"               'FWTEST: PASS'
chk "$L" "desktop alive on SMP (compositor heartbeat healthy)"          'service=compositor event=healthy'
grep -aqE 'SCHED_INVARIANT|TLB_INVARIANT\] VIOLATION' "$L" && bad "no scheduler/TLB invariant violations" || ok "no scheduler/TLB invariant violations"
kernel_fault "$L" && bad "no kernel fault on SMP" || ok "no kernel fault on SMP"
echo "  --- internet on the SMP kernel:"
grep -a 'LIVENET:' "$L" | sed 's/^/     /'
chk "$L" "INTERNET on 2 cores: real DNS + HTTP + verified HTTPS"        'LIVENET: PASS'

echo
echo "################ SUMMARY ################"
P=$(grep -c '^PASS' "$RESULTS"); F=$(grep -c '^FAIL' "$RESULTS")
echo "  passed=$P failed=$F"
grep '^FAIL' "$RESULTS" | sed 's/^/  /'
[ "$FAILS" -eq 0 ] && echo "VM-VERIFY: PASS" || echo "VM-VERIFY: FAIL"
exit $([ "$FAILS" -eq 0 ] && echo 0 || echo 1)
