#!/bin/bash
# WPA3-SAE verify: sae_selftest passes AND p256 stays unbroken (the agent exposed
# p256 internals + added fe_sqrt etc.). Userspace-only -> build_all. The kernel in
# build/kernel.elf is reused as-is (crypto is userspace). Run: wsl -d Arch bash build_test/sae_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[sae] build_all (sae.c + modified p256.c)..."
bash scripts/build_all.sh > /tmp/sae_ba.log 2>&1
if grep -qiE 'error:|undefined reference|redefinition|conflicting types' /tmp/sae_ba.log; then
  echo "BUILD ERRORS:"; grep -iE 'error:|undefined reference|redefinition|conflicting types' /tmp/sae_ba.log | head -20; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/sae_ser.log; rm -f "$SER"
echo "[sae] booting (70s)..."
timeout 70 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== crypto markers ==="
grep -aE '\[CRYPTOTEST\] (sae|p256|gcmp)|CRYPTOTEST:' "$SER"
SAE=0;  grep -qaE '\[CRYPTOTEST\] sae .*: PASS' "$SER" && SAE=1
P256=0; grep -qaE '\[CRYPTOTEST\] p256 .*: PASS' "$SER" && P256=1
ALL=0;  grep -qaE 'CRYPTOTEST: PASS' "$SER" && ALL=1
echo ""
echo "sae=$SAE p256_intact=$P256 battery=$ALL"
if [ "$SAE" = 1 ] && [ "$P256" = 1 ] && [ "$ALL" = 1 ]; then
  echo "WPA3-SAE: PASS (sae KAT green; p256 unbroken; full battery CRYPTOTEST: PASS)"
  exit 0
else
  echo "WPA3-SAE: FAIL (sae=$SAE p256=$P256 battery=$ALL)"
  echo "--- tail ---"; tail -25 "$SER"; exit 1
fi
