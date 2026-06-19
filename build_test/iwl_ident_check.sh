#!/bin/bash
# IWL-IDENT verify: the real-iwlwifi scaffolding compiles + boots clean, and in
# QEMU (no iwlwifi card) reports "no Intel WiFi card found" gracefully -- the
# graceful-absence acceptance for a default-OFF, hardware-deferred driver.
# On a real T410 the same build prints the card name + CSR_HW_REV instead.
# Run: wsl -d Arch bash build_test/iwl_ident_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[iwl] quick_build IWLWIFI=1..."
IWLWIFI=1 bash scripts/quick_build.sh > /tmp/iwl_qb.log 2>&1
if grep -qiE 'error:|^FAIL:|undefined reference' /tmp/iwl_qb.log; then
  echo "KERNEL BUILD ERRORS:"; grep -iE 'error:|^FAIL:|undefined reference' /tmp/iwl_qb.log | head; exit 1
fi
grep -q SUCCESS /tmp/iwl_qb.log || { echo "kernel no SUCCESS"; tail -6 /tmp/iwl_qb.log; exit 1; }
grep -q 'IWLWIFI build' /tmp/iwl_qb.log || echo "  (warn: IWLWIFI flag echo not seen)"

echo "[iwl] build_all..."
bash scripts/build_all.sh > /tmp/iwl_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/iwl_ba.log; then
  echo "USERSPACE BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/iwl_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/iwl_ser.log; rm -f "$SER"
echo "[iwl] booting (60s)..."
timeout 60 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== IWL markers ==="
grep -aE 'IWL:' "$SER" | head
NOCARD=0; grep -qaE 'IWL: no Intel WiFi card found' "$SER" && NOCARD=1
BOOT=0;   grep -qaE 'All services started' "$SER" && BOOT=1
echo ""
echo "graceful_no_card=$NOCARD desktop=$BOOT"
if [ "$NOCARD" = 1 ] && [ "$BOOT" = 1 ]; then
  echo "IWL-IDENT: PASS (scaffolding compiles + boots clean; graceful no-card in QEMU)"
  exit 0
else
  echo "IWL-IDENT: FAIL (no_card=$NOCARD boot=$BOOT)"
  echo "--- tail ---"; tail -20 "$SER"; exit 1
fi
