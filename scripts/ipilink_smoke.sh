#!/bin/bash
# ipilink_smoke.sh -- the SMP-G0 IPI-LINK proof vehicle.
# =============================================================================
# Builds the kernel with SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 --
# the F3-5 dispatch build PLUS the (previously dormant) IPI subsystem linked,
# initialized, and exercised once. Gates, in order:
#
#   1. LINK gate -- "Link OK" grepped from the build log. quick_build.sh exits
#      0 on a failed link (the SMP-R0 lesson: every SMP build was link-broken
#      for days because rc was trusted); the LOG is the authority.
#   2. LINK-MAP gate -- every ipi.c global (ipi_stats, call_queue*) sits BELOW
#      0x200000. linker.ld's contract: user ELFs link at 0x200000 and shadow
#      any kernel global above it under their CR3; the IPI queues/stats are
#      touched from IPI handlers that run under ARBITRARY CR3, so a >= 0x200000
#      placement is a real corruption hazard, not a style issue.
#   3. IPILINK gate -- serial "IPILINK: PASS ipi_resched=1 cpu1_count=1"
#      (BSP sent ONE IPI_RESCHEDULE; CPU1 handled + counted it).
#   4. REGRESSION gate -- the frozen F3-5 ladder stays green under the new
#      build: F2 delta>0, APCURRENT PASS, CPU1HELLO exit/reap, 0 invariant
#      violations, desktop alive, 0 panic.
#
# Prereq: iso/boot/initrd.img + build/kernel.elf exist (IDE=1 build_all first).
# Run: wsl -d Arch bash -lc 'bash scripts/ipilink_smoke.sh'
set -u
ROOT=/mnt/c/Users/wilde/Desktop/Kernel
cd "$ROOT" || exit 9

QEMU_TIMEOUT="${QEMU_TIMEOUT:-240}"
SER=/tmp/ipilink_serial.log
QB=/tmp/ipilink_qb.log

echo "[ipilink-smoke] building kernel-smp.elf (SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1) ..."
SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 bash scripts/quick_build.sh > "$QB" 2>&1 \
    || { echo "[ipilink-smoke] build FAILED"; tail -25 "$QB"; exit 1; }

# --- gate 1: the LINK gate (grep the log; NEVER trust quick_build's rc) ----
if ! grep -qF 'Link OK -- no unresolved symbols' "$QB"; then
    echo "[ipilink-smoke] LINK gate FAILED (no 'Link OK' in build log):"
    grep -A8 -F 'LINK FAILED' "$QB" | head -20
    exit 1
fi
if grep -qE '^FAIL: ' "$QB"; then
    echo "[ipilink-smoke] COMPILE gate FAILED:"
    grep -B1 -A6 -E '^FAIL: ' "$QB" | head -30
    exit 1
fi
echo "[ipilink-smoke] LINK gate OK"

# --- gate 2: the LINK-MAP gate (no >= 0x200000 shadow hazard for IPI state) -
MAP_OK=1
SYMS=$(nm build/kernel-smp.elf | grep -E ' [bBdD] (ipi_stats|call_queue|call_queue_head|call_queue_tail|call_queue_lock|ipi_bsp_apic_id|ipi_ready)$')
if [ -z "$SYMS" ]; then
    echo "[ipilink-smoke] LINK-MAP gate FAILED: no IPI symbols found in kernel-smp.elf"
    MAP_OK=0
else
    while read -r addr type name; do
        dec=$((16#$addr))
        if [ "$dec" -ge $((0x200000)) ]; then
            echo "[ipilink-smoke] SHADOW HAZARD: $name at 0x$addr (>= 0x200000)"
            MAP_OK=0
        else
            echo "[ipilink-smoke]   map ok: $name @ 0x$addr"
        fi
    done <<< "$SYMS"
fi
# Visibility (not a hard gate): where the packed control .bss ends. G0 finding:
# the LIVE user link base is 0x800000 (userspace.ld) -- linker.ld's "0x200000"
# comment is STALE (predates the user-base move), so packed .bss ending past
# 0x200000 (0x25d000 SMP / 0x236000 default) is NOT a hazard today. The IPI gate
# above is kept at the stricter historical 0x200000 bound anyway.
BDEF=$(nm build/kernel-smp.elf | grep -E ' __bss_deferred_start$' | awk '{print $1}')
[ -n "$BDEF" ] && echo "[ipilink-smoke]   info: __bss_deferred_start = 0x$BDEF (real shadow boundary = 0x800000 user base)"
if [ "$MAP_OK" != "1" ]; then
    echo "[ipilink-smoke] LINK-MAP gate FAILED"
    exit 1
fi
echo "[ipilink-smoke] LINK-MAP gate OK (all IPI globals < 0x200000)"

if [ ! -s iso/boot/initrd.img ]; then
    echo "[ipilink-smoke] iso/boot/initrd.img missing -- run build_all first"; exit 1
fi
if [ ! -s build/kernel.elf ]; then
    echo "[ipilink-smoke] build/kernel.elf missing (need it to restore the iso tree)"; exit 1
fi

echo "[ipilink-smoke] assembling ISO (kernel-smp.elf + existing initrd) ..."
cp build/kernel-smp.elf iso/boot/kernel.elf
grub-mkrescue -o build/automationos-ipilink.iso iso/ 2>/dev/null \
    || { echo "[ipilink-smoke] grub-mkrescue FAILED"; cp build/kernel.elf iso/boot/kernel.elf; exit 1; }
# Restore the default kernel in the iso tree so a later default smoke isn't poisoned.
cp build/kernel.elf iso/boot/kernel.elf

rm -f "$SER"
echo "[ipilink-smoke] booting qemu -smp 2 (timeout ${QEMU_TIMEOUT}s) ..."
timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
    -cdrom build/automationos-ipilink.iso \
    -m 512 -smp 2 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== IPI init (vector claim) ==="
grep -F '[IPI]' "$SER" | head -4

echo "=== gate 3: IPILINK (the G0 acceptance) ==="
grep -F 'IPILINK' "$SER" | tail -2
IPI_OK=0; grep -qF 'IPILINK: PASS ipi_resched=1 cpu1_count=1' "$SER" && IPI_OK=1

echo "=== gate 4: F3-5 regression ladder ==="
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
echo "  F2=$F2_OK APCURRENT=$AC_OK cpu1hello_marks=$MARKS exit42=$EXIT42 reaped=$REAPED alive=$ALIVE sched_viol=$NSCHED panic=$NPANIC"

if [ "$IPI_OK" = "1" ] && [ "$F2_OK" = "1" ] && [ "$AC_OK" = "1" ] && \
   [ "$MARKS" -ge 1 ] && [ "$EXIT42" = "1" ] && [ "$REAPED" = "1" ] && \
   [ "$ALIVE" = "1" ] && [ "$NSCHED" = "0" ] && [ "$NPANIC" = "0" ]; then
    echo "IPILINK-SMOKE: PASS link=1 map=1 ipilink=1 f35_regression=0"
    echo "[ipilink-smoke] RESULT: PASS -- IPI linked + proven, F3-5 ladder still green"
    exit 0
else
    echo "IPILINK-SMOKE: FAIL ipilink=$IPI_OK f2=$F2_OK apcurrent=$AC_OK marks=$MARKS exit42=$EXIT42 reaped=$REAPED alive=$ALIVE sched=$NSCHED panic=$NPANIC"
    echo "--- last 40 serial lines ---"; tail -40 "$SER"
    exit 1
fi
