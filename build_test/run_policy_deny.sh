#!/bin/bash
# POLICY-LOAD proof: agentd's gate is now driven by /etc/ai/policy.json (tighten-only).
# Temporarily add "read_file" to the deny array, rebuild (build_all seeds policy.json into
# the initrd), boot with the DEFAULT mock plan (list_dir/stat/read_file), and assert agentd
# DENIES read_file -- proving that editing the policy file changes the LIVE agent gate (it
# was previously hardcoded/decorative). Then restore policy.json + rebuild. Self-contained.
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
POL=etc/ai/policy.json
SER=build_test/policy_deny_ser.log
rm -f "$SER"
cp "$POL" /tmp/policy.json.bak
restore(){ cp /tmp/policy.json.bak "$POL"; echo "[run] restoring policy + rebuilding..."; bash scripts/build_all.sh > /tmp/pd_restore.log 2>&1; }
trap restore EXIT

# Inject "read_file" as the first deny entry (a tool normally classed auto).
python3 - <<'PY'
p="etc/ai/policy.json"; s=open(p).read()
s=s.replace('"deny": [\n', '"deny": [\n    "read_file",\n', 1)
open(p,"w").write(s)
PY
echo "[run] rebuilding with read_file DENIED in policy.json..."
bash scripts/build_all.sh > /tmp/pd_build.log 2>&1 || { echo "POLICY-LOAD: FAIL build"; exit 1; }

python3 scripts/nemotron_mock.py > /tmp/pd_mock.log 2>&1 &   # DEFAULT plan: list_dir/stat/read_file
MOCK=$!
sleep 1
echo "[run] booting (95s)..."
timeout 95 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null
kill "$MOCK" 2>/dev/null

echo "=== POLICY-LOAD proof (tighten-only deny via policy.json) ==="
grep -aE "AGENTD: (DENY|TOOL) " "$SER" | head
if grep -qaF "AGENTD: DENY tool=read_file" "$SER"; then
  echo "  PASS  policy DENY took effect -- read_file refused by the LIVE gate"
  echo "POLICY-LOAD: PASS"; exit 0
else
  echo "  FAIL  read_file was NOT denied (policy not loaded into the live gate?)"
  echo "POLICY-LOAD: FAIL"; exit 1
fi
