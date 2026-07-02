#!/bin/bash
# TOOLSET-0: userspace-only (safe tools over the existing rail). build_all, boot
# headless, capture the TOOLSET verdict + a screenshot.
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
OUT="${1:-tscheck}"

echo "[ts] build_all (userspace + ISO; kernel unchanged)..."
bash scripts/build_all.sh > /tmp/ts_all.log 2>&1
echo "[ts] build_all rc=$?; tail:"; tail -2 /tmp/ts_all.log
if grep -nE "error:|undefined reference" /tmp/ts_all.log; then echo "[ts] BUILD ERRORS"; exit 1; fi

SOCK=/tmp/qmp_$OUT.sock; LOG=/tmp/ts_serial_$OUT.log
rm -f "$SOCK" "$LOG" /tmp/$OUT.png
qemu-system-x86_64 -cdrom build/automationos.iso -m 512 -netdev user,id=n0 -device e1000,netdev=n0 \
  -display none -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" &
QPID=$!
for i in $(seq 1 50); do grep -qiF desktop "$LOG" 2>/dev/null && break; sleep 1; done
sleep 9
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

echo "=== TOOLSET (the tool-surface verdict) ==="; grep -nE "TOOLSET:" "$LOG" || echo "(none)"
echo "=== rail still green (AGENTHOST/TOOLRUN/RPCTEST/[CHAN]) ==="; grep -nE "AGENTHOST:|TOOLRUN:|RPCTEST:|\[CHAN\]" "$LOG" || echo "(none)"
echo "=== PANIC? ==="; grep -niE "PANIC" "$LOG" | head -3 || echo "(no panic)"
