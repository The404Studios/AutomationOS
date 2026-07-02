#!/bin/bash
# Boot-sanity the T410 ethernet ISO (PCH_NIC=1 build) in QEMU.
# The PCH path is QEMU-safe (QEMU is a classic 82540, not a PCH part), so this
# should reach the desktop exactly like the default build -- proving the
# PCH_NIC=1 kernel is not broken. Run: wsl -d Arch bash build_test/t410eth_bootcheck.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
SER=build_test/t410eth_boot_ser.log
rm -f "$SER"
ISO=build/automationos-t410-eth.iso
[ -s "$ISO" ] || { echo "MISSING: $ISO"; exit 1; }

timeout 75 qemu-system-x86_64 -cdrom "$ISO" -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== serial captured ==="
ls -la "$SER" 2>/dev/null || { echo "NO SERIAL"; exit 1; }
echo ""
echo "=== boot + network markers ==="
grep -aiE 'E1000|Initializing networking|network:|All services started|compositor|desktop|reached|PANIC|EXCEPTION|UNHANDLED' "$SER" | head -30
echo ""
echo "=== verdict ==="
PANIC=0; grep -aqiE 'PANIC|UNHANDLED EXCEPTION' "$SER" && PANIC=1
BOOTED=0; grep -aqiE 'All services started|compositor|desktop' "$SER" && BOOTED=1
NET=0; grep -aqiE 'e1000|networking|network:' "$SER" && NET=1
echo "panic=$PANIC booted=$BOOTED net_init_ran=$NET"
if [ "$PANIC" = "0" ] && [ "$BOOTED" = "1" ]; then
  echo "T410ETH-BOOT: PASS (ISO boots clean in QEMU; PCH_NIC kernel not broken)"
else
  echo "T410ETH-BOOT: CHECK (see $SER tail below)"
  tail -25 "$SER"
fi
