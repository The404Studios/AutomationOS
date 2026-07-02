#!/bin/bash
# AGENT-RPC-0 P6c: kernel grant/accept + capability proof. quick_build (kernel,
# SCHED_DEBUG=0) + build_all + boot, capture TOOLRUN/RUNNER + earlier green lines.
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
OUT="${1:-p6ccheck}"

echo "[p6c] quick_build (kernel, SCHED_DEBUG=0)..."
SCHED_DEBUG=0 bash scripts/quick_build.sh > /tmp/p6c_qb.log 2>&1
echo "[p6c] quick_build rc=$?; tail:"; tail -3 /tmp/p6c_qb.log
if grep -nE "error:|undefined reference" /tmp/p6c_qb.log; then echo "[p6c] KERNEL BUILD ERRORS"; exit 1; fi

echo "[p6c] build_all (ISO)..."
bash scripts/build_all.sh > /tmp/p6c_all.log 2>&1
echo "[p6c] build_all rc=$?; tail:"; tail -2 /tmp/p6c_all.log
if grep -nE "error:|undefined reference" /tmp/p6c_all.log; then echo "[p6c] ISO BUILD ERRORS"; exit 1; fi

SOCK=/tmp/qmp_$OUT.sock; LOG=/tmp/p6c_serial_$OUT.log
rm -f "$SOCK" "$LOG" /tmp/$OUT.png
qemu-system-x86_64 -cdrom build/automationos.iso -m 512 -netdev user,id=n0 -device e1000,netdev=n0 \
  -display none -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" &
QPID=$!
for i in $(seq 1 50); do grep -qiF desktop "$LOG" 2>/dev/null && break; sleep 1; done
sleep 8
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

echo "=== TOOLRUN (P6c agent verdict) ==="; grep -nE "TOOLRUN:" "$LOG" || echo "(none)"
echo "=== RUNNER (P6c runner verdict) ==="; grep -nE "RUNNER:" "$LOG" || echo "(none)"
echo "=== earlier green (RPCTEST/MSGTEST/[CHAN]) ==="; grep -nE "RPCTEST:|MSGTEST:|\[CHAN\]" "$LOG" || echo "(none)"
echo "=== PANIC? ==="; grep -niE "PANIC" "$LOG" | head -3 || echo "(no panic)"
