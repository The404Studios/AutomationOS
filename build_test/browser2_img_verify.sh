#!/bin/bash
# BROWSER2-IMG-0: <img> rendering in browser2 (userspace-only; kernel unchanged).
# build_all (generates the /etc/imgtest fixtures into the initrd), boot headless,
# capture the BROWSER2-IMG verdict + the browser/rail lines + a screenshot.
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
OUT="${1:-imgcheck}"

echo "[img] build_all (userspace + ISO; kernel unchanged)..."
# BROWSER-DEDUP: the about:imgtest self-test spawn is gated behind SMOKE_SELFTEST
# (default OFF so a normal desktop opens one browser); this gate needs it ON.
SMOKE_SELFTEST=1 bash scripts/build_all.sh > /tmp/img_all.log 2>&1
echo "[img] build_all rc=$?; tail:"; tail -2 /tmp/img_all.log
if grep -nE "error:|undefined reference" /tmp/img_all.log; then echo "[img] BUILD ERRORS"; exit 1; fi
grep -n "imgfixtures" /tmp/img_all.log || { echo "[img] FIXTURES MISSING"; exit 1; }

SOCK=/tmp/qmp_$OUT.sock; LOG=/tmp/img_serial_$OUT.log
rm -f "$SOCK" "$LOG" /tmp/$OUT.png
qemu-system-x86_64 -cdrom build/automationos.iso -m 512 -netdev user,id=n0 -device e1000,netdev=n0 \
  -display none -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" &
QPID=$!
for i in $(seq 1 75); do grep -qE "BROWSER2-IMG:" "$LOG" 2>/dev/null && break; sleep 1; done
sleep 6
python3 - "$SOCK" "$OUT" <<'PY'
import socket, json, sys
sock, out = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(sock)
f = s.makefile('rw'); f.readline()
def cmd(o):
    f.write(json.dumps(o)+"\n"); f.flush()
    while True:
        l = f.readline()
        if not l: return ""
        if '"return"' in l or '"error"' in l: return l.strip()
cmd({"execute":"qmp_capabilities"})
cmd({"execute":"screendump","arguments":{"filename":"/tmp/%s.png"%out,"format":"png"}})
PY
sleep 1; kill $QPID 2>/dev/null
[ -f /tmp/$OUT.png ] && { mkdir -p screenshots; cp -f /tmp/$OUT.png screenshots/$OUT.png; echo "SAVED screenshots/$OUT.png ($(stat -c%s screenshots/$OUT.png) B)"; }

echo "=== BROWSER2-IMG (the img-rendering verdict) ==="; grep -nE "BROWSER2-IMG:" "$LOG" || echo "(none)"
echo "=== img loader lines ==="; grep -nE "BROWSER2: img " "$LOG" || echo "(none)"
echo "=== browser pipeline still green ==="; grep -nE "DOMTEST:|HTMLTEST:|CSSTEST:|LAYOUTTEST:|WEBTEST:|WEBAPITEST:|BROWSER2: rendered" "$LOG" || echo "(none)"
echo "=== rail still green ==="; grep -nE "MODELBRIDGE:|CHAINHOST:|TOOLSET:|AGENTHOST:|TOOLRUN:|RPCTEST:" "$LOG" || echo "(none)"
echo "=== PANIC? ==="; grep -niE "PANIC" "$LOG" | head -3; echo "(panic grep done)"
