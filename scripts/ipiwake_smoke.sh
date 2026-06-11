#!/bin/bash
# ipiwake_smoke.sh -- the SMP-G1 IPI-WAKE proof vehicle.
# =============================================================================
# Same build as G0 (SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1); G1 adds
# the wake path: enqueue-to-CPU1 sends IPI_RESCHEDULE, the handler sets per-CPU
# need_resched, and the AP idle loop closes the lost-wakeup race
# (cli -> check rq/need_resched -> sti;hlt). Gates, in order:
#
#   1. LINK gate     -- "Link OK" grepped from the build log (never trust rc).
#   2. LINK-MAP gate -- every IPI/G1 global sits below 0x200000 (law 15:
#      handler/idle-loop data is touched under arbitrary CR3 -- the IPI handler
#      can interrupt ring-3 cpu1hello, and the AP dying path runs
#      ap_cooperative_schedule under the dead task's user CR3).
#   3. IPIWAKE gates (serial):
#        a. "[SMP] G1: ping summary acks=32/32 max_latency_us=<n>" -- 32 IPI
#           pings each woke the hlt-parked idle loop (ticks CANNOT ack: only
#           the IPI handler sets need_resched). acks=32/32 = no_lost_wake;
#           max < 1000 us = IPI wake, not the 10 ms tick rescue.
#        b. "[SMP] G1: enqueue->dispatch latency=<n> us" -- the REAL cpu1hello
#           enqueue rode the IPI kick and CPU1 dispatched it in < 1000 us
#           (enqueue_to_cpu1 + first_instruction_lt_1ms).
#      The composite acceptance line is assembled here (the F3-5 convention).
#   4. REGRESSION gate -- IPILINK still PASS + the whole F3-5 ladder green.
#
# Prereq: iso/boot/initrd.img + build/kernel.elf exist (IDE=1 build_all first).
# Run: wsl -d Arch bash -lc 'bash scripts/ipiwake_smoke.sh'
set -u
ROOT=/mnt/c/Users/wilde/Desktop/Kernel
cd "$ROOT" || exit 9

QEMU_TIMEOUT="${QEMU_TIMEOUT:-240}"
SER=/tmp/ipiwake_serial.log
QB=/tmp/ipiwake_qb.log

echo "[ipiwake-smoke] building kernel-smp.elf (SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1) ..."
SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 bash scripts/quick_build.sh > "$QB" 2>&1 \
    || { echo "[ipiwake-smoke] build FAILED"; tail -25 "$QB"; exit 1; }

# --- gate 1: the LINK gate ---------------------------------------------------
if ! grep -qF 'Link OK -- no unresolved symbols' "$QB"; then
    echo "[ipiwake-smoke] LINK gate FAILED (no 'Link OK' in build log):"
    grep -A8 -F 'LINK FAILED' "$QB" | head -20
    exit 1
fi
if grep -qE '^FAIL: ' "$QB"; then
    echo "[ipiwake-smoke] COMPILE gate FAILED:"
    grep -B1 -A6 -E '^FAIL: ' "$QB" | head -30
    exit 1
fi
echo "[ipiwake-smoke] LINK gate OK"

# --- gate 2: the LINK-MAP gate (IPI + G1 globals below 0x200000) -------------
MAP_OK=1
SYMS=$(nm build/kernel-smp.elf | grep -E ' [bBdD] (ipi_stats|ipi_need_resched|call_queue|call_queue_head|call_queue_tail|call_queue_lock|ipi_bsp_apic_id|ipi_ready|g_g1_ping_req|g_g1_ping_ack|g_g1_enq_tsc|g_g1_enq_pid|g_g1_dispatch_tsc)$')
if [ -z "$SYMS" ]; then
    echo "[ipiwake-smoke] LINK-MAP gate FAILED: no IPI/G1 symbols found"
    MAP_OK=0
