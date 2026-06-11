#!/bin/bash
# tlbshoot_smoke.sh -- the SMP-G2 TLBSHOOT-MIN proof vehicle.
# =============================================================================
# Same build as G0/G1 (SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1); G2
# adds the bounded ack-counted KERNEL-range shootdown (IPI_TLB_FLUSH_PAGE,
# vector 0x57 -- the stash-mined SMP-R0 harvest) + the TLB_INVARIANT validator
# + the loud pin/no-migration audit. Gates, in order:
#
#   1. LINK gate     -- "Link OK" grepped from the build log (never trust rc).
#   2. LINK-MAP gate -- all IPI/G1/G2 state below 0x200000 (law 15: the 0x57
#      handler reads the request block under arbitrary CR3).
#   3. TLBSHOOT gates (kernel-printed, deterministic):
#        TLBSHOOT: PASS kernel_flush=1 acked=1 bounded=1 invariant=1
#          one REAL shootdown: local invlpg + IPI to the hlt-parked CPU1,
#          CPU1's 0x57 handler invlpg'd + acked, the TSC-bounded wait
#          returned well under its 50 ms cap, 0 TLB_INVARIANT violations.
#        TLBSHOOT_NEG: PASS no_user_crossflush_needed_under_pinning=1
#          the pin-model audit: every live process has a single-CPU affinity
#          mask, so user mappings are CPU-local and local invlpg suffices.
#          ANY multi-CPU mask fails this gate -- the forcing function for
#          per-mm shootdown work before the assumption silently rots.
#   4. REGRESSION gate -- IPIWAKE pings + IPILINK + the full F3-5 ladder, plus
#      0 [TLB_INVARIANT] lines anywhere in the boot.
#
# Prereq: iso/boot/initrd.img + build/kernel.elf exist (IDE=1 build_all first).
# Run: wsl -d Arch bash -lc 'bash scripts/tlbshoot_smoke.sh'
set -u
ROOT=/mnt/c/Users/wilde/Desktop/Kernel
cd "$ROOT" || exit 9

QEMU_TIMEOUT="${QEMU_TIMEOUT:-240}"
SER=/tmp/tlbshoot_serial.log
QB=/tmp/tlbshoot_qb.log

echo "[tlbshoot-smoke] building kernel-smp.elf (SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1) ..."
SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 bash scripts/quick_build.sh > "$QB" 2>&1 \
    || { echo "[tlbshoot-smoke] build FAILED"; tail -25 "$QB"; exit 1; }

# --- gate 1: the LINK gate ---------------------------------------------------
if ! grep -qF 'Link OK -- no unresolved symbols' "$QB"; then
    echo "[tlbshoot-smoke] LINK gate FAILED:"; grep -A8 -F 'LINK FAILED' "$QB" | head -20
    exit 1
fi
if grep -qE '^FAIL: ' "$QB"; then
    echo "[tlbshoot-smoke] COMPILE gate FAILED:"; grep -B1 -A6 -E '^FAIL: ' "$QB" | head -30
    exit 1
fi
echo "[tlbshoot-smoke] LINK gate OK"

# --- gate 2: the LINK-MAP gate -----------------------------------------------
MAP_OK=1
SYMS=$(nm build/kernel-smp.elf | grep -E ' [bBdD] (ipi_stats|ipi_need_resched|call_queue|call_queue_head|call_queue_tail|call_queue_lock|ipi_bsp_apic_id|ipi_ready|g_g1_ping_req|g_g1_ping_ack|g_g1_enq_tsc|g_g1_enq_pid|g_g1_dispatch_tsc|tlbshoot_addr|tlbshoot_npages|tlbshoot_ack|tlbshoot_lock|g_tlb_invariant_violations|tlbshoot_testpage)$')
if [ -z "$SYMS" ]; then
    echo "[tlbshoot-smoke] LINK-MAP gate FAILED: no IPI/G2 symbols found"; MAP_OK=0
