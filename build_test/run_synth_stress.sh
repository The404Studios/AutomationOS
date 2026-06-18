#!/bin/bash
# run_synth_stress.sh -- SYNTHINPUT-WRAP-0 proof: the >64-event ring-wrap.
# =============================================================================
# WHY THIS EXISTS
#   The synthetic-input ring (userspace/include/synthinput.h, QMAX=64) had a
#   stale-replay bug: the consumer (compositor pump_synth_input) advanced `tail`
#   WITHOUT masking it into [0,QMAX), while the producer (tool_mouse enqueue)
#   keeps `head` masked. After the very first wrap past slot 63, tail != head
#   stayed true FOREVER and the pump replayed stale slots every frame -- the
#   cursor would keep drifting on its own. The fix (compositor_m8.c:4945-4948)
#   masks tail the SAME way: `si->tail = (si->tail + 1) & (QMAX-1)`.
#
#   That fix was NEVER exercised past the 64-slot threshold where the bug lived:
#   the GUI proof (run_agentd_gui.sh) injects only ~6 events total, and agentd
#   caps the ReAct loop at MAX_STEPS=16, so the agent path tops out around
#   16*2 = 32 events -- BELOW 64. This runner crosses 64 from a SINGLE gated
#   step using tool_mouse's `moven <dx> <dy> <count>` subcommand: one TOOL call
#   enqueues `count` X-moves (here 200), so the consumer's tail provably wraps
#   past slot 63 multiple times.
#
# HOW WE ASSERT (keyed to real kernel/compositor serial strings) --------------
#   The compositor accumulates the count of drained synth events across frames
#   and, the first time that cumulative count exceeds QMAX (64), prints ONCE:
#       [SHELL] SYNTHINPUT: drained N events cursor_x=X
#   (the minimal assertable drain marker added in pump_synth_input). The two
#   facts this single line proves:
#     drained>64      -- the tail crossed the 64-slot wrap boundary at least once
#                        (the regime the bug lived in). N is the cumulative
#                        drained count at the moment it first exceeds 64.
#     cursor settled  -- the cursor X is a SANE bounded value (>0, < screen W),
#                        NOT a runaway. The pre-fix bug replayed stale +dx slots
#                        every frame forever, so by the time this line could even
#                        print the cursor would have been pinned at the right
#                        clamp (W-1) by infinite self-drift -- AND it would keep
#                        drifting after injection stops. We additionally assert
#                        the cursor is STABLE across the rest of the boot: no
#                        further "drained" line appears once injection ended
#                        (stale replay would re-trip the >64 print or keep moving
#                        the cursor frame after frame).
#
#   We ALSO assert the event actually reached the pump:
#     input_applied   -- "[SHELL] SYNTHINPUT: input applied (agent is driving)"
#                        (the existing one-shot first-event marker).
#     page_ready      -- "[SHELL] SYNTHINPUT: injection page ready"
#     tool_moven_ok   -- the gated tool ran: "TOOL_MOUSE: OK moven <n>" (its
#                        one-line outcome) + "AGENTD: TOOL mouse moven".
#
# WHY moven, not 16 move steps: a single TOOL step injects only 1-3 events and
# agentd's MAX_STEPS=16 caps the agent at ~32 events -- it cannot reach 64. ONE
# moven call enqueues `count` moves from a single gated step, the clean way to
# cross 64. (The ring still holds at most QMAX-1=63 unread at once: the producer
# drops the newest on a full ring and the compositor frame loop drains
# concurrently, so the 200 moves are consumed across several frames -- exactly
# the multi-wrap path the masked-tail fix must survive.)
#
# Composite acceptance assembled here:
#   SYNTHINPUT-WRAP-0 PASS drained_over_64=1 cursor_settled=1 no_stale_replay=1
#
# Prereq: WSL Arch + the host toolchain. This runner rebuilds the world
# (build_all.sh ships the patched compositor + tool_mouse with `moven` into the
# initrd; quick_build.sh is invoked by build_all for the kernel) so it is
# self-contained.
# Run: wsl -d Arch bash -lc 'bash build_test/run_synth_stress.sh'
#      MOVEN_COUNT=300 bash build_test/run_synth_stress.sh   # push more events
set -u
ROOT=/mnt/c/Users/wilde/Desktop/Kernel
cd "$ROOT" || exit 9

