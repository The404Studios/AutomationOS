#!/bin/bash
# Boot-verify the TLS trust fix: no boot-time regression (CRYPTOTEST battery,
# which includes the x509/ca selftests, still PASS; desktop comes up).
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
SER=build_test/tls_verify_ser.log; rm -f "$SER"
timeout 95 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null
echo "=== crypto/tls/cert markers ==="
grep -aE 'CRYPTOTEST|x509|X509|CERT|ca_bundle|TLS' "$SER" | head -20
echo "=== desktop ==="
grep -aE 'All services started' "$SER" | head -1
PASS=1
grep -qaE 'CRYPTOTEST: PASS' "$SER" || { echo "  CRYPTOTEST not PASS"; PASS=0; }
grep -qaE 'All services started' "$SER" || { echo "  desktop FAIL"; PASS=0; }
echo ""
[ "$PASS" = 1 ] && echo "TLS-BOOT: PASS (no regression)" || echo "TLS-BOOT: FAIL"
