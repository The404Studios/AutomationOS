#!/bin/bash
# MODEL-BRIDGE-0: the model seam fed by an EXTERNAL endpoint (userspace-only).
# Start the host-side model stub (scripts/model_server_stub.py, loopback :8431),
# build_all, boot headless with slirp networking, capture the MODELBRIDGE verdict
# + the whole rail + a screenshot. The guest reaches the stub via 10.0.2.2:8431.
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
OUT="${1:-mbcheck}"
PORT=8431

echo "[mb] build_all (userspace + ISO; kernel unchanged)..."
bash scripts/build_all.sh > /tmp/mb_all.log 2>&1
echo "[mb] build_all rc=$?; tail:"; tail -2 /tmp/mb_all.log
if grep -nE "error:|undefined reference" /tmp/mb_all.log; then echo "[mb] BUILD ERRORS"; exit 1; fi

echo "[mb] starting the model endpoint stub on 127.0.0.1:$PORT ..."
STUBLOG=/tmp/mb_stub_$OUT.log
pkill -f model_server_stub.py 2>/dev/null; sleep 0.3
python3 scripts/model_server_stub.py $PORT > "$STUBLOG" 2>&1 &
SPID=$!
sleep 1
grep -qF "listening" "$STUBLOG" || { echo "[mb] STUB FAILED to start"; cat "$STUBLOG"; exit 1; }

SOCK=/tmp/qmp_$OUT.sock; LOG=/tmp/mb_serial_$OUT.log
rm -f "$SOCK" "$LOG" /tmp/$OUT.png
qemu-system-x86_64 -cdrom build/automationos.iso -m 512 -netdev user,id=n0 -device e1000,netdev=n0 \
  -display none -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" &
QPID=$!
# wait for the verdict itself (modelbridge runs after DHCP), bounded
for i in $(seq 1 75); do grep -qE "MODELBRIDGE:" "$LOG" 2>/dev/null && break; sleep 1; done
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
sleep 1; kill $QPID 2>/dev/null; kill $SPID 2>/dev/null
[ -f /tmp/$OUT.png ] && { mkdir -p screenshots; cp -f /tmp/$OUT.png screenshots/$OUT.png; echo "SAVED screenshots/$OUT.png ($(stat -c%s screenshots/$OUT.png) B)"; }

echo "=== MODELBRIDGE (the external-model verdict) ==="; grep -nE "MODELBRIDGE:" "$LOG" || echo "(none)"
echo "=== the stub saw (request -> reply) ==="; grep -nE "\->" "$STUBLOG" || echo "(none)"
echo "=== rail still green (CHAINHOST/TOOLSET/AGENTHOST/TOOLRUN/RPCTEST/[CHAN]) ==="; grep -nE "CHAINHOST:|TOOLSET:|AGENTHOST:|TOOLRUN:|RPCTEST:|\[CHAN\]" "$LOG" || echo "(none)"
echo "=== PANIC? ==="; grep -niE "PANIC" "$LOG" | head -3 || echo "(no panic)"
