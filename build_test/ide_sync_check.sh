#!/bin/bash
# IDE-SYNC-0 verify: build, boot with IDE autostart, QMP-inject Ctrl+Shift+E
# to switch to the LEGO workspace, screenshot both workspaces.
# Usage: bash ide_sync_check.sh <outname>
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
OUT="${1:-synccheck}"

echo "[sync] quick_build..."
bash scripts/quick_build.sh > /tmp/sync_kernel.log 2>&1
echo "[sync] quick_build rc=$?"
if grep -nE "error:|undefined reference" /tmp/sync_kernel.log; then echo "[sync] KERNEL BUILD ERRORS"; exit 1; fi
echo "[sync] build_all IDE=1..."
IDE=1 bash scripts/build_all.sh > /tmp/sync_all.log 2>&1
echo "[sync] build_all rc=$?; tail:"; tail -2 /tmp/sync_all.log
if grep -nE "error:|undefined reference" /tmp/sync_all.log; then echo "[sync] BUILD ERRORS"; exit 1; fi

SOCK=/tmp/qmp_$OUT.sock; LOG=/tmp/sync_serial_$OUT.log
rm -f "$SOCK" "$LOG" /tmp/${OUT}_ed.png /tmp/${OUT}_lego.png /tmp/${OUT}_insp.png
qemu-system-x86_64 -cdrom build/automationos.iso -m 512 -netdev user,id=n0 -device e1000,netdev=n0 \
  -display none -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" &
QPID=$!
for i in $(seq 1 60); do grep -qF "IDE_AUTOSTART" "$LOG" 2>/dev/null && break; sleep 1; done
sleep 14
python3 - "$SOCK" "$OUT" <<'PY'
import socket, json, time, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(sys.argv[1])
out = sys.argv[2]
f = s.makefile('rw'); f.readline()
def cmd(o):
    f.write(json.dumps(o)+"\n"); f.flush()
    while True:
        l = f.readline()
        if not l: return ""
        if '"return"' in l or '"error"' in l: return l.strip()
cmd({"execute":"qmp_capabilities"})
def key(qcodes, hold=0.08):
    evs = [{"type":"key","data":{"down":True,"key":{"type":"qcode","data":q}}} for q in qcodes]
    cmd({"execute":"input-send-event","arguments":{"events":evs}})
    time.sleep(hold)
    evs = [{"type":"key","data":{"down":False,"key":{"type":"qcode","data":q}}} for q in reversed(qcodes)]
    cmd({"execute":"input-send-event","arguments":{"events":evs}})
    time.sleep(0.4)
def shot(name):
    time.sleep(0.8)
    cmd({"execute":"screendump","arguments":{"filename":name,"format":"png"}})

# move the caret into tower_tick's body (down x27), proving caret->symbol
# tracking, then screenshot the editor workspace
for _ in range(27): key(["down"], hold=0.03)
shot("/tmp/%s_ed.png" % out)
# LEGO workspace: Ctrl+, lands in WS_LEGO (settings tab), then '1' = VIZ_MAP
# and '2' = VIZ_INSPECTOR -- the map central card + inspector must show the
# function the EDITOR caret was in (tower_tick), and the status-bar breadcrumb
# must read FN tower_tick.
key(["ctrl","comma"])
key(["1"])
shot("/tmp/%s_lego.png" % out)
# S2: keyboard-select a map satellite (arrow) and FOLLOW it (Enter) -- the
# central card, the breadcrumb FN, and the Ln must all jump to the called
# function (map click -> editor jump, via the same map_sat_follow path).
key(["right"])
key(["ret"])
shot("/tmp/%s_jump.png" % out)
key(["2"])
shot("/tmp/%s_insp.png" % out)
PY
sleep 1; kill $QPID 2>/dev/null
for p in ${OUT}_ed ${OUT}_lego ${OUT}_jump ${OUT}_insp; do
  [ -f /tmp/$p.png ] && { mkdir -p screenshots; cp -f /tmp/$p.png screenshots/$p.png; echo "SAVED screenshots/$p.png ($(stat -c%s screenshots/$p.png) B)"; }
done
echo "=== crashes? ==="; grep -niE "PANIC|CPU EXCEPTION|Terminating faulting" "$LOG" | head -3; echo "(done)"
