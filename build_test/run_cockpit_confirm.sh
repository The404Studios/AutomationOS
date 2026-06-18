#!/bin/bash
# CONFIRM-gate END-TO-END proof (the keystone the audit found missing). The cockpit (--proof)
# drives a CONFIRM-class plan and auto-ALLOWs file ops / auto-DENIES process spawns, proving the
# human-in-the-loop gate fires BOTH ways -- plus pre_snapshot + rollback actually restore content.
# (A no-cockpit agentd also runs at boot and auto-allows; the asserts below target ONLY the
# cockpit-attached agentd's markers, which a no-cockpit run cannot produce.)
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
SER=build_test/cockpit_confirm_ser.log
MLOG=build_test/cockpit_confirm_mock.log
rm -f "$SER" "$MLOG"
NEMO_CONFIRM=1 python3 scripts/nemotron_mock.py > "$MLOG" 2>&1 &
MOCK=$!
sleep 1
echo "[run] CONFIRM mock pid=$MOCK; booting QEMU (110s)..."
timeout 110 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null
kill "$MOCK" 2>/dev/null

pass=0; fail=0
chk(){ if grep -qaF "$2" "$SER"; then echo "  PASS  $1"; pass=$((pass+1)); else echo "  FAIL  $1 (missing: $2)"; fail=$((fail+1)); fi; }
echo "=== CONFIRM-gate end-to-end proof ==="
chk "cockpit posted goal"          "[COCKPIT] proof: posted goal seq=1"
chk "remove parked for confirm"    "AGENTD: CONFIRM-WAIT tool=remove"
chk "cockpit ALLOWed remove"       "[COCKPIT] auto-confirm ALLOW remove"
chk "agentd ran remove on allow"   "AGENTD: CONFIRM-ALLOW tool=remove"
chk "spawn parked for confirm"     "AGENTD: CONFIRM-WAIT tool=spawn"
chk "cockpit DENIED spawn"         "[COCKPIT] auto-confirm DENY spawn"
chk "agentd blocked spawn on deny" "AGENTD: CONFIRM-DENY tool=spawn"
chk "rollback ALLOWed"             "AGENTD: CONFIRM-ALLOW tool=rollback"
# tool_rollback's stdout flows to the agentd RESULT (mock log), not the serial:
if grep -qaF "TOOL_ROLLBACK: OK" "$MLOG"; then echo "  PASS  rollback restored the file"; pass=$((pass+1)); else echo "  FAIL  rollback restored the file"; fail=$((fail+1)); fi
echo "--- mock RESULTs (the post-rollback read should be v1) ---"
grep -aE "RESULT" "$MLOG" | tail -4
if grep -qaE "RESULT v1" "$MLOG"; then echo "  PASS  rollback content restored (read=v1)"; pass=$((pass+1)); else echo "  FAIL  rollback content (read != v1)"; fail=$((fail+1)); fi
echo "COCKPIT-CONFIRM: $([ $fail -eq 0 ] && echo PASS || echo FAIL) pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
