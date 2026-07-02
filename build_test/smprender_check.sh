#!/bin/bash
# smprender_check.sh -- the SMP-RENDER-0 proof vehicle.
# =============================================================================
# Multicore rendering, first brick: the compositor delegates the TOP band of
# present_diff()'s phase-1 back-vs-prev scan to a `renderworker` process that
# the DESKTOP-SPLIT seam routes to CPU1 (BATCH allowlist), over a SysV-SHM job
# page + SHM scene buffers. The compositor scans the bottom band itself, JOINS
# with an iteration-capped spin, and falls back to a solo scan if the worker
# misses the deadline -- correctness NEVER depends on the worker.
#
# GATES (keyed to real serial strings):
#   worker_ready   : "[RENDERWORKER] ready"      (attached job page + buffers)
#   selftest       : "[SMPRENDER] selftest scan_match=1 rounds=4 worker=1"
#                    (solo full scan vs split scan -> IDENTICAL dirty bbox on
#                    synthetic patterns, including rects spanning both bands)
#   seam_cpu1      : "[DSPLIT] 'sbin/renderworker' PID n .. seam chose cpu1"
#   worker_on_cpu1 : "[DSPLIT] observed: .. 'sbin/renderworker' ran=0x2"
#                    (health-monitor OBSERVED execution reality)
#   desktop_cpu0   : "'sbin/compositor' ran=0x1" and NO desktop proc ran=0x3
#   walls          : RUNMASK-CORE PASS (exactly 1 planted violation), IPILINK,
#                    TLBSHOOT, desktop alive, 0 kernel-context faults
#   byte-identity  : DEFAULT kernel.elf md5 unchanged (all kernel changes are
#                    #ifdef SMP_DSPLIT) AND the DEFAULT-flags compositor OBJECT
#                    is byte-identical to the one compiled from the COMMITTED
#                    HEAD source (all compositor changes #ifdef COMP_SMP_RENDER).
#                    comp.elf itself can't be md5-anchored: the About dialog
#                    embeds __DATE__/__TIME__ (compositor_m8.c:3640), so both
#                    objects are compiled with those macros PINNED.
#
# Kernel anchor recorded on the post-NET-GAPS default build (commit 2febb05):
#   kernel.elf 710d3cf7373ca02ac1ffbfed5645afe1
#
# Run: wsl -d Arch bash build_test/smprender_check.sh
set -u
ROOT=/mnt/c/Users/wilde/Desktop/Kernel
cd "$ROOT" || exit 9

SOAK_SECS="${SOAK_SECS:-150}"
DEF_KERN_MD5="710d3cf7373ca02ac1ffbfed5645afe1"
SER=/tmp/smpr_soak.log

echo "[smpr] (1/5) DEFAULT compositor object identity vs committed HEAD ..."
# Both versions are compiled through the SAME temp filename sequentially --
# a .o embeds an STT_FILE symbol with the source name, so different paths
# can never be byte-identical. __DATE__/__TIME__ are pinned (the About
# dialog embeds them).
CF_LINE=$(grep -m1 '^CF=' scripts/build_all.sh)
eval "$CF_LINE"
PIN='-Wno-builtin-macro-redefined -D__DATE__="X" -D__TIME__="X"'
IDENT_C=userspace/compositor/.smpr_ident.c
cp userspace/compositor/compositor_m8.c "$IDENT_C"
gcc $CF $PIN -c "$IDENT_C" -o /tmp/smpr_comp_work.o 2>/tmp/smpr_ci1.log
git show HEAD:userspace/compositor/compositor_m8.c > "$IDENT_C"
gcc $CF $PIN -c "$IDENT_C" -o /tmp/smpr_comp_head.o 2>/tmp/smpr_ci2.log
rm -f "$IDENT_C"
M_WORK=$(md5sum /tmp/smpr_comp_work.o | awk '{print $1}')
M_HEAD=$(md5sum /tmp/smpr_comp_head.o | awk '{print $1}')
COMP_IDENT=0; [ "$M_WORK" = "$M_HEAD" ] && COMP_IDENT=1
echo "[smpr] default-flags compositor object: work=$M_WORK head=$M_HEAD ident=$COMP_IDENT"

echo "[smpr] (2/5) SMP_RENDER=1 build_all (flagged compositor + renderworker in initrd) ..."
SMP_RENDER=1 bash scripts/build_all.sh > /tmp/smpr_ba.log 2>&1
grep -qE 'error:|undefined reference' /tmp/smpr_ba.log && { echo "[smpr] SMP_RENDER build_all ERRORS:"; grep -E 'error:|undefined reference' /tmp/smpr_ba.log | head -10; exit 1; }
grep -qF 'renderworker' iso/boot/initrd.img || { echo "[smpr] sbin/renderworker NOT in initrd"; exit 1; }

echo "[smpr] (3/5) SMP kernel (full stack + DSPLIT + THREAD_INHERIT) ..."
SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 SMP_BKL=1 SMP_BATCH=1 SMP_RUNMASK=1 SMP_DSPLIT=1 SMP_THREAD_INHERIT=1 \
    bash scripts/quick_build.sh > /tmp/smpr_qb.log 2>&1 || { echo "[smpr] SMP kernel build FAILED"; tail -20 /tmp/smpr_qb.log; exit 1; }
