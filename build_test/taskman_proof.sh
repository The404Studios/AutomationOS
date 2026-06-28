#!/bin/bash
# TASKMAN proof: the Task Manager is REAL -- it reads the live kernel process
# table via SYS_PROCLIST and reports the HONEST total (no longer silently pinned
# at MAX_ROWS=12). TASKMAN_TEST=1 makes init spawn sbin/taskman at boot; we assert
# from serial: [TASKMAN] starting + [TASKMAN] N procs with N > 12 (a full boot has
# dozens of processes, so a count >12 proves the PROC_CAP=256 honest-count fix --
# the old code capped the request at 12). The selection-highlight + kill paths are
# code-correct (ui_widget_set_bg / SYS_KILL) but GUI-visual; this gate covers the
# data/count path headlessly. Run: wsl -d Arch bash build_test/taskman_proof.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[tm] quick_build kernel (byte-unchanged)..."
bash scripts/quick_build.sh > /tmp/tm_qb.log 2>&1
grep -q SUCCESS /tmp/tm_qb.log || { echo "KERNEL BUILD FAIL"; tail -6 /tmp/tm_qb.log; exit 1; }

echo "[tm] TASKMAN_TEST=1 build_all (init spawns sbin/taskman)..."
TASKMAN_TEST=1 bash scripts/build_all.sh > /tmp/tm_ba.log 2>&1
if grep -qiE 'taskman\.c.*error:|ui\.c.*error:|error:.*taskman|undefined reference' /tmp/tm_ba.log; then
  echo "TASKMAN COMPILE ERRORS:"; grep -iE 'error:|undefined reference' /tmp/tm_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

echo ""
echo "=== boots + spawns taskman (40s) ==="
SER=build_test/tm_ser.log; rm -f "$SER"
timeout 45 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot >/dev/null 2>&1 || true
sleep 1

CRASH=0
for pat in 'PANIC' 'triple fault' 'double fault' 'KERNEL HALT'; do
  c=$(grep -icF "$pat" "$SER"); CRASH=$((CRASH+c))
done
LINES=$(wc -l < "$SER")
START=$(grep -acF '[TASKMAN] starting' "$SER")
# the highest [TASKMAN] N procs count seen (the honest live total)
NPROCS=$(grep -aoE '\[TASKMAN\] [0-9]+ procs' "$SER" | grep -oE '[0-9]+' | sort -n | tail -1)
[ -z "$NPROCS" ] && NPROCS=0

echo "  lines=$LINES  kernel_fatal=$CRASH  taskman_start=$START  max_proc_count=$NPROCS"
echo "  --- [TASKMAN] markers ---"; grep -aF '[TASKMAN]' "$SER" | head
echo ""
if [ "$CRASH" -eq 0 ] && [ "$START" -ge 1 ] && [ "$NPROCS" -gt 12 ] && [ "$LINES" -ge 2000 ]; then
  echo "TASKMAN: PASS (real process manager: [TASKMAN] starting + reads the live process"
  echo "  table via SYS_PROCLIST -- honest count=$NPROCS (>12, the old silent cap))"
  exit 0
else
  echo "TASKMAN: FAIL (kernel_fatal=$CRASH start=$START max_proc_count=$NPROCS lines=$LINES)"
  echo "--- [TASKMAN] lines ---"; grep -aF '[TASKMAN]' "$SER" | head -20
  echo "--- tail 15 ---"; tail -15 "$SER"
  exit 1
fi
