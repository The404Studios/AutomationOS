#!/bin/bash
# CHANNEL-0 P5a: rebuild kernel (default gates) + ISO, boot headless, capture the
# [CHAN] selftest serial lines + a desktop screenshot.
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
OUT="${1:-p5check}"

echo "[p5] quick_build (kernel, SCHED_DEBUG=0 -- clean milestone profile)..."
SCHED_DEBUG=0 bash scripts/quick_build.sh > /tmp/p5_qb.log 2>&1
echo "[p5] quick_build rc=$?; tail:"; tail -3 /tmp/p5_qb.log
if grep -nE "error:|undefined reference" /tmp/p5_qb.log; then echo "[p5] KERNEL BUILD ERRORS"; exit 1; fi

echo "[p5] build_all (ISO)..."
FULL=1 bash scripts/build_all.sh > /tmp/p5_all.log 2>&1
echo "[p5] build_all rc=$?; tail:"; tail -2 /tmp/p5_all.log
if grep -nE "error:|undefined reference" /tmp/p5_all.log; then echo "[p5] ISO BUILD ERRORS"; exit 1; fi

SOCK=/tmp/qmp_$OUT.sock; LOG=/tmp/p5_serial_$OUT.log
rm -f "$SOCK" "$LOG" /tmp/$OUT.png
qemu-system-x86_64 -cdrom build/automationos.iso -m 512 -netdev user,id=n0 -device e1000,netdev=n0 \
  -display none -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" &
QPID=$!
for i in $(seq 1 50); do grep -qiF desktop "$LOG" 2>/dev/null && break; sleep 1; done
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

echo "=== [CHAN] selftest lines (serial) ==="; grep -nE "\[CHAN\]" "$LOG" || echo "(none captured)"
echo "=== MSGTEST (P5b userspace round-trip) ==="; grep -nE "MSGTEST:" "$LOG" || echo "(none captured)"
echo "=== desktop / PANIC ==="; grep -niE "reached desktop|PANIC|FORKTEST|ARGVTEST" "$LOG" | head -6
