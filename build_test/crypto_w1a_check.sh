#!/bin/bash
# CRYPTO WAVE-1a verify: PBKDF2-HMAC-SHA1 + AES key-wrap (RFC 3394) added to the
# boot cryptotest KAT battery. Userspace-only -> build_all. Assert both new KATs
# pass AND the whole battery stays green. Run: wsl -d Arch bash build_test/crypto_w1a_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[crypto] build_all (userspace)..."
bash scripts/build_all.sh > /tmp/crypto_ba.log 2>&1
if grep -qiE 'error:|undefined reference|redefinition|conflicting types' /tmp/crypto_ba.log; then
  echo "BUILD ERRORS:"; grep -iE 'error:|undefined reference|redefinition|conflicting types' /tmp/crypto_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/crypto_w1a_ser.log; rm -f "$SER"
echo "[crypto] booting (70s)..."
timeout 70 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== crypto markers ==="
grep -aE 'CRYPTOTEST:|pbkdf2|key-wrap' "$SER"
PB=0; grep -qaE '\[CRYPTOTEST\] pbkdf2-hmac-sha1 .*: PASS' "$SER" && PB=1
KW=0; grep -qaE '\[CRYPTOTEST\] aes key-wrap .*: PASS' "$SER" && KW=1
ALL=0; grep -qaE 'CRYPTOTEST: PASS' "$SER" && ALL=1
echo ""
echo "pbkdf2_pass=$PB keywrap_pass=$KW battery_pass=$ALL"
if [ "$PB" = "1" ] && [ "$KW" = "1" ] && [ "$ALL" = "1" ]; then
  echo "CRYPTO-W1a: PASS (pbkdf2 + keywrap KATs green; full battery PASS)"
  exit 0
else
  echo "CRYPTO-W1a: FAIL"
  echo "--- tail ---"; tail -25 "$SER"
  exit 1
fi
