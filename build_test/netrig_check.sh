#!/bin/bash
# NET-P1-A0 verify: (1) default kernel builds clean with the rig TU empty;
# (2) NET_SELFTEST=1 kernel boots and prints "NETRIG: PASS loopback=1 cap=1".
# Usage: bash build_test/netrig_check.sh
set -uo pipefail
cd /mnt/c/Users/wilde/Desktop/Kernel

echo "[netrig] default build (rig compiled empty)..."
bash scripts/quick_build.sh > /tmp/netrig_def.log 2>&1
if grep -qE "error:|undefined reference|FAIL:" /tmp/netrig_def.log; then
    echo "[netrig] DEFAULT BUILD ERRORS:"; grep -E "error:|undefined reference|FAIL:" /tmp/netrig_def.log | head -5; exit 1
fi
echo "  default OK"

echo "[netrig] NET_SELFTEST=1 build..."
NET_SELFTEST=1 bash scripts/quick_build.sh > /tmp/netrig_sf.log 2>&1
if grep -qE "error:|undefined reference|FAIL:" /tmp/netrig_sf.log; then
    echo "[netrig] SELFTEST BUILD ERRORS:"; grep -E "error:|undefined reference|FAIL:" /tmp/netrig_sf.log | head -5; exit 1
fi
grep -qF "NET_SELFTEST build" /tmp/netrig_sf.log && echo "  flag plumbed OK"

echo "[netrig] packaging ISO (userspace unchanged)..."
bash scripts/build_all.sh > /tmp/netrig_all.log 2>&1
if grep -qE "error:|undefined reference" /tmp/netrig_all.log; then
    echo "[netrig] ISO BUILD ERRORS:"; exit 1
fi

LOG=/tmp/netrig_serial.log
rm -f "$LOG"
echo "[netrig] booting (60s)..."
timeout 60 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$LOG" -display none -no-reboot >/dev/null 2>&1 || true
sleep 2

echo "=== result ==="
grep -E "NETRIG:|NETP1[A-Z]:" "$LOG" || { echo "rig markers MISSING (see $LOG)"; exit 1; }
P=1
grep -qF "NETRIG: PASS loopback=1 cap=1" "$LOG" || P=0
# Per-brick markers: only assert the ones that exist in this build's serial
# (each NET-P1 brick adds its own PASS line; a FAIL on any present marker fails).
if grep -qF "NETP1A:" "$LOG"; then
    grep -qF "NETP1A: SYNQ PASS" "$LOG" || P=0
fi
if grep -qF "NETP1B:" "$LOG"; then
    grep -qF "NETP1B: OOO PASS slots=4 reassembled=5840" "$LOG" || P=0
fi
if grep -qF "NETP1C:" "$LOG"; then
    grep -qF "NETP1C: ZWND PASS probes=3 delivered=1" "$LOG" || P=0
fi
if grep -qF "NETP1D:" "$LOG"; then
    grep -qF "NETP1D: UDPQ PASS depth=16 queued=16 dropped=0" "$LOG" || P=0
fi
if grep -qF "NETP1E:" "$LOG"; then
    grep -qF "NETP1E: SOCKMAX PASS n=32 heapok=1 extra_rejected=1" "$LOG" || P=0
fi
# NET-HARDENING-0 (F4/F5/F7) markers
if grep -qF "NETP1Q:" "$LOG"; then
    grep -qF "NETP1Q: WINUPD PASS armed=1 no_rtx_on_winupd=1 fires_on_dup=1" "$LOG" || P=0
fi
if grep -qF "NETP1R:" "$LOG"; then
    grep -qF "NETP1R: SYNQRETRY PASS synack=1 promoted=1" "$LOG" || P=0
fi
if grep -qF "NETP1S:" "$LOG"; then
    grep -qF "NETP1S: LORING PASS bursts=64 all_nonneg=1" "$LOG" || P=0
fi
if grep -qF "NETP1T:" "$LOG"; then
    grep -qF "NETP1T: DUPFIN PASS established=1 first_ack=1 in_close_wait=1 reack=1" "$LOG" || P=0
fi
if grep -qF "NETP1U:" "$LOG"; then
    grep -qF "NETP1U: RSTVAL PASS listen_survives=1 est_survives_oow=1 est_resets_inwin=1" "$LOG" || P=0
fi
if grep -qF "NETP1V:" "$LOG"; then
    grep -qF "NETP1V: ACKACC PASS established=1 snd_una_held=1" "$LOG" || P=0
fi
if grep -qF "NETP1W:" "$LOG"; then
    grep -qF "NETP1W: MARTIAN PASS spoof_src=1 spoof_dst=1 legit=1" "$LOG" || P=0
fi
if grep -qF "NETP1X:" "$LOG"; then
    grep -qF "NETP1X: IPFRAG PASS frag_dropped=1 legit=1" "$LOG" || P=0
fi
if grep -qF "NETP1Y:" "$LOG"; then
    grep -qF "NETP1Y: CKSUM0 PASS established=1 good=1 zero_dropped=1" "$LOG" || P=0
fi
if grep -qF "NETP1Z:" "$LOG"; then
    grep -qF "NETP1Z: SWSWND PASS established=1 filled=1 reopen_ack=1" "$LOG" || P=0
fi
# Real unrecoverable kernel faults always fail.
if grep -qiE "KERNEL PANIC|TRIPLE FAULT" "$LOG"; then
    echo "KERNEL FAULT during boot"; P=0
fi
# A "CPU EXCEPTION" that the kernel HANDLES by terminating the faulting ring-3
# process is healthy -- e.g. the sigtest bad-handler-VA fail-safe deliberately
# faults at userspace VA 0x4000 to prove the kernel survives. Only an exception
# WITHOUT a matching "Terminating faulting process" (i.e. a kernel-context fault)
# is a real failure.
exc=$(grep -ciE "CPU EXCEPTION" "$LOG")
handled=$(grep -ciE "Terminating faulting process" "$LOG")
if [ "$exc" -gt "$handled" ]; then
    echo "UNHANDLED KERNEL EXCEPTION during boot (exc=$exc handled=$handled)"; P=0
fi
if [ "$P" = "1" ]; then
    echo "NETRIG CHECK: PASS"
    # restore a default (rig-free) kernel.elf so later builds aren't contaminated
    bash scripts/quick_build.sh > /tmp/netrig_def2.log 2>&1
    exit 0
else
    echo "NETRIG CHECK: FAIL"; exit 1
fi
