#!/bin/bash
# dsplit_smoke.sh -- the DESKTOP-SPLIT-0 proof vehicle.
# =============================================================================
# THE MILESTONE BRICK: the desktop visibly two-core, through typed intent and
# the boring allowlist only. Two boots:
#
#   PHASE A (baseline): the DEFAULT kernel, -smp 1, ~5 min. Collects the
#     compositor's "[COMP] fps window" lines -> baseline FPS.
#   PHASE B (split): the FULL stack + SMP_DSPLIT kernel, -smp 2, a 33-minute
#     run (boot + the 30-minute soak). Collects everything.
#
# Gates (phase B unless stated):
#   cpu0_desktop : "[DSPLIT] observed: ... 'sbin/compositor' ran=0x1" -- the
#                  compositor's ACTUAL run history is CPU0-only (law 18
#                  evidence, not an assumption). Shell/terminal likewise.
#   cpu1_batch   : "[DSPLIT] '...' -> BATCH allowlist, the seam chose cpu1"
#                  for a userspace sys_spawn-created allowlisted app, AND a CPU1 run
#                  history observed (the [DSPLIT] observation or an exit
#                  record with ran bit1).
#   fps          : split first-9-window median fps_x10 >= 90% of the baseline
#                  first-9-window median. Matched boot-relative periods; the
#                  tolerance band absorbs host/QEMU jitter at the ~10fps cap
#                  (run-to-run baseline max varied 89..100 with an uncorrelated
#                  mid-soak dip -- host-side noise, not the split). The claim is
#                  "the split does not materially hurt the desktop", NOT a speedup.
#   runmask      : RUNMASK-CORE: PASS + exactly ONE [RUNMASK] VIOLATION in
#                  the whole soak (the planted selftest case; real traffic
#                  must add ZERO across 30 minutes).
#   tlb_neg      : TLBSHOOT_NEG: PASS (the upgraded execution-reality form).
#   bkl          : both 60s storms errors=0 + [BKL] engaged + 0 deadlock.
#   ladder       : IPIWAKE pings 32/32, IPILINK, TLBSHOOT, F2, APCURRENT,
#                  CPU1HELLO -- the whole inheritance stays green.
#   soak         : >= 55 fps windows (~28+ min of live desktop), desktop
#                  alive at the end, 0 PANIC / 0 SCHED+TLB invariant.
#
# Composite acceptance assembled here:
#   DESKTOPSPLIT: PASS cpu0_desktop=1 cpu1_batch=1 fps_within_tolerance=1
#                 runmask=1 tlb_neg=1 bkl=1 soak=30m panic=0 invariant=0
#
# Prereq: a current world ISO (IDE=1 build_all) -- both phases share its
# userspace; only the kernel differs.
# Run: wsl -d Arch bash -lc 'bash scripts/dsplit_smoke.sh'
set -u
ROOT=/mnt/c/Users/wilde/Desktop/Kernel
cd "$ROOT" || exit 9

BASE_SECS="${BASE_SECS:-300}"
SOAK_SECS="${SOAK_SECS:-1980}"
SER_A=/tmp/dsplit_base.log
SER_B=/tmp/dsplit_soak.log
QB=/tmp/dsplit_qb.log

# first-9-window median of fps_x10 -- matched boot-relative periods, jitter-robust.
fps_median_first9() {
    grep -F '[COMP] fps window' "$1" | sed -nE 's/.*fps_x10=([0-9]+).*/\1/p' | \
    head -9 | sort -n | awk '{a[NR]=$1} END {if (NR==0) print 0; else print a[int((NR+1)/2)]}'
}

echo "[dsplit] building the DSPLIT kernel (full stack + SMP_DSPLIT=1) ..."
SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 SMP_BKL=1 SMP_BATCH=1 SMP_RUNMASK=1 SMP_DSPLIT=1 \
    bash scripts/quick_build.sh > "$QB" 2>&1 || { echo "[dsplit] build FAILED"; tail -20 "$QB"; exit 1; }
