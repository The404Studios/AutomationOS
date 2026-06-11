#!/bin/bash
# Capture a clean QEMU framebuffer screenshot via QMP screendump.
#   usage: shot.sh <iso> <out-name> [extra qemu args...]
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
ISO="${1:?iso}"; OUT="${2:?out}"; shift 2
mkdir -p screenshots
SOCK=/tmp/qmp_$OUT.sock; BOOTLOG=/tmp/shot_${OUT}.log
rm -f "$SOCK" /tmp/${OUT}.ppm /tmp/${OUT}.png

qemu-system-x86_64 -cdrom "$ISO" -m 512 -netdev user,id=n0 -device e1000,netdev=n0 \
  -display none -qmp "unix:$SOCK,server,nowait" -serial "file:$BOOTLOG" "$@" &
QPID=$!
for i in $(seq 1 45); do grep -qiF desktop "$BOOTLOG" 2>/dev/null && break; sleep 1; done
sleep 8

python3 - "$SOCK" "$OUT" <<'PY'
import socket, json, sys
sock, out = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(sock)
f = s.makefile('rw')
f.readline()
def cmd(o):
    f.write(json.dumps(o)+"\n"); f.flush()
    while True:
        l = f.readline()
        if not l: return ""
        if '"return"' in l or '"error"' in l: return l.strip()
cmd({"execute":"qmp_capabilities"})
r = cmd({"execute":"screendump","arguments":{"filename":"/tmp/%s.png"%out,"format":"png"}})
print("png:", r[:80])
if '"error"' in r:
    r = cmd({"execute":"screendump","arguments":{"filename":"/tmp/%s.ppm"%out}})
    print("ppm:", r[:80])
PY

sleep 1; kill $QPID 2>/dev/null
if [ -f /tmp/${OUT}.ppm ] && [ ! -f /tmp/${OUT}.png ]; then
  command -v pnmtopng >/dev/null && pnmtopng /tmp/${OUT}.ppm > /tmp/${OUT}.png 2>/dev/null
fi
if [ -f /tmp/${OUT}.png ]; then cp -f /tmp/${OUT}.png screenshots/${OUT}.png; echo "SAVED screenshots/${OUT}.png ($(stat -c%s screenshots/${OUT}.png) B)"; else echo "NO PNG"; fi
