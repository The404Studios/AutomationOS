#!/bin/bash
# USB-EHCI-0 E1 acceptance: gate + inert init, no destructive hardware access.
#   (a) default build (EHCI_USB unset) stays clean + no EHCI code leaks in
#   (b) EHCI_USB=1 compiles ehci.c and links
#   (c) the EHCI_USB=1 kernel still boots to desktop, logs the inert EHCI init,
#       and does NOT panic
# NOTE (MERGE-PARKED-0): the original E1 probe grepped the literal "skeleton
# (E1)" string, but the branch's own E2 commit REPLACED the inert skeleton with
# bounded PCI discovery, so that string no longer exists -- ehci_init now prints
# "[EHCI] No EHCI controller found on PCI bus (E2)" in QEMU (no controller, no
# MMIO, no panic: still inert there). The probes below use that stable E2 marker
# so the E1 acceptance intent (default-clean / links / boots-inert) stays a live
# gate. ehci_e2.sh is the current milestone gate.
EHCI_MARK="No EHCI controller found on PCI bus (E2)"
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
FAIL=0

echo "=== E1a: default build (EHCI_USB unset) must stay clean ==="
T410_SAFE=1 SCHED_DEBUG=0 bash scripts/quick_build.sh > /tmp/e1_def.log 2>&1
grep -E "SUCCESS: build/kernel.elf|=== Results:|LINK FAILED" /tmp/e1_def.log | tail -2
DEF=$(stat -c%s build/kernel.elf 2>/dev/null); echo "  default kernel = ${DEF:-?} bytes"
if grep -qa "$EHCI_MARK" build/kernel.elf; then echo "  FAIL: EHCI code leaked into default build"; FAIL=1; else echo "  OK: no EHCI code in default build"; fi

echo "=== E1b: EHCI_USB=1 build must compile ehci.c + link ==="
T410_SAFE=1 SCHED_DEBUG=0 EHCI_USB=1 bash scripts/quick_build.sh > /tmp/e1_ehci.log 2>&1
grep -E "EHCI_USB build|SUCCESS: build/kernel.elf|=== Results:|LINK FAILED" /tmp/e1_ehci.log | tail -4
EH=$(stat -c%s build/kernel.elf 2>/dev/null); echo "  EHCI kernel = ${EH:-?} bytes"
if grep -qa "$EHCI_MARK" build/kernel.elf; then echo "  OK: ehci.c linked (E2 marker present)"; else echo "  FAIL: ehci.c not linked"; FAIL=1; fi

echo "=== E1c: boot the EHCI_USB=1 kernel -> desktop, inert EHCI log, no panic ==="
cp build/kernel.elf iso/boot/kernel.elf
grub-mkrescue -o build/automationos-ehci-e1.iso iso/ > /tmp/e1_iso.log 2>&1 || { echo "  ISO FAIL"; tail /tmp/e1_iso.log; exit 1; }
timeout 50 qemu-system-x86_64 -cdrom build/automationos-ehci-e1.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 -serial file:/tmp/e1_boot.log -display none >/dev/null 2>&1
sleep 1
echo "  [EHCI] lines:"; grep -F "[EHCI]" /tmp/e1_boot.log | head -3 | sed 's/^/    /'
DT=$(grep -ciF desktop /tmp/e1_boot.log); PA=$(grep -ciF PANIC /tmp/e1_boot.log)
printf '  desktop=%s panic=%s\n' "$DT" "$PA"
grep -qF "$EHCI_MARK" /tmp/e1_boot.log || { echo "  FAIL: inert EHCI init line not in boot log"; FAIL=1; }
[ "$DT" -ge 1 ] || { echo "  FAIL: desktop not reached"; FAIL=1; }
[ "$PA" -eq 0 ] || { echo "  FAIL: panic during boot"; FAIL=1; }

echo "=== RESULT ==="
[ "$FAIL" = 0 ] && echo "E1 PASS (gate+inert: default clean; EHCI=1 links; boots to desktop, inert, no panic)" || { echo "E1 FAIL"; exit 1; }