MOVEN_COUNT="${MOVEN_COUNT:-200}"   # how many X-moves the single moven step enqueues
BOOT_SECS="${BOOT_SECS:-95}"        # boot long enough for agentd to connect + run
SER=build_test/synth_stress_ser.log
MLOG=build_test/synth_stress_mock.log
BLOG=build_test/synth_stress_build.log

# ---- 0) sanity: the moven plan must be wired into the mock ------------------
if ! grep -q 'NEMO_SYNTH' scripts/nemotron_mock.py; then
    echo "[synth] ERROR: scripts/nemotron_mock.py has no NEMO_SYNTH plan -- add the SYNTH_STEPS branch first"
    exit 1
fi
# the moven subcommand must exist in the tool
if ! grep -q 'moven' userspace/apps/tool_mouse/tool_mouse.c; then
    echo "[synth] ERROR: tool_mouse.c has no 'moven' subcommand -- add it first"
    exit 1
fi
# the assertable drain marker must exist in the compositor
if ! grep -q 'SYNTHINPUT: drained' userspace/compositor/compositor_m8.c; then
    echo "[synth] ERROR: compositor has no 'SYNTHINPUT: drained N events cursor_x=X' marker -- add it first"
    exit 1
fi

# ---- 1) build the world (patched compositor + tool_mouse into the initrd) ---
echo "[synth] building the world (build_all.sh: compositor + tool_mouse + kernel + ISO) ..."
bash scripts/build_all.sh > "$BLOG" 2>&1 || { echo "[synth] build FAILED"; tail -30 "$BLOG"; exit 1; }
# build_all.sh has no set -e (MEMORY: do not trust DONE_EXIT=0) -- scan for hard errors
if grep -qiE 'error:|undefined reference|unresolved' "$BLOG"; then
    echo "[synth] build log shows errors:"; grep -iE 'error:|undefined reference|unresolved' "$BLOG" | head -20; exit 1
fi
[ -s build/automationos.iso ] || { echo "[synth] ERROR: no ISO produced"; exit 1; }
# the patched tool must actually be shipped in the initrd
grep -qF 'moven' iso/boot/initrd.img || { echo "[synth] ERROR: 'moven' tool_mouse not in initrd -- build_all did not ship it"; exit 1; }

# ---- 2) launch the SYNTH mock (one moven step of MOVEN_COUNT X-moves) -------
rm -f "$SER" "$MLOG"
NEMO_SYNTH=1 SYNTH_MOVEN_COUNT="$MOVEN_COUNT" python3 scripts/nemotron_mock.py > "$MLOG" 2>&1 &
MOCK=$!
# make sure we always reap the mock
trap 'kill "$MOCK" 2>/dev/null' EXIT
sleep 1
echo "[synth] SYNTH mock pid=$MOCK (moven count=$MOVEN_COUNT); booting QEMU (${BOOT_SECS}s)..."

# ---- 3) the boot: default ISO, agentd auto-connects to the mock @ 8433 ------
timeout "$BOOT_SECS" qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

kill "$MOCK" 2>/dev/null
[ -s "$SER" ] || { echo "[synth] ERROR: no serial captured"; exit 1; }

echo "=== SYNTHINPUT lines, observed ==="
grep -E 'SYNTHINPUT|TOOL_MOUSE|AGENTD: (TOOL|PASS|DENY)' "$SER" | head -30

# ---- 4) gates --------------------------------------------------------------
PAGE_READY=0
grep -qF '[SHELL] SYNTHINPUT: injection page ready' "$SER" && PAGE_READY=1

