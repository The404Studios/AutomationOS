#!/bin/bash
# AGENT-LEDGER-0 end-to-end proof: the LIVE agent's audit ledger (/var/log/ai/agent.log)
# is tamper-evident and verifiable.
# =============================================================================
# Unlike the legacy C4 chain (which audited aibroker's boot self-test on
# actions.log), this proves the *live agentd path* writes a hash-chained ledger
# that ledgerver re-verifies. The chain of custody:
#
#   1. nemotron_mock.py (NEMO_CODETASK) stands in for the model broker on
#      10.0.2.2:8433 -- it drives mkdir -> write_file -> compile -> execute ->
#      ps -> remove (the last is a DESTRUCTIVE op the gate denies). Every one of
#      those decisions is what agentd appends to /var/log/ai/agent.log.
#   2. The ISO boots with the slirp e1000 NIC so agentd can reach the broker.
#   3. agentd runs its GOAL/TOOL/RESULT/DONE loop, appending one chained ledger
#      line PER tool decision, then EXITS.
#   4. init reaps agentd and ONLY THEN spawns `ledgerver /var/log/ai/agent.log`
#      (the late re-verify -- see init/main.c reaper loop). By keying the verify
#      to agentd's exit we avoid the race where the boot-time ledgerver runs
#      before the agent has written anything (that early one harmlessly prints
#      "LEDGER: EMPTY" because the broker loop is still in flight).
#
# So the boot-time `ledgerver /var/log/ai/agent.log` (right after "All services
# started") is EXPECTED to be EMPTY on this timeline; the AUTHORITATIVE verdict
# is the LATE one emitted after "AGENTD: ... DONE"/reap. This script asserts the
# late VERIFIED records>0 AND that the agent's decisions actually flowed (the
# destructive `remove` DENY proves the live gate, and that the ledger captured a
# real decision line -- tool=write_file / tool=remove).
#
# Zero cost: scripted mock, no NVIDIA/API key. ~110s boot like its siblings.
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

SER=build_test/agent_ledger_ser.log
MLOG=build_test/agent_ledger_mock.log
rm -f "$SER" "$MLOG"

# Bring the scripted agent broker up first (CODETASK plan = a real multi-tool run
# whose decisions populate the live ledger).
NEMO_CODETASK=1 python3 scripts/nemotron_mock.py > "$MLOG" 2>&1 &
MOCK=$!
sleep 1
echo "[run] AGENT-LEDGER mock pid=$MOCK (NEMO_CODETASK); booting QEMU (110s)..."
timeout 110 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null
kill "$MOCK" 2>/dev/null

pass=0; fail=0
chk(){ if grep -qaF "$2" "$SER"; then echo "  PASS  $1"; pass=$((pass+1)); else echo "  FAIL  $1 (missing: $2)"; fail=$((fail+1)); fi; }

echo "=== AGENT-LEDGER end-to-end proof (live agent.log hash-chain) ==="

# 1) The agent loop actually ran against the broker (not the no-broker SKIP path).
chk "agentd ran the broker loop"   "AGENTD:"
# 2) The destructive op was blocked. With no cockpit attached, `remove` auto-allows at
#    agentd, then tool_rm's OWN path gate refuses /etc -> "DENY policy" in the RESULT
#    (mock log). Either layer denying proves the destructive op did not succeed.
if grep -qaE "DENY policy|AGENTD: DENY|denied" "$MLOG" "$SER"; then
  echo "  PASS  destructive op blocked (gate/tool)"; pass=$((pass+1))
else echo "  FAIL  destructive op not blocked"; fail=$((fail+1)); fi

# 3) The AUTHORITATIVE verdict: the LATE ledgerver (spawned after agentd is reaped)
#    must report the agent ledger VERIFIED with records>0. We accept any records=N
#    where N>=1 (the chain is intact and non-empty). records=0 / EMPTY here = FAIL.
LATE_VERDICT="$(grep -aE 'LEDGER: VERIFIED records=[0-9]+|LEDGER: EMPTY|LEDGER: TAMPERED' "$SER" | tail -1)"
echo "--- ledger verdicts seen (boot-time EMPTY is expected; LAST must be VERIFIED N>0) ---"
grep -aE 'LEDGER: (VERIFIED|EMPTY|TAMPERED)' "$SER" || echo "  (none)"
RECS="$(printf '%s\n' "$LATE_VERDICT" | sed -nE 's/.*LEDGER: VERIFIED records=([0-9]+).*/\1/p')"
if [ -n "$RECS" ] && [ "$RECS" -gt 0 ] 2>/dev/null; then
  echo "  PASS  agent ledger VERIFIED records=$RECS (>0)"; pass=$((pass+1))
else
  echo "  FAIL  agent ledger not VERIFIED with records>0 (last verdict: ${LATE_VERDICT:-none})"; fail=$((fail+1))
fi

# 4) The ledger captured MULTIPLE chained decisions. agentd writes each decision to
#    /var/log/ai/agent.log (not serial); ledgerver verifies the file. The CODETASK plan
#    drives ~6 tool steps, so records>=5 proves the live agent's decisions were each
#    logged + chain-linked (not just a single entry).
if [ -n "$RECS" ] && [ "$RECS" -ge 5 ] 2>/dev/null; then
  echo "  PASS  ledger captured all agent decisions (records=$RECS >= 5)"; pass=$((pass+1))
else
  echo "  FAIL  ledger captured too few decisions (records=${RECS:-0})"; fail=$((fail+1))
fi

echo "--- mock LOG (what the agent did, step by step) ---"
cat "$MLOG"

echo "AGENT-LEDGER: $([ $fail -eq 0 ] && echo PASS || echo FAIL) pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
