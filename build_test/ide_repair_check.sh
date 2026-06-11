#!/bin/bash
# IDE-REPAIR-0 per-checkpoint verify: rebuild kernel (SCHED_DEBUG now default
# OFF) + userspace with IDE autostart, boot headless, screenshot the open IDE,
# grep for crashes. Usage: bash ide_repair_check.sh <outname>
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
OUT="${1:-idecheck}"

echo "[ide] quick_build (default profile, SCHED_DEBUG now opt-in)..."
bash scripts/quick_build.sh > /tmp/ide_kernel.log 2>&1
echo "[ide] quick_build rc=$?; tail:"; tail -2 /tmp/ide_kernel.log
if grep -nE "error:|undefined reference" /tmp/ide_kernel.log; then echo "[ide] KERNEL BUILD ERRORS"; exit 1; fi

echo "[ide] build_all IDE=1 (autostarts sbin/ide)..."
IDE=1 bash scripts/build_all.sh > /tmp/ide_all.log 2>&1
echo "[ide] build_all rc=$?; tail:"; tail -2 /tmp/ide_all.log
if grep -nE "error:|undefined reference" /tmp/ide_all.log; then echo "[ide] BUILD ERRORS"; exit 1; fi

SOCK=/tmp/qmp_$OUT.sock; LOG=/tmp/ide_serial_$OUT.log
rm -f "$SOCK" "$LOG" /tmp/$OUT.png
qemu-system-x86_64 -cdrom build/automationos.iso -m 512 -netdev user,id=n0 -device e1000,netdev=n0 \
  -display none -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" &
QPID=$!
for i in $(seq 1 60); do grep -qF "IDE_AUTOSTART" "$LOG" 2>/dev/null && break; sleep 1; done
sleep 14
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

echo "=== IDE autostart ==="; grep -nF "IDE_AUTOSTART" "$LOG" | head -2 || echo "(none)"
echo "=== crashes? ==="; grep -niE "PANIC|CPU EXCEPTION|Terminating faulting" "$LOG" | head -5; echo "(crash grep done)"