else
    while read -r addr type name; do
        dec=$((16#$addr))
        if [ "$dec" -ge $((0x200000)) ]; then
            echo "[ipiwake-smoke] SHADOW HAZARD: $name at 0x$addr (>= 0x200000)"
            MAP_OK=0
        else
            echo "[ipiwake-smoke]   map ok: $name @ 0x$addr"
        fi
    done <<< "$SYMS"
fi
if [ "$MAP_OK" != "1" ]; then
    echo "[ipiwake-smoke] LINK-MAP gate FAILED"
    exit 1
fi
echo "[ipiwake-smoke] LINK-MAP gate OK"

if [ ! -s iso/boot/initrd.img ]; then
    echo "[ipiwake-smoke] iso/boot/initrd.img missing -- run build_all first"; exit 1
fi
if [ ! -s build/kernel.elf ]; then
    echo "[ipiwake-smoke] build/kernel.elf missing (need it to restore the iso tree)"; exit 1
fi

echo "[ipiwake-smoke] assembling ISO (kernel-smp.elf + existing initrd) ..."
cp build/kernel-smp.elf iso/boot/kernel.elf
grub-mkrescue -o build/automationos-ipiwake.iso iso/ 2>/dev/null \
    || { echo "[ipiwake-smoke] grub-mkrescue FAILED"; cp build/kernel.elf iso/boot/kernel.elf; exit 1; }
cp build/kernel.elf iso/boot/kernel.elf

rm -f "$SER"
echo "[ipiwake-smoke] booting qemu -smp 2 (timeout ${QEMU_TIMEOUT}s) ..."
timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
    -cdrom build/automationos-ipiwake.iso \
    -m 512 -smp 2 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== gate 3a: no-lost-wake ping ladder ==="
grep -F 'G1: ping summary' "$SER" | tail -1
WAKE_OK=0; WAKE_US=999999
PINGLINE=$(grep -oE 'ping summary acks=32/32 max_latency_us=[0-9]+' "$SER" | tail -1)
if [ -n "$PINGLINE" ]; then
    WAKE_US=$(echo "$PINGLINE" | grep -oE '[0-9]+$')
    [ "$WAKE_US" -lt 1000 ] && WAKE_OK=1
fi

echo "=== gate 3b: enqueue->dispatch latency (the real cpu1hello enqueue) ==="
grep -F 'G1: enqueue->dispatch' "$SER" | tail -1
ENQ_OK=0; ENQ_US=999999
ENQLINE=$(grep -oE 'enqueue->dispatch latency=[0-9]+ us' "$SER" | tail -1)
if [ -n "$ENQLINE" ]; then
    ENQ_US=$(echo "$ENQLINE" | grep -oE '[0-9]+' | head -1)
    [ "$ENQ_US" -lt 1000 ] && ENQ_OK=1
fi

echo "=== gate 4: G0 + F3-5 regression ladder ==="
IPI_OK=0; grep -qF 'IPILINK: PASS ipi_resched=1 cpu1_count=1' "$SER" && IPI_OK=1
F2_OK=0
grep -qE 'Brick F2 VERIFY:.*delta=[1-9][0-9]*' "$SER" && F2_OK=1
AC_OK=0; grep -qF 'APCURRENT: PASS' "$SER" && AC_OK=1
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
echo "  ipilink=$IPI_OK F2=$F2_OK APCURRENT=$AC_OK marks=$MARKS exit42=$EXIT42 reaped=$REAPED alive=$ALIVE sched_viol=$NSCHED panic=$NPANIC"

if [ "$WAKE_OK" = "1" ] && [ "$ENQ_OK" = "1" ] && [ "$IPI_OK" = "1" ] && \
   [ "$F2_OK" = "1" ] && [ "$AC_OK" = "1" ] && [ "$MARKS" -ge 1 ] && \
   [ "$EXIT42" = "1" ] && [ "$REAPED" = "1" ] && [ "$ALIVE" = "1" ] && \
   [ "$NSCHED" = "0" ] && [ "$NPANIC" = "0" ]; then
    echo "IPIWAKE: PASS enqueue_to_cpu1=1 first_instruction_lt_1ms=1 no_lost_wake=1"
    echo "[ipiwake-smoke] RESULT: PASS -- wake_max=${WAKE_US}us enqueue_dispatch=${ENQ_US}us (tick floor was 10000us)"
    exit 0
else
    echo "IPIWAKE: FAIL no_lost_wake=$WAKE_OK(max=${WAKE_US}us) first_dispatch=$ENQ_OK(${ENQ_US}us) ipilink=$IPI_OK f35=(f2=$F2_OK ac=$AC_OK marks=$MARKS exit42=$EXIT42 reaped=$REAPED) alive=$ALIVE sched=$NSCHED panic=$NPANIC"
    echo "--- last 40 serial lines ---"; tail -40 "$SER"
    exit 1
fi