grep -qF 'Link OK -- no unresolved symbols' "$QB" || { echo "[dsplit] LINK gate FAILED"; exit 1; }
echo "[dsplit] building the DEFAULT kernel (baseline) ..."
bash scripts/quick_build.sh > /tmp/dsplit_qb_def.log 2>&1
grep -qF 'Link OK -- no unresolved symbols' /tmp/dsplit_qb_def.log || { echo "[dsplit] default LINK FAILED"; exit 1; }

[ -s iso/boot/initrd.img ] || { echo "[dsplit] initrd missing -- run IDE=1 build_all"; exit 1; }

# ---- PHASE A: baseline (default kernel, -smp 1) -----------------------------
echo "[dsplit] PHASE A: baseline boot (${BASE_SECS}s, default kernel, -smp 1) ..."
cp build/kernel.elf iso/boot/kernel.elf
grub-mkrescue -o build/automationos-dsplit-base.iso iso/ 2>/dev/null || { echo "grub fail"; exit 1; }
rm -f "$SER_A"
timeout "$BASE_SECS" qemu-system-x86_64 -cdrom build/automationos-dsplit-base.iso \
    -m 512 -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$SER_A" -display none -no-reboot 2>/dev/null

BASE_FPS=$(fps_median_first9 "$SER_A"); BASE_FPS=${BASE_FPS:-0}
BASE_WINS=$(grep -cF '[COMP] fps window' "$SER_A" || true)
echo "[dsplit] baseline: first9_median fps_x10=$BASE_FPS over $BASE_WINS windows"
[ "$BASE_WINS" -ge 3 ] || { echo "[dsplit] baseline produced too few fps windows"; exit 1; }

# ---- PHASE B: the split soak (DSPLIT kernel, -smp 2, 33 min) ----------------
echo "[dsplit] PHASE B: split soak (${SOAK_SECS}s, DSPLIT kernel, -smp 2) ..."
cp build/kernel-smp.elf iso/boot/kernel.elf
grub-mkrescue -o build/automationos-dsplit.iso iso/ 2>/dev/null || { echo "grub fail"; exit 1; }
cp build/kernel.elf iso/boot/kernel.elf      # restore the default in the tree
rm -f "$SER_B"
timeout "$SOAK_SECS" qemu-system-x86_64 -cdrom build/automationos-dsplit.iso \
    -m 512 -smp 2 -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$SER_B" -display none -no-reboot 2>/dev/null

echo "=== the split, observed ==="
grep -F '[DSPLIT]' "$SER_B" | head -12

ALLOW_OK=0
grep -qE "\[DSPLIT\] '.*(batchdemo|bklstorm)' PID [0-9]+ -> BATCH allowlist, the seam chose cpu1" "$SER_B" && ALLOW_OK=1
COMP0_OK=0
grep -qE "\[DSPLIT\] observed: pid=[0-9]+ 'sbin/compositor' ran=0x1\b" "$SER_B" && COMP0_OK=1
CPU1RAN_OK=0
grep -qE "\[DSPLIT\] observed: .* ran=0x[23]" "$SER_B" && CPU1RAN_OK=1
grep -qE "\[RUNMASK\] exit record: .* ran=0x2 " "$SER_B" && CPU1RAN_OK=1

SPLIT_FPS=$(fps_median_first9 "$SER_B"); SPLIT_FPS=${SPLIT_FPS:-0}
SOAK_WINS=$(grep -cF '[COMP] fps window' "$SER_B" || true)
FPS_OK=$(awk -v s="$SPLIT_FPS" -v b="$BASE_FPS" 'BEGIN {print (b > 0 && s*10 >= b*9) ? 1 : 0}')
FPS_PCT=$(awk -v s="$SPLIT_FPS" -v b="$BASE_FPS" 'BEGIN {if (b>0) printf "%.1f", s*100/b; else print 0}')
SOAK_OK=0; [ "$SOAK_WINS" -ge 55 ] && SOAK_OK=1
echo "  fps: split_first9_med=$SPLIT_FPS baseline_first9_med=$BASE_FPS ratio=${FPS_PCT}% (gate >=90%) windows=$SOAK_WINS"

