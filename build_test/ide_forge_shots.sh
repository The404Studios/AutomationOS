#!/bin/bash
# IDE-FORGE-0 proof shots: the dictionary popup teaching pane, the map node
# chips, the ACTIONS deck with a stamped result, the Project Pulse BEFORE and
# AFTER a G GENERATE (the headline absent-card -> code -> coherence-rises loop,
# captured on tower_tick this time, per the audit), and the MARK knob flip with
# its FUNCTIONS-list + Pulse consequences. Same QMP pipeline as the showcase.
# Usage: bash build_test/ide_forge_shots.sh   (ISO must exist: IDE=1 build_all)
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
OUT="forge"

SOCK=/tmp/qmp_$OUT.sock; LOG=/tmp/forge_serial.log
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

# caret into tower_tick (the function WITH the absent card + risk)
for _ in range(27): key(["down"], hold=0.03)

# ---- 1. DICTIONARY popup: "sys" summons syscall/sys_write with the
#         teaching preview pane (signature + doc + snippet)
key(["end"]); key(["ret"])
for q in ["s","y","s"]: key([q], hold=0.04)
shot("dict")
key(["esc"])                                   # popup close (the audit fix)
for _ in range(4): key(["backspace"], hold=0.03)

# ---- 2. the map on tower_tick with the new node chips (ln / R W C / holes)
key(["ctrl","comma"])                          # WS_LEGO (settings)
key(["1"])
shot("map_chips")

# ---- 3. Pulse BEFORE generate (missing-cards count, real COH trend)
key(["5"])
shot("pulse_before")

# ---- 4. ACTIONS deck; stamp one row (A = re-analyze, fast) -> result chip
key(["4"])
key(["a"])
time.sleep(2)
shot("deck")

# ---- 5. THE HEADLINE LOOP: G generates the missing claim_slot from the
#         blueprint, the card resolves, coherence rises -- on camera
key(["1"])
key(["g"])
time.sleep(3)
shot("map_generated")
key(["5"])
shot("pulse_after")

# ---- 6. MARK tab: flip DONE on the focused function; consequences land in
#         the FUNCTIONS list (OK glyph) + the Pulse done count
key(["2"])
for _ in range(4): key(["tab"], hold=0.05)     # PORTS->CONN->INFO->LIB->MARK
shot("mark_tab")
key(["spc"])                                   # flip DONE
shot("mark_done")
key(["5"])
shot("pulse_done")
PY
sleep 1; kill $QPID 2>/dev/null
mkdir -p screenshots
N=0
for p in dict map_chips pulse_before deck map_generated pulse_after mark_tab mark_done pulse_done; do
  [ -f /tmp/${OUT}_$p.png ] && { cp -f /tmp/${OUT}_$p.png screenshots/forge_$p.png; N=$((N+1)); echo "SAVED screenshots/forge_$p.png ($(stat -c%s screenshots/forge_$p.png) B)"; }
done
echo "captured=$N/9"
echo "=== crashes? ==="; grep -niE "PANIC|CPU EXCEPTION|Terminating faulting" "$LOG" | head -5; echo "(done)"
