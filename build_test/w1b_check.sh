#!/bin/bash
# WAVE-1b verify (two tracks, one build+boot -- both userspace, disjoint files):
#   crypto: CCM (NIST 800-38C) + CCMP (802.11 WPA2) + GCMP (802.11 WPA3) KATs in
#           the boot cryptotest battery.
#   ui:     the additive animated toolkit must stay INTEGER-ONLY (no float/canary).
# Run: wsl -d Arch bash build_test/w1b_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[w1b] build_all (crypto ccmp/gcmp + ui toolkit)..."
bash scripts/build_all.sh > /tmp/w1b_ba.log 2>&1
if grep -qiE 'error:|undefined reference|redefinition|conflicting types' /tmp/w1b_ba.log; then
  echo "BUILD ERRORS:"; grep -iE 'error:|undefined reference|redefinition|conflicting types' /tmp/w1b_ba.log | head -20; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

# --- UI toolkit float/canary gate (must stay integer-only Q8) ---
XMM=$(objdump -d /tmp/ui.o 2>/dev/null | grep -ciE 'xmm|movss|movsd')
CANARY=$(objdump -d /tmp/ui.o 2>/dev/null | grep -c 'fs:0x28')
echo "ui.o: xmm/float=$XMM canary=$CANARY (both must be 0)"

SER=build_test/w1b_ser.log; rm -f "$SER"
echo "[w1b] booting (70s)..."
timeout 70 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== crypto markers ==="
grep -aE '\[CRYPTOTEST\] (aes-ccm|ccmp|gcmp)|CRYPTOTEST:' "$SER"
CCM=0;  grep -qaE '\[CRYPTOTEST\] aes-ccm .*: PASS' "$SER" && CCM=1
CCMP=0; grep -qaE '\[CRYPTOTEST\] ccmp .*: PASS' "$SER" && CCMP=1
GCMP=0; grep -qaE '\[CRYPTOTEST\] gcmp .*: PASS' "$SER" && GCMP=1
ALL=0;  grep -qaE 'CRYPTOTEST: PASS' "$SER" && ALL=1
BOOT=0; grep -qaE 'All services started' "$SER" && BOOT=1
echo ""
echo "ccm=$CCM ccmp=$CCMP gcmp=$GCMP battery=$ALL desktop=$BOOT ui_float=$XMM ui_canary=$CANARY"
if [ "$CCM" = "1" ] && [ "$CCMP" = "1" ] && [ "$GCMP" = "1" ] && [ "$ALL" = "1" ] && \
   [ "$BOOT" = "1" ] && [ "$XMM" = "0" ] && [ "$CANARY" = "0" ]; then
  echo "WAVE-1b: PASS (ccm/ccmp/gcmp KATs green; full battery PASS; UI toolkit float-free; desktop up)"
  exit 0
else
  echo "WAVE-1b: FAIL"; echo "--- tail ---"; tail -25 "$SER"; exit 1
fi