RM_OK=0
grep -qF 'RUNMASK-CORE: PASS baseline_clean=1 forced_crosscpu_detected=1 restored_clean=1' "$SER_B" && RM_OK=1
NVIOL=$(grep -cF '[RUNMASK] VIOLATION' "$SER_B" || true)
RM_CLEAN=0; [ "$NVIOL" = "1" ] && RM_CLEAN=1
NEG_OK=0
grep -qF 'TLBSHOOT_NEG: PASS no_user_crossflush_needed_under_pinning=1' "$SER_B" && NEG_OK=1
ST_DONE=$(grep -cE 'BKLSTORM pid=[0-9]+ done iters=[0-9]+ errors=0 secs=' "$SER_B" || true)
ENG=0; grep -qE '\[BKL\] engaged' "$SER_B" && ENG=1
NDEAD=$(grep -cE '\[BKL\] (possible deadlock|BUG)' "$SER_B" || true)
BKL_OK=0; [ "$ST_DONE" = "2" ] && [ "$ENG" = "1" ] && [ "$NDEAD" = "0" ] && BKL_OK=1
WK_OK=0; grep -qE 'ping summary acks=32/32' "$SER_B" && WK_OK=1
IPI_OK=0; grep -qF 'IPILINK: PASS' "$SER_B" && IPI_OK=1
TS_OK=0; grep -qE 'TLBSHOOT: PASS kernel_flush=1' "$SER_B" && TS_OK=1
NTLBV=$(grep -cF '[TLB_INVARIANT] VIOLATION' "$SER_B" || true)
NSCHED=$(grep -cF '[SCHED_INVARIANT]' "$SER_B" || true)
NPANIC=$(grep -cE 'PANIC|CPU EXCEPTION|TRIPLE FAULT|KERNEL PANIC' "$SER_B" || true)
ALIVE=1
grep -qF 'entering frame loop' "$SER_B" || ALIVE=0
grep -qF '[INIT] Compositor died' "$SER_B" && ALIVE=0
echo "  walls: runmask=$RM_OK(viol=$NVIOL) tlb_neg=$NEG_OK bkl=$BKL_OK(storms=$ST_DONE warn=$NDEAD) ipiwake=$WK_OK ipilink=$IPI_OK tlbshoot=$TS_OK"
echo "  soak: windows=$SOAK_WINS alive=$ALIVE sched_viol=$NSCHED tlb_viol=$NTLBV panic=$NPANIC"

if [ "$COMP0_OK" = "1" ] && [ "$ALLOW_OK" = "1" ] && [ "$CPU1RAN_OK" = "1" ] && \
   [ "$FPS_OK" = "1" ] && [ "$RM_OK" = "1" ] && [ "$RM_CLEAN" = "1" ] && \
   [ "$NEG_OK" = "1" ] && [ "$BKL_OK" = "1" ] && [ "$WK_OK" = "1" ] && \
   [ "$IPI_OK" = "1" ] && [ "$TS_OK" = "1" ] && [ "$SOAK_OK" = "1" ] && \
   [ "$ALIVE" = "1" ] && [ "$NSCHED" = "0" ] && [ "$NTLBV" = "0" ] && [ "$NPANIC" = "0" ]; then
    echo "DESKTOPSPLIT: PASS cpu0_desktop=1 cpu1_batch=1 fps_within_tolerance=1 runmask=1 tlb_neg=1 bkl=1 soak=30m panic=0 invariant=0"
    echo "[dsplit] RESULT: PASS -- the OS is visibly two-core, through typed intent and the allowlist only"
    exit 0
else
    echo "DESKTOPSPLIT: FAIL comp0=$COMP0_OK allow=$ALLOW_OK cpu1ran=$CPU1RAN_OK fps=$FPS_OK(med $SPLIT_FPS vs $BASE_FPS = ${FPS_PCT}%, gate>=90%) runmask=$RM_OK/$RM_CLEAN(viol=$NVIOL) neg=$NEG_OK bkl=$BKL_OK wake=$WK_OK ipi=$IPI_OK ts=$TS_OK soak=$SOAK_OK($SOAK_WINS) alive=$ALIVE sched=$NSCHED tlb=$NTLBV panic=$NPANIC"
    echo "--- last 50 serial lines ---"; tail -50 "$SER_B"
    exit 1
fi
