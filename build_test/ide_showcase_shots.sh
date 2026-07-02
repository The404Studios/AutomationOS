#!/bin/bash
# IDE-SHOWCASE-0: capture the full Semantic LEGO IDE tour for the showcase
# doc -- every workspace and VIZ panel, the live build/run loop, completion.
# Reuses the proven ide_sync_check.sh QMP pipeline (keyboard injection +
# screendump; mouse rel does not reach the compositor).
# Usage: bash build_test/ide_showcase_shots.sh   (ISO must exist: IDE=1 build_all)
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
OUT="showcase"

SOCK=/tmp/qmp_$OUT.sock; LOG=/tmp/showcase_serial.log
rm -f "$SOCK" "$LOG" /tmp/${OUT}_*.png
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
    time.sleep(0.35)
def shot(name):
    time.sleep(0.9)
    cmd({"execute":"screendump","arguments":{"filename":"/tmp/%s_%s.png" % (out, name),"format":"png"}})

# ---- 1. EDITOR: caret inside tower_tick (down x27) -- breadcrumb + gutter
for _ in range(27): key(["down"], hold=0.03)
shot("editor")

# ---- 2. completion popup: type a prefix at a fresh line end
key(["end"]); key(["ret"])
for q in ["t","o","w"]: key([q], hold=0.04)
shot("complete")
# REGRESSION PROBE (showcase-audit fix): Esc here used to ide_exit(0) the
# whole IDE -- the first capture run photographed the death. With the fix it
# only closes the popup; every later shot in this tour proves it.
key(["esc"])
for _ in range(4): key(["backspace"], hold=0.03)   # undo the "tow" + newline

# ---- 3. LEGO MAP: the central card + satellites for the caret's function
key(["ctrl","e"])                  # WS_EDITOR -> WS_LEGO
key(["1"])
shot("map")

# ---- 4. map keyboard-follow: arrow to a callee satellite, Enter follows
key(["right"]); key(["ret"])
shot("map_follow")

# ---- 5. cross-file follow: the incoming caller (sibling file) -- breadcrumb flips
key(["left"]); key(["ret"])
shot("map_xfile")

# ---- 6. INSPECTOR: tables for the active symbol
key(["2"])
shot("inspector")

# ---- 7. BUILD inside the IDE (on-device cc) -> the build view
key(["b"])
time.sleep(6)
shot("build")

# ---- 8. RUN the project, then the RUNTIME flow panel
key(["r"])
time.sleep(6)
key(["3"])
shot("runtime")

# ---- 9. ACTIONS + POTENTIALS panels
key(["4"]); shot("actions")
key(["5"]); shot("potentials")

# ---- 10. SETTINGS (knobs & switches)
key(["6"]); shot("settings")

# ---- 11. back to the editor: the round trip survives everything above
key(["ctrl","e"])
shot("editor_back")
PY
sleep 1; kill $QPID 2>/dev/null
mkdir -p screenshots
N=0
for p in editor complete map map_follow map_xfile inspector build runtime actions potentials settings editor_back; do
  [ -f /tmp/${OUT}_$p.png ] && { cp -f /tmp/${OUT}_$p.png screenshots/showcase_$p.png; N=$((N+1)); echo "SAVED screenshots/showcase_$p.png ($(stat -c%s screenshots/showcase_$p.png) B)"; }
done
echo "captured=$N/12"
echo "=== crashes? ==="; grep -niE "PANIC|CPU EXCEPTION|Terminating faulting" "$LOG" | head -5; echo "(done)"
