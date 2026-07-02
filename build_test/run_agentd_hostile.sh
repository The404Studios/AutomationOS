#!/bin/bash
# Adversarial proof: the mock plays the hostile model (unknown destructive tool +
# path traversal); the OS gate MUST reject both, then run the final legit read.
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
SER=build_test/agentd_hostile_ser.log
MLOG=build_test/agentd_hostile_mock.log
rm -f "$SER" "$MLOG"
NEMO_HOSTILE=1 python3 scripts/nemotron_mock.py > "$MLOG" 2>&1 &
MOCK=$!
sleep 1
echo "[run] HOSTILE mock pid=$MOCK; booting QEMU (95s)..."
timeout 95 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null
kill "$MOCK" 2>/dev/null
echo "=== MOCK LOG ==="
cat "$MLOG"
echo "[run] done"
