#!/bin/bash
# End-to-end proof: the agent drives the run-open-code pipeline through the GATED
# toolset -- mkdir -> write(base64 C) -> compile on-device -> execute -> ps -> and a
# destructive /etc delete the gate MUST deny. Zero cost (scripted mock, no API key).
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
SER=build_test/agentd_codetask_ser.log
MLOG=build_test/agentd_codetask_mock.log
rm -f "$SER" "$MLOG"
NEMO_CODETASK=1 python3 scripts/nemotron_mock.py > "$MLOG" 2>&1 &
MOCK=$!
sleep 1
echo "[run] CODETASK mock pid=$MOCK; booting QEMU (110s)..."
timeout 110 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null
kill "$MOCK" 2>/dev/null
echo "=== MOCK LOG (what the agent did, step by step) ==="
cat "$MLOG"
echo "=== compiled-program output + agent trace in serial ==="
grep -aE "AGENTD:|AGENTOK|EXEC |COMPILED|WROTE|MKDIR|RM |DENY" "$SER" | head -40
echo "[run] done"
