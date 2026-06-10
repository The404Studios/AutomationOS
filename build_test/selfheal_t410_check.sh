#!/bin/bash
# =============================================================================
# selfheal_t410_check.sh -- SELFHEAL-FIX-0 verify: the T410-PROFILE recovery
# proof with the acceptance the original selfheal_smoke.sh never had: after a
# recovery the desktop must come back WITH ITS WINDOWS (terminal+filemanager),
# not empty. An empty post-recovery desktop is what reads as "self heal is
# not working" on the real T410.
#
# One run: blocking forced freeze (FREEZE_MODE=0, recoverable on the default
# cooperative kernel) under the exact shipping T410 profile:
#   kernel:    T410_SAFE=1 SCHED_DEBUG=0
#   userspace: DESKTOP_MINIMAL=1 SELFHEAL=1 (+ FREEZE_TEST=1 for the proof)
#
# PASS = full recovery marker chain + >=2 "SELFHEAL: restored win=" lines +
# no storm + no kernel fault, plus a post-recovery screendump for eyes-on.
# Usage: bash build_test/selfheal_t410_check.sh <outname>
# =============================================================================
set -uo pipefail
cd /mnt/c/Users/wilde/Desktop/Kernel
OUT="${1:-shfix}"

echo "[shfix] T410_SAFE kernel (quick_build)..."
T410_SAFE=1 SCHED_DEBUG=0 bash scripts/quick_build.sh > /tmp/shfix_kernel.log 2>&1
if grep -qE "error:|undefined reference" /tmp/shfix_kernel.log; then
    echo "[shfix] KERNEL BUILD ERRORS:"; grep -E "error:|undefined reference" /tmp/shfix_kernel.log | head -5; exit 1
fi
echo "[shfix] DESKTOP_MINIMAL+SELFHEAL+FREEZE_TEST userspace (build_all)..."
DESKTOP_MINIMAL=1 SELFHEAL=1 FREEZE_TEST=1 FREEZE_MODE=0 bash scripts/build_all.sh > /tmp/shfix_all.log 2>&1
if grep -qE "error:|undefined reference" /tmp/shfix_all.log; then
    echo "[shfix] BUILD ERRORS:"; grep -E "error:|undefined reference" /tmp/shfix_all.log | head -5; exit 1
fi
tail -2 /tmp/shfix_all.log

SOCK=/tmp/qmp_$OUT.sock; LOG=/tmp/shfix_$OUT.log
rm -f "$SOCK" "$LOG"
qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -display none -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" &
QPID=$!

# Recovery timeline: freeze @ frame 240 (~4s into the loop) + 2.5s stall
# detect + 2s overlay + kill + respawn. Wait generously, then let the
# restored desktop actually render before the screendump.
ok=0
for i in $(seq 1 90); do
    grep -qF "CWATCHDOG: PASS respawned" "$LOG" 2>/dev/null && { ok=1; break; }
    sleep 1
done
sleep 4

python3 - "$SOCK" "$OUT" <<'PY'
import socket, json, sys
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(sys.argv[1])
    f = s.makefile('rw'); f.readline()
    def cmd(o):
        f.write(json.dumps(o)+"\n"); f.flush()
        while True:
            l = f.readline()
            if not l or '"return"' in l or '"error"' in l: return
    cmd({"execute":"qmp_capabilities"})
    cmd({"execute":"screendump","arguments":{"filename":"/tmp/%s_recovered.png" % sys.argv[2],"format":"png"}})
    print("screendump ok")
except Exception as e:
    print("QMP screendump failed: %s" % e)
PY
sleep 1; kill $QPID 2>/dev/null
if [ -f /tmp/${OUT}_recovered.png ]; then
    mkdir -p screenshots
    cp -f /tmp/${OUT}_recovered.png screenshots/${OUT}_recovered.png
    echo "SAVED screenshots/${OUT}_recovered.png ($(stat -c%s screenshots/${OUT}_recovered.png) B)"
fi

echo "=== markers ==="
P=1
while IFS= read -r m; do
    if grep -qF "$m" "$LOG"; then echo "  ok   $m"; else echo "  MISS $m"; P=0; fi
done <<'MARKERS'
[INIT] SELFHEAL: heartbeat segment ready
[SHELL] SELFHEAL: heartbeat published
CWATCHDOG: watching
FREEZE_TEST: entering freeze mode 0
CWATCHDOG: heartbeat stalled
CWATCHDOG: PASS respawned
MARKERS

RESTORED=$(grep -cF "SELFHEAL: restored win=" "$LOG" 2>/dev/null || true)
RESTORED=${RESTORED:-0}
echo "  restored_windows=$RESTORED (expect >= 2: terminal + filemanager)"
[ "$RESTORED" -ge 2 ] || P=0
if grep -qF "CWATCHDOG: FAIL recovery storm" "$LOG"; then echo "  STORM (one-shot latch broken)"; P=0; fi
if grep -qiE "PANIC|CPU EXCEPTION|TRIPLE FAULT" "$LOG"; then echo "  KERNEL FAULT during recovery"; P=0; fi

if [ "$P" = "1" ]; then
    echo "SELFHEAL-FIX: PASS recovery=1 restored_windows=$RESTORED no_storm=1 no_fault=1"
    exit 0
else
    echo "SELFHEAL-FIX: FAIL (serial: $LOG)"
    exit 1
fi
