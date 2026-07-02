#!/bin/bash
# Zero-cost proof: boot the OS with the scripted Nemotron mock broker and
# capture sbin/agentd driving the gated multi-step tool loop to DONE.
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
SER=build_test/agentd_ser.log
MLOG=build_test/agentd_mock.log
rm -f "$SER" "$MLOG"
python3 scripts/nemotron_mock.py > "$MLOG" 2>&1 &
MOCK=$!
sleep 1
echo "[run] mock pid=$MOCK; booting QEMU (95s) with slirp net..."
timeout 95 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null
kill "$MOCK" 2>/dev/null
echo "=== MOCK LOG ==="
cat "$MLOG"
echo "[run] done"
