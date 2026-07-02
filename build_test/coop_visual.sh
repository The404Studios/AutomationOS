#!/bin/bash
# COOP-VISUAL-0: capture the 2-window DeadZone co-op demo as PNG screendumps.
# DZ_MPGUI_DEMO=1 makes init spawn deadzoned + two GUI DeadZone windows that
# auto-join over loopback; each prints "DEADZONE: coop slot=N drew_cyan=M" every
# 30 frames (M = teammate sprites that passed projection AND hit the framebuffer).
# We wait for drew_cyan>0 (co-op state actually on screen), then QMP-screendump.
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 9
mkdir -p screenshots
SER=/tmp/coop_ser.log; SOCK=/tmp/coop_qmp.sock
rm -f "$SER" "$SOCK" screenshots/coop_*.png /tmp/coop_*.ppm

echo "[coop] build_all DZ_MPGUI_DEMO=1 ..."
DZ_MPGUI_DEMO=1 bash scripts/build_all.sh > /tmp/coop_ba.log 2>&1
grep -qE 'error:|undefined reference' /tmp/coop_ba.log && { echo "BUILD ERRORS"; grep -E 'error:|undefined reference' /tmp/coop_ba.log|head; exit 1; }
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

echo "[coop] boot with QMP + wait for drew_cyan>0 (co-op teammate on screen) ..."
qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -display none -qmp "unix:$SOCK,server,nowait" -serial "file:$SER" -no-reboot &
QPID=$!

# Wait up to 110s for a co-op frame that actually drew a teammate.
CY=0
for i in $(seq 1 110); do
    if grep -aoE 'drew_cyan=[1-9][0-9]*' "$SER" 2>/dev/null | head -1 | grep -q .; then CY=1; break; fi
    sleep 1
done
echo "  drew_cyan seen=$CY"
echo "  co-op serial markers:"; grep -aE 'DZ_MPGUI_DEMO|DEADZONED: (listening|client joined)|DEADZONE: (ready|coop slot)' "$SER" | head -10

# Capture two screendumps a few seconds apart (motion between them).
python3 - "$SOCK" <<'PY'
import socket, json, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(sys.argv[1])
f = s.makefile('rw'); f.readline()
def cmd(o):
    f.write(json.dumps(o)+"\n"); f.flush()
    while True:
        l=f.readline()
        if not l: return ""
        if '"return"' in l or '"error"' in l: return l.strip()
cmd({"execute":"qmp_capabilities"})
for n in (1,2):
    r=cmd({"execute":"screendump","arguments":{"filename":"/tmp/coop_%d.png"%n,"format":"png"}})
    if '"error"' in r:
        cmd({"execute":"screendump","arguments":{"filename":"/tmp/coop_%d.ppm"%n}})
    print("shot%d:"%n, r[:60]); time.sleep(3)
PY

sleep 1; kill $QPID 2>/dev/null
for n in 1 2; do
    if [ -f /tmp/coop_$n.png ]; then cp /tmp/coop_$n.png screenshots/coop_$n.png
    elif [ -f /tmp/coop_$n.ppm ]; then
        command -v pnmtopng >/dev/null && pnmtopng /tmp/coop_$n.ppm > screenshots/coop_$n.png 2>/dev/null || cp /tmp/coop_$n.ppm screenshots/coop_$n.ppm
    fi
done
echo "[coop] artifacts:"; ls -la screenshots/coop_* 2>/dev/null
[ "$CY" = 1 ] && [ -f screenshots/coop_1.png ] && echo "COOP-VISUAL-0: PASS (drew_cyan>0 + screendump captured)" || echo "COOP-VISUAL-0: PARTIAL (see markers/artifacts above)"