else
    while read -r addr type name; do
        dec=$((16#$addr))
        if [ "$dec" -ge $((0x200000)) ]; then
            echo "[tlbshoot-smoke] SHADOW HAZARD: $name at 0x$addr (>= 0x200000)"; MAP_OK=0
        else
            echo "[tlbshoot-smoke]   map ok: $name @ 0x$addr"
        fi
    done <<< "$SYMS"
fi
if [ "$MAP_OK" != "1" ]; then echo "[tlbshoot-smoke] LINK-MAP gate FAILED"; exit 1; fi
echo "[tlbshoot-smoke] LINK-MAP gate OK"

if [ ! -s iso/boot/initrd.img ]; then echo "[tlbshoot-smoke] initrd missing"; exit 1; fi
if [ ! -s build/kernel.elf ];   then echo "[tlbshoot-smoke] default kernel.elf missing"; exit 1; fi

echo "[tlbshoot-smoke] assembling ISO ..."
cp build/kernel-smp.elf iso/boot/kernel.elf
grub-mkrescue -o build/automationos-tlbshoot.iso iso/ 2>/dev/null \
    || { echo "[tlbshoot-smoke] grub-mkrescue FAILED"; cp build/kernel.elf iso/boot/kernel.elf; exit 1; }
cp build/kernel.elf iso/boot/kernel.elf

rm -f "$SER"
echo "[tlbshoot-smoke] booting qemu -smp 2 (timeout ${QEMU_TIMEOUT}s) ..."
timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
    -cdrom build/automationos-tlbshoot.iso \
    -m 512 -smp 2 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== gate 3: TLBSHOOT (the G2 acceptance, kernel-printed) ==="
grep -F 'TLBSHOOT' "$SER" | head -4
TS_OK=0;  grep -qE 'TLBSHOOT: PASS kernel_flush=1 acked=1 bounded=1 invariant=1' "$SER" && TS_OK=1
NEG_OK=0; grep -qF 'TLBSHOOT_NEG: PASS no_user_crossflush_needed_under_pinning=1' "$SER" && NEG_OK=1
NTLBV=$(grep -cF '[TLB_INVARIANT] VIOLATION' "$SER" || true)

echo "=== gate 4: G1 + G0 + F3-5 regression ladder ==="
WAKE_OK=0
grep -qE 'ping summary acks=32/32 max_latency_us=[0-9]+' "$SER" && WAKE_OK=1
ENQ_OK=0
grep -qE 'enqueue->dispatch latency=[0-9]+ us' "$SER" && ENQ_OK=1
IPI_OK=0; grep -qF 'IPILINK: PASS ipi_resched=1 cpu1_count=1' "$SER" && IPI_OK=1
F2_OK=0;  grep -qE 'Brick F2 VERIFY:.*delta=[1-9][0-9]*' "$SER" && F2_OK=1
AC_OK=0;  grep -qF 'APCURRENT: PASS' "$SER" && AC_OK=1
MARKS=$(grep -cF 'CPU1HELLO mark' "$SER" || true); MARKS=${MARKS:-0}
HPID=$(grep -oE 'cpu1hello PID [0-9]+' "$SER" | head -1 | grep -oE 'PID [0-9]+' | grep -oE '[0-9]+')
EXIT42=0
grep -qE "sys_exit: Process 'cpu1hello' \(PID [0-9]+\) exiting with status 42" "$SER" && EXIT42=1
REAPED=0
if [ -n "${HPID:-}" ]; then
    grep -qE "\[INIT\] Process ${HPID} exited" "$SER" && REAPED=1
fi
ALIVE=1
grep -qF 'entering frame loop' "$SER" || ALIVE=0
grep -qF '[INIT] Compositor died' "$SER" && ALIVE=0
NSCHED=$(grep -cF '[SCHED_INVARIANT]' "$SER" || true)
NPANIC=$(grep -cE 'PANIC|CPU EXCEPTION|TRIPLE FAULT|KERNEL PANIC' "$SER" || true)
[ "$NPANIC" = "0" ] || ALIVE=0
echo "  wake=$WAKE_OK enq=$ENQ_OK ipilink=$IPI_OK F2=$F2_OK APCURRENT=$AC_OK marks=$MARKS exit42=$EXIT42 reaped=$REAPED alive=$ALIVE sched_viol=$NSCHED tlb_viol=$NTLBV panic=$NPANIC"

if [ "$TS_OK" = "1" ] && [ "$NEG_OK" = "1" ] && [ "$NTLBV" = "0" ] && \
   [ "$WAKE_OK" = "1" ] && [ "$ENQ_OK" = "1" ] && [ "$IPI_OK" = "1" ] && \
   [ "$F2_OK" = "1" ] && [ "$AC_OK" = "1" ] && [ "$MARKS" -ge 1 ] && \
   [ "$EXIT42" = "1" ] && [ "$REAPED" = "1" ] && [ "$ALIVE" = "1" ] && \
   [ "$NSCHED" = "0" ] && [ "$NPANIC" = "0" ]; then
    echo "TLBSHOOT-SMOKE: PASS tlbshoot=1 neg=1 tlb_violations=0 regression_green=1"
    echo "[tlbshoot-smoke] RESULT: PASS -- kernel-range shootdown proven, pin model audited"
    exit 0
else
    echo "TLBSHOOT-SMOKE: FAIL tlbshoot=$TS_OK neg=$NEG_OK tlb_viol=$NTLBV wake=$WAKE_OK enq=$ENQ_OK ipilink=$IPI_OK f35=(f2=$F2_OK ac=$AC_OK marks=$MARKS exit42=$EXIT42 reaped=$REAPED) alive=$ALIVE sched=$NSCHED panic=$NPANIC"
    echo "--- last 40 serial lines ---"; tail -40 "$SER"
    exit 1
fi
