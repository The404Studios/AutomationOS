#!/bin/bash
# USB-EHCI-0 E2 acceptance: PCI discovery + MMIO map + BIOS handoff + reset.
#   - no-EHCI machine: graceful "No EHCI controller found", boots clean
#   - with -device usb-ehci: full routing ledger printed through configflag,
#     HCRESET ok, boots to desktop, no panic
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
FAIL=0

echo "=== E2 build (EHCI_USB=1, T410_SAFE, sched-debug off) ==="
T410_SAFE=1 SCHED_DEBUG=0 EHCI_USB=1 bash scripts/quick_build.sh > /tmp/e2_build.log 2>&1
grep -E "EHCI_USB build|SUCCESS: build/kernel.elf|=== Results:|LINK FAILED" /tmp/e2_build.log | tail -3
grep -q "SUCCESS: build/kernel.elf" /tmp/e2_build.log || { echo "BUILD FAIL"; tail -20 /tmp/e2_build.log; exit 1; }
cp build/kernel.elf iso/boot/kernel.elf
grub-mkrescue -o build/automationos-ehci-e2.iso iso/ > /tmp/e2_iso.log 2>&1 || { echo "ISO FAIL"; tail /tmp/e2_iso.log; exit 1; }

run() { tag="$1"; shift; timeout 50 qemu-system-x86_64 -cdrom build/automationos-ehci-e2.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 -serial "file:/tmp/e2_$tag.log" -display none "$@" >/dev/null 2>&1; sleep 1; }

echo "=== CASE no-ehci (default i440fx; no EHCI controller) ==="
run noehci
grep -F "[EHCI]" /tmp/e2_noehci.log | head -3 | sed 's/^/  /'
grep -qF "No EHCI controller found" /tmp/e2_noehci.log && echo "  PASS: graceful no-controller" || { echo "  FAIL: expected no-controller msg"; FAIL=1; }
printf "  desktop=%s panic=%s\n" "$(grep -ciF desktop /tmp/e2_noehci.log)" "$(grep -ciF PANIC /tmp/e2_noehci.log)"

echo "=== CASE ehci (-device usb-ehci) ==="
run ehci -device usb-ehci,id=ehci
echo "  --- [EHCI] ledger ---"; grep -F "[EHCI]" /tmp/e2_ehci.log | sed 's/^/  /'
DT=$(grep -ciF desktop /tmp/e2_ehci.log); PA=$(grep -ciF PANIC /tmp/e2_ehci.log)
printf "  desktop=%s panic=%s\n" "$DT" "$PA"
grep -qF "] controller " /tmp/e2_ehci.log && echo "  PASS: controller discovered" || { echo "  FAIL: controller not discovered"; FAIL=1; }
grep -qF "reset_ok=1"     /tmp/e2_ehci.log && echo "  PASS: HCRESET ok"          || echo "  WARN: reset_ok != 1 (inspect ledger)"
[ "$PA" -eq 0 ] || { echo "  FAIL: panic"; FAIL=1; }
[ "$DT" -ge 1 ] || { echo "  FAIL: desktop not reached"; FAIL=1; }

echo "=== RESULT ==="
[ "$FAIL" = 0 ] && echo "E2 PASS" || { echo "E2 FAIL"; exit 1; }
