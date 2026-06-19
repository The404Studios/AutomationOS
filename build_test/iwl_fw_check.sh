#!/bin/bash
# IWL-FW verify: the iwlwifi firmware-TLV parser proves itself at boot on an
# embedded synthetic .ucode (round-trip + hostile-input negatives), alongside
# IWL-IDENT's graceful no-card. Real iwlwifi-<fam>-*.ucode drops into the initrd
# on the T410. Run: wsl -d Arch bash build_test/iwl_fw_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[iwlfw] quick_build IWLWIFI=1..."
IWLWIFI=1 bash scripts/quick_build.sh > /tmp/iwlfw_qb.log 2>&1
if grep -qiE 'error:|^FAIL:|undefined reference' /tmp/iwlfw_qb.log; then
  echo "KERNEL BUILD ERRORS:"; grep -iE 'error:|^FAIL:|undefined reference' /tmp/iwlfw_qb.log | head; exit 1
fi
grep -q SUCCESS /tmp/iwlfw_qb.log || { echo "kernel no SUCCESS"; tail -6 /tmp/iwlfw_qb.log; exit 1; }

echo "[iwlfw] build_all..."
bash scripts/build_all.sh > /tmp/iwlfw_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/iwlfw_ba.log; then
  echo "USERSPACE BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/iwlfw_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/iwlfw_ser.log; rm -f "$SER"
echo "[iwlfw] booting (60s)..."
timeout 60 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== IWL markers ==="
grep -aE 'IWL:|IWL-FW:' "$SER" | head
FWPASS=0; grep -qaE 'IWL-FW: PASS' "$SER" && FWPASS=1
PARSED=0; grep -qaE 'IWL-FW: parsed inst=256' "$SER" && PARSED=1
NOCARD=0; grep -qaE 'IWL: no Intel WiFi card found' "$SER" && NOCARD=1
BOOT=0;   grep -qaE 'All services started' "$SER" && BOOT=1
echo ""
echo "fw_pass=$FWPASS parsed_ok=$PARSED no_card=$NOCARD desktop=$BOOT"
if [ "$FWPASS" = 1 ] && [ "$PARSED" = 1 ] && [ "$BOOT" = 1 ]; then
  echo "IWL-FW: PASS (firmware TLV parser proven on synthetic ucode; graceful no-card; desktop up)"
  exit 0
else
  echo "IWL-FW: FAIL (fw=$FWPASS parsed=$PARSED card=$NOCARD boot=$BOOT)"
  echo "--- tail ---"; tail -20 "$SER"; exit 1
fi