grep -qF 'Link OK -- no unresolved symbols' /tmp/smpr_qb.log || { echo "[smpr] SMP LINK gate FAILED"; exit 1; }

echo "[smpr] (4/5) boot (${SOAK_SECS}s, -smp 2) ..."
cp build/kernel-smp.elf iso/boot/kernel.elf
grub-mkrescue -o build/automationos-smprender.iso iso/ 2>/dev/null || { echo "grub fail"; exit 1; }
rm -f "$SER"
timeout "$SOAK_SECS" qemu-system-x86_64 -cdrom build/automationos-smprender.iso \
    -m 512 -smp 2 -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$SER" -display none -no-reboot 2>/dev/null
[ -s "$SER" ] || { echo "[smpr] ERROR: no serial captured"; exit 1; }

echo "=== smprender evidence ==="
grep -aE '\[SMPRENDER\]|\[RENDERWORKER\]|renderworker' "$SER" | head -20

echo "[smpr] (5/5) restore DEFAULT kernel -- byte-identity gate ..."
bash scripts/quick_build.sh > /tmp/smpr_qb_def.log 2>&1
cp build/kernel.elf iso/boot/kernel.elf
KERN_MD5=$(md5sum build/kernel.elf | awk '{print $1}')
echo "[smpr] default kernel md5=$KERN_MD5 (expect $DEF_KERN_MD5)"

# ---- gates ------------------------------------------------------------------
READY=0;  grep -qaF '[RENDERWORKER] ready' "$SER" && READY=1
SELF=0;   grep -qaF '[SMPRENDER] selftest scan_match=1 rounds=4 worker=1' "$SER" && SELF=1
SEAM=0;   grep -qaE "\[DSPLIT\] 'sbin/renderworker' PID [0-9]+ .*seam chose cpu1" "$SER" && SEAM=1
WCPU1=0;  grep -qaE "\[DSPLIT\] observed: pid=[0-9]+ 'sbin/renderworker' ran=0x2\b" "$SER" && WCPU1=1
DESK=0
if grep -qaE "\[DSPLIT\] observed: pid=[0-9]+ 'sbin/compositor' ran=0x1\b" "$SER"; then
    if ! grep -qaE "\[DSPLIT\] observed: pid=[0-9]+ 'sbin/(compositor|terminal|filemanager|netman|browser2|ide)' ran=0x3" "$SER"; then
        DESK=1
    fi
fi
RM_OK=0;  grep -qaF 'RUNMASK-CORE: PASS baseline_clean=1 forced_crosscpu_detected=1 restored_clean=1' "$SER" && RM_OK=1
NVIOL=$(grep -acF '[RUNMASK] VIOLATION' "$SER" || true)
IPI_OK=0; grep -qaF 'IPILINK: PASS' "$SER" && IPI_OK=1
TS_OK=0;  grep -qaE 'TLBSHOOT: PASS kernel_flush=1' "$SER" && TS_OK=1
ALIVE=1
grep -qaF 'entering frame loop' "$SER" || ALIVE=0
grep -qaF '[INIT] Compositor died' "$SER" && ALIVE=0
NKERN=$(grep -acE 'TRIPLE FAULT|KERNEL PANIC|Privilege level: (Supervisor|Kernel)' "$SER" || true)
CRIT=$(grep -acE "Terminating faulting process '(sbin/renderworker|sbin/compositor|/sbin/init)" "$SER" || true)
NWIN=$(grep -acF '[COMP] fps window' "$SER" || true)

echo ""
echo "smpr:  ready=$READY selftest=$SELF seam_cpu1=$SEAM worker_ran_cpu1=$WCPU1 desktop_cpu0=$DESK"
echo "walls: runmask=$RM_OK(viol=$NVIOL) ipilink=$IPI_OK tlbshoot=$TS_OK alive=$ALIVE(fpswin=$NWIN) kern_fault=$NKERN crit_killed=$CRIT"
echo "ident: kernel=$KERN_MD5 comp_obj_ident=$COMP_IDENT"

if [ "$READY" = "1" ] && [ "$SELF" = "1" ] && [ "$SEAM" = "1" ] && [ "$WCPU1" = "1" ] && \
   [ "$DESK" = "1" ] && [ "$RM_OK" = "1" ] && [ "$NVIOL" = "1" ] && [ "$IPI_OK" = "1" ] && \
   [ "$TS_OK" = "1" ] && [ "$ALIVE" = "1" ] && [ "$NKERN" = "0" ] && [ "$CRIT" = "0" ] && \
   [ "$NWIN" -ge 2 ] && [ "$KERN_MD5" = "$DEF_KERN_MD5" ] && [ "$COMP_IDENT" = "1" ]; then
    echo ""
    echo "SMP-RENDER-0 PASS worker_cpu1=1 scan_match=1 desktop_cpu0=1 default_byte_identical=1"
    exit 0
else
    echo ""
    echo "SMP-RENDER-0 FAIL -- see $SER"
    tail -25 "$SER"
    exit 1
fi
