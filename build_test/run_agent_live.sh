#!/bin/bash
# run_agent_live.sh -- boot AutomationOS with a LIVE DeepSeek agent brain.
# ============================================================================
# Same harness as run_agentd_proof.sh, but the broker on 10.0.2.2:8433 is the
# real DeepSeek model (scripts/deepseek_broker.py) instead of the scripted mock.
# sbin/agentd sends its GOAL, DeepSeek plans tool calls, the OS gates+runs each
# one, and the loop runs to DONE -- all over the QEMU slirp seam.
#
# The key lives ONLY in your host env; it never enters the image or the repo:
#   export DEEPSEEK_API_KEY=sk-...
#   bash build_test/run_agent_live.sh
#
# Headless boot uses agentd's default goal (sys_spawn truncates a multi-word argv
# to one word). For a full multi-word goal + per-step Allow/Deny, boot the ISO
# interactively and drive the cockpit instead -- the broker is identical.
#
# Env passthrough: DEEPSEEK_MODEL (deepseek-chat|deepseek-reasoner),
#   DEEPSEEK_MAX_STEPS, DEEPSEEK_TEMPERATURE, LIVE_TIMEOUT (default 120s).
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

# One broker backs agentd (8433 agentic) + the in-OS Claude apps (8432) + modelbridge
# (8431) at once. Override by exporting DEEPSEEK_BROKER_PORTS before running.
: "${DEEPSEEK_BROKER_PORTS:=8433,8432,8431}"; export DEEPSEEK_BROKER_PORTS

if [ -z "$DEEPSEEK_API_KEY" ] && [ -z "$MODEL_API_KEY" ] && [ "$MODEL_SCHEME" != "http" ]; then
  echo "ERROR: DEEPSEEK_API_KEY is not set. Run: export DEEPSEEK_API_KEY=sk-..."
  echo "(Without it the broker still proves plumbing but returns a no-key DONE.)"
fi
if [ ! -f build/automationos.iso ]; then
  echo "ERROR: build/automationos.iso missing -- build it first (scripts/build_all.sh)."
  exit 1
fi

SER=build_test/agent_live_ser.log
BLOG=build_test/agent_live_broker.log
TMO="${LIVE_TIMEOUT:-120}"
rm -f "$SER" "$BLOG"

python3 scripts/deepseek_broker.py > "$BLOG" 2>&1 &
BROKER=$!
sleep 1
echo "[run] deepseek broker pid=$BROKER (model=${DEEPSEEK_MODEL:-deepseek-chat}); booting QEMU (${TMO}s) with slirp net..."
timeout "$TMO" qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null
kill "$BROKER" 2>/dev/null

echo "=== BROKER LOG (host-side DeepSeek calls) ==="
cat "$BLOG"
echo "=== AGENTD SERIAL MARKERS (guest-side gated loop) ==="
grep -E "AGENTD:|AGENT" "$SER" 2>/dev/null || echo "(no AGENTD markers in serial)"

echo "=== VERDICT ==="
if grep -q "AGENTD: DONE" "$SER" 2>/dev/null; then
  echo "LIVE-AGENT: PASS (agentd reached DONE driven by the live broker)"
elif grep -q "AGENTD: TOOL" "$SER" 2>/dev/null; then
  echo "LIVE-AGENT: PARTIAL (live TOOL calls ran; loop did not reach DONE in ${TMO}s)"
elif grep -q "AGENTD: GOAL sent" "$SER" 2>/dev/null; then
  echo "LIVE-AGENT: PLUMBING-ONLY (GOAL sent; check broker log -- key set? model reachable?)"
else
  echo "LIVE-AGENT: NO-CONTACT (agentd never sent GOAL -- net not ready or agentd not spawned)"
fi
echo "[run] done"
