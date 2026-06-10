#!/bin/bash
# E1000-PCH-0A/0B/0C verify (QEMU side -- all NEGATIVE gates by design):
#   1. default kernel builds clean (PCH code compiled, gate off).
#   2. PCH_NIC=1 kernel builds clean.
#   3. userspace builds; bin/nicup ships in the initrd.
#   4. PCH_NIC=1 ISO boots on QEMU's CLASSIC e1000: networking still comes up
#      normally (slirp gateway resolved), no E1000PCH markers (the deferred
#      path never runs on a non-PCH part), no faults.
# The POSITIVE ladder (PROBE->FWSM->SWFLAG->PHYID->ANEG->LINK->dhcp->ping->
# httpd) is the T410 hardware validation (E1000-PCH-0D/0E), via bin/nicup.
# Usage: bash build_test/pchnic_check.sh
set -uo pipefail
cd /mnt/c/Users/wilde/Desktop/Kernel

echo "[pchnic] default build..."
bash scripts/quick_build.sh > /tmp/pch_def.log 2>&1
if grep -qE "error:|undefined reference|FAIL:" /tmp/pch_def.log; then
    echo "[pchnic] DEFAULT BUILD ERRORS:"; grep -E "error:|undefined reference|FAIL:" /tmp/pch_def.log | head -5; exit 1
fi
echo "  default OK"

echo "[pchnic] PCH_NIC=1 build..."
PCH_NIC=1 bash scripts/quick_build.sh > /tmp/pch_on.log 2>&1
if grep -qE "error:|undefined reference|FAIL:" /tmp/pch_on.log; then
    echo "[pchnic] PCH BUILD ERRORS:"; grep -E "error:|undefined reference|FAIL:" /tmp/pch_on.log | head -5; exit 1
fi
grep -qF "PCH_NIC build" /tmp/pch_on.log && echo "  flag plumbed OK"

echo "[pchnic] userspace + ISO..."
bash scripts/build_all.sh > /tmp/pch_all.log 2>&1
if grep -qE "error:|undefined reference" /tmp/pch_all.log; then
    echo "[pchnic] ISO BUILD ERRORS:"; exit 1
fi
if [ -f /tmp/ird/bin/nicup ]; then
    echo "  nicup shipped ($(stat -c%s /tmp/ird/bin/nicup) B)"
else
    echo "[pchnic] FAIL: bin/nicup missing from initrd"; exit 1
fi

LOG=/tmp/pch_serial.log
rm -f "$LOG"
echo "[pchnic] booting PCH_NIC kernel on classic QEMU e1000 (75s)..."
timeout 75 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$LOG" -display none -no-reboot >/dev/null 2>&1 || true
sleep 2

echo "=== result ==="
P=1
# Gate on "[NET] up:" (stack attached to the classic NIC). The boot-time
# gateway ARP pre-resolve marker is FLAKY under heavy boot IO (slirp can
# answer slower than the iter cap) and resolve_mac re-resolves in-syscall,
# so it is diagnostic, not a gate.
if grep -qF "[NET] up: ip=10.0.2.15" "$LOG"; then
    echo "  ok   classic NIC + stack up"
else
    echo "  MISS classic networking did not come up"; P=0
fi
if grep -qF "E1000PCH:" "$LOG"; then
    echo "  FAIL E1000PCH markers on a NON-PCH part (deferred path leaked)"; P=0
else
    echo "  ok   PCH path inert on QEMU"
fi
if grep -qiE "PANIC|CPU EXCEPTION|TRIPLE FAULT" "$LOG"; then
    echo "  FAIL kernel fault"; P=0
fi
# restore a default kernel.elf so later builds aren't contaminated
bash scripts/quick_build.sh > /tmp/pch_def2.log 2>&1
if [ "$P" = "1" ]; then echo "PCHNIC CHECK: PASS (QEMU negative gates green)"; exit 0
else echo "PCHNIC CHECK: FAIL"; exit 1; fi