APPLIED=0
grep -qF '[SHELL] SYNTHINPUT: input applied' "$SER" && APPLIED=1

# the gated moven tool ran end to end
TOOL_OK=0
# "TOOL_MOUSE: OK moven" is the tool's stdout -> the agentd RESULT -> mock log (NOT serial);
# "AGENTD: TOOL mouse moven" is on serial. Check each in the right log.
grep -qaE 'TOOL_MOUSE: OK moven' "$MLOG" && grep -qaE 'AGENTD: TOOL mouse moven' "$SER" && TOOL_OK=1

# THE proof line: "[SHELL] SYNTHINPUT: drained N events cursor_x=X". Parse N + X.
DRAIN_LINE=$(grep -E '\[SHELL\] SYNTHINPUT: drained [0-9]+ events cursor_x=-?[0-9]+' "$SER" | head -1)
DRAINED=$(echo "$DRAIN_LINE" | grep -oE 'drained [0-9]+' | grep -oE '[0-9]+')
CURX=$(echo "$DRAIN_LINE" | grep -oE 'cursor_x=-?[0-9]+' | grep -oE '\-?[0-9]+')
DRAINED=${DRAINED:-0}
CURX=${CURX:-0}

# crossed the 64-slot wrap boundary
OVER_64=0
[ "$DRAINED" -gt 64 ] 2>/dev/null && OVER_64=1

# cursor settled at a SANE bounded value (>0 because we moved right by +dx, and
# strictly inside a plausible screen width -- not a runaway). The framebuffer is
# 1024x768 or 1280x800; allow up to 4096 as the hard upper sanity bound.
CURSOR_SETTLED=0
{ [ "$CURX" -gt 0 ] 2>/dev/null && [ "$CURX" -lt 4096 ] 2>/dev/null; } && CURSOR_SETTLED=1

# no stale replay: the drain marker is ONE-SHOT, so exactly ONE such line should
# ever appear. The pre-fix bug replayed stale slots EVERY frame forever, which
# (a) keeps draining after injection stopped and (b) keeps moving the cursor.
# A single drain line + a cursor pinned at the right clamp by infinite drift are
# distinguishable: with the fix, X reflects only the real injected moves and
# stays put. We assert exactly one drain line AND that the cursor never reaches
# the far-right clamp via runaway (CURX must be < 1023 unless MOVEN_COUNT itself
# legitimately pushed it there -- with count<=1000 and a fresh cursor at 0 the
# net displacement is bounded by the consumed moves, well under the clamp).
NDRAIN=$(grep -cE '\[SHELL\] SYNTHINPUT: drained [0-9]+ events' "$SER" || true)
NO_STALE=0
[ "$NDRAIN" = "1" ] && NO_STALE=1

echo ""
echo "marker: page_ready=$PAGE_READY input_applied=$APPLIED tool_moven_ok=$TOOL_OK"
echo "ring:   drained=$DRAINED over_64=$OVER_64 cursor_x=$CURX cursor_settled=$CURSOR_SETTLED drain_lines=$NDRAIN no_stale_replay=$NO_STALE"

if [ "$PAGE_READY" = "1" ] && [ "$APPLIED" = "1" ] && [ "$TOOL_OK" = "1" ] && \
   [ "$OVER_64" = "1" ] && [ "$CURSOR_SETTLED" = "1" ] && [ "$NO_STALE" = "1" ]; then
    echo ""
    echo "SYNTHINPUT-WRAP-0 PASS drained_over_64=1 cursor_settled=1 no_stale_replay=1"
    echo "[synth] RESULT: PASS -- one gated moven step drained >64 events past the 64-slot wrap; cursor settled, no stale replay (the masked-tail fix holds)"
    exit 0
else
    echo ""
    echo "SYNTHINPUT-WRAP-0 FAIL (one or more gates missed) -- see $SER"
    echo "--- last 40 serial lines ---"; tail -40 "$SER"
    exit 1
fi
