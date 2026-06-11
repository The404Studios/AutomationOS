#!/bin/bash
# DESKTOP-SPLIT-0 verdict (revised FPS gate), standalone, over captured logs.
#
# FPS RULE (user-set, replacing the brittle max-vs-max form):
#   fps_within_tolerance = split_first9_median >= 90% of baseline_first9_median
# Median over the first 9 fps windows of each boot = matched boot-relative
# periods; the tolerance band absorbs host/QEMU jitter at the ~10fps cap
# (run-to-run baseline max varied 89..100 with an uncorrelated mid-soak dip --
# host-side noise, not the split). The claim is "the split does not materially
# hurt the desktop", NOT "FPS improves".
#
# Usage: _dsplit_verdict.sh [base.log] [soak.log]
set -u
SER_A=${1:-/tmp/dsplit_base.log}
SER_B=${2:-/tmp/dsplit_soak.log}

fps_first9_median() {
    grep -F '[COMP] fps window' "$1" | sed -nE 's/.*fps_x10=([0-9]+).*/\1/p' | \
    head -9 | sort -n | awk '{a[NR]=$1} END {if (NR==0) print 0; else print a[int((NR+1)/2)]}'
}
BASE_MED=$(fps_first9_median "$SER_A")
SPLIT_MED=$(fps_first9_median "$SER_B")
BASE_WINS=$(grep -cF '[COMP] fps window' "$SER_A" || true)
echo "baseline: first9_median fps_x10=$BASE_MED windows=$BASE_WINS"

echo "=== the split, observed ==="
grep -F '[DSPLIT]' "$SER_B" | head -16
grep -E "\[RUNMASK\] exit record: .*batchdemo" "$SER_B" | head -2

ALLOW_OK=0
grep -qE "\[DSPLIT\] '.*(batchdemo|bklstorm)' PID [0-9]+ -> BATCH allowlist, the seam chose cpu1" "$SER_B" && ALLOW_OK=1
COMP0_OK=0
grep -qE "\[DSPLIT\] observed: pid=[0-9]+ 'sbin/compositor' ran=0x1$" "$SER_B" && COMP0_OK=1
CPU1RAN_OK=0
grep -qE "\[DSPLIT\] observed: .* ran=0x[23]" "$SER_B" && CPU1RAN_OK=1
grep -qE "\[RUNMASK\] exit record: .* ran=0x2 " "$SER_B" && CPU1RAN_OK=1

SOAK_WINS=$(grep -cF '[COMP] fps window' "$SER_B" || true)
FPS_OK=$(awk -v s="$SPLIT_MED" -v b="$BASE_MED" 'BEGIN {print (b > 0 && s*10 >= b*9) ? 1 : 0}')
FPS_PCT=$(awk -v s="$SPLIT_MED" -v b="$BASE_MED" 'BEGIN {if (b>0) printf "%.1f", s*100/b; else print 0}')
SOAK_OK=0; [ "$SOAK_WINS" -ge 55 ] && SOAK_OK=1
echo "fps: split_first9_med=$SPLIT_MED baseline_first9_med=$BASE_MED ratio=${FPS_PCT}% (gate >=90%) soak_windows=$SOAK_WINS"

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
echo "walls: runmask=$RM_OK(viol=$NVIOL) tlb_neg=$NEG_OK bkl=$BKL_OK(storms=$ST_DONE warn=$NDEAD) ipiwake=$WK_OK ipilink=$IPI_OK tlbshoot=$TS_OK"
echo "soak: windows=$SOAK_WINS alive=$ALIVE sched_viol=$NSCHED tlb_viol=$NTLBV panic=$NPANIC"
echo "split: comp0=$COMP0_OK allow=$ALLOW_OK cpu1ran=$CPU1RAN_OK"

if [ "$COMP0_OK" = "1" ] && [ "$ALLOW_OK" = "1" ] && [ "$CPU1RAN_OK" = "1" ] && \
   [ "$FPS_OK" = "1" ] && [ "$RM_OK" = "1" ] && [ "$RM_CLEAN" = "1" ] && \
   [ "$NEG_OK" = "1" ] && [ "$BKL_OK" = "1" ] && [ "$WK_OK" = "1" ] && \
   [ "$IPI_OK" = "1" ] && [ "$TS_OK" = "1" ] && [ "$SOAK_OK" = "1" ] && \
   [ "$ALIVE" = "1" ] && [ "$NSCHED" = "0" ] && [ "$NTLBV" = "0" ] && [ "$NPANIC" = "0" ]; then
    echo "DESKTOPSPLIT: PASS cpu0_desktop=1 cpu1_batch=1 fps_within_tolerance=1 runmask=1 tlb_neg=1 bkl=1 soak=30m panic=0 invariant=0"
else
    echo "DESKTOPSPLIT: FAIL comp0=$COMP0_OK allow=$ALLOW_OK cpu1ran=$CPU1RAN_OK fps=$FPS_OK($SPLIT_MED vs $BASE_MED = ${FPS_PCT}%) runmask=$RM_OK/$RM_CLEAN(viol=$NVIOL) neg=$NEG_OK bkl=$BKL_OK wake=$WK_OK ipi=$IPI_OK ts=$TS_OK soak=$SOAK_OK($SOAK_WINS) alive=$ALIVE sched=$NSCHED tlb=$NTLBV panic=$NPANIC"
fi
