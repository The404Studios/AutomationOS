#!/bin/bash
# AGENTCOCKPIT-0 proof: the agent COCKPIT (GUI) drives the headless agent (agentd)
# through the well-known SHM seam (agentcockpit.h, KEY 0x41434B50) -- with NO human
# clicking. The cockpit is launched with --proof, so it auto-posts a goal
# ("List /etc and read /etc/toolset0.txt"), bumps goal_seq, and auto-clicks RUN; the
# scripted Nemotron mock plays the model so the whole GOAL/TOOL/RESULT/DONE loop runs
# deterministically over the QEMU slirp seam. We assert, end to end on a real boot:
#   - the cockpit posted the goal + auto-ran    ([COCKPIT] proof: posted goal seq=1)
#   - agentd picked up the goal and ran a tool  (AGENTD: GOAL sent + AGENTD: TOOL list_dir)
#   - the gated ReAct loop completed            (AGENTD: PASS loop_completed)
#   - the cockpit OBSERVED agentd's steps via the SHM status fields, proving the
#     status rail flows back (the [COCKPIT] step N: <tool> lines -- see NOTE below)
#
# This proves the cockpit<->agentd seam (post a goal -> agent runs -> status returns)
# without a human, exactly the way run_agentd_gui.sh proves SYNTHINPUT headlessly.
#
# PREREQS:
#   1. sbin/cockpit must be built + packaged into the initrd (scripts/build_all.sh,
#      same pattern as agentd at lines 192 / 676).
#   2. init must auto-spawn the cockpit with --proof, gated behind COCKPIT_PROOF so
#      it ONLY auto-runs under this test build, never on a normal boot. Build with:
#        COCKPIT_PROOF=1 bash scripts/build_all.sh
#      (run_cockpit.sh's design doc spells out the exact init/main.c spawn line.)
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

SER=build_test/cockpit_ser.log
MLOG=build_test/cockpit_mock.log
rm -f "$SER" "$MLOG"

# The DEFAULT mock plan is the read-only list_dir/stat/read_file sequence -- exactly
# the goal --proof posts ("List /etc and read /etc/toolset0.txt"). No NEMO_* env set.
python3 scripts/nemotron_mock.py > "$MLOG" 2>&1 &
MOCK=$!
sleep 1
echo "[run] cockpit mock pid=$MOCK; booting QEMU (95s) with slirp net..."

# Headless boot, slirp e1000 (so agentd can reach the mock at 10.0.2.2:8433),
# serial captured to a file. Mirrors run_agentd_gui.sh exactly.
timeout 95 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

kill "$MOCK" 2>/dev/null

echo "=== MOCK LOG ==="
cat "$MLOG"

# ---- verdict: grep the captured serial for the seam markers ----
echo "=== VERDICT ==="
PASS=1

check() {  # check <label> <regex>
    if grep -qE "$2" "$SER"; then
        echo "  PASS  $1"
    else
        echo "  FAIL  $1   (missing: /$2/)"
        PASS=0
    fi
}

# (1) the cockpit posted the goal + auto-clicked RUN (the --proof path fired)
check "cockpit posted goal"     '\[COCKPIT\] proof: posted goal seq=1'
# (2) agentd picked up the goal off the seam and opened the broker loop
check "agentd got goal"         'AGENTD: GOAL sent'
# (3) agentd ran the first whitelisted tool (the model's list_dir step)
check "agentd ran list_dir"     'AGENTD: TOOL list_dir'
# (4) the bounded gated ReAct loop completed cleanly
check "agentd loop completed"   'AGENTD: PASS loop_completed'

# (5) OPTIONAL status-rail-back proof: if the cockpit logs each new step it reads
#     out of the SHM status fields (recommended: print "[COCKPIT] step N: <tool>"
#     when step/run_seq advance), assert it saw agentd's steps. This is the half of
#     the seam that proves STATUS flows cockpit<-agentd (the markers above only prove
#     CONTROL flows cockpit->agentd). It is a soft check: absent (older cockpit) =>
#     WARN, not FAIL, so the core seam proof still stands on (1)-(4).
if grep -qE '\[COCKPIT\] step [0-9]+: ' "$SER"; then
    echo "  PASS  cockpit observed steps via SHM status"
    grep -E '\[COCKPIT\] step [0-9]+: ' "$SER" | sed 's/^/        /'
    # If the cockpit logs steps, require it saw the list_dir step specifically.
    check "cockpit saw list_dir step" '\[COCKPIT\] step [0-9]+: list_dir'
else
    echo "  WARN  cockpit step-observation logging absent (recommend [COCKPIT] step N: <tool>)"
fi

echo "==============="
if [ "$PASS" = "1" ]; then
    echo "COCKPIT: PASS  cockpit<->agentd seam proven headlessly (--proof, no human)"
    exit 0
else
    echo "COCKPIT: FAIL  see missing markers above; full serial in $SER"
    exit 1
fi
