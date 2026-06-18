#!/bin/bash
# DHCP-APPLY verify: dhcpc gained an optional "dhcpc run [ifname]" arg (default
# eth0) + comment cleanup. Userspace-only (kernel unchanged) -> build_all only.
# Asserts dhcpc still compiles + its selftest passes + the desktop is unregressed.
# (The live ifname-apply path is exercised end-to-end in WAVE 1 by wpasupp.)
# Run: wsl -d Arch bash build_test/dhcp_apply_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[dhcp] build_all (userspace; kernel unchanged)..."
bash scripts/build_all.sh > /tmp/dhcp_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/dhcp_ba.log; then
  echo "BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/dhcp_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/dhcp_apply_ser.log; rm -f "$SER"
echo "[dhcp] booting (75s)..."
timeout 75 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== dhcp + boot markers ==="
grep -aE 'DHCPC SELFTEST|lease applied|All services started' "$SER" | head
SELF=0; grep -qaE 'DHCPC SELFTEST: PASS' "$SER" && SELF=1
BOOT=0; grep -qaE 'All services started' "$SER" && BOOT=1
echo ""
echo "selftest_pass=$SELF desktop_up=$BOOT"
if [ "$SELF" = "1" ] && [ "$BOOT" = "1" ]; then
  echo "DHCP-APPLY: PASS (dhcpc builds + selftests; ifname-arg backward-compatible; desktop unregressed)"
  exit 0
else
  echo "DHCP-APPLY: CHECK"
  echo "--- tail ---"; tail -20 "$SER"
  exit 1
fi
