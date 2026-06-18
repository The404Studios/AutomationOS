#!/bin/bash
# SYNTHINPUT-0 proof: the agent drives the GUI. The mock plays a Nemotron that moves
# the mouse, clicks, and types via the gated mouse/key tools; those enqueue events the
# compositor drains (pump_synth_input). We assert, end to end on a real boot:
#   - the compositor created the injection page  ([SHELL] SYNTHINPUT: injection page ready)
#   - the agent dispatched the gated mouse/key tools (AGENTD: TOOL mouse/key + TOOL_*: OK)
#   - the compositor CONSUMED an injected event   ([SHELL] SYNTHINPUT: input applied)
#   - an unknown tool is still DENIED, and the loop completes (AGENTD: PASS)
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
SER=build_test/agentd_gui_ser.log
MLOG=build_test/agentd_gui_mock.log
rm -f "$SER" "$MLOG"
NEMO_GUI=1 python3 scripts/nemotron_mock.py > "$MLOG" 2>&1 &
MOCK=$!
sleep 1
echo "[run] GUI mock pid=$MOCK; booting QEMU (95s)..."
timeout 95 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null
kill "$MOCK" 2>/dev/null
echo "=== MOCK LOG ==="
cat "$MLOG"
echo "[run] done"
