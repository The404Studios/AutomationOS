#!/bin/bash
# T410-RETEST: build the flashable ISO carrying BOTH heap/aliasing fixes
# (malloc tcache three-state 8a0aafc + initrd direct-map 9dad3ac) on the
# proven T410 profile, sanity-boot it headless in QEMU, then restore the
# DEFAULT kernel+ISO so the tree's build artifacts stay on the default
# profile for the next brick.
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
OUT=automationos-t410-bothfixes.iso

echo "[t410] kernel: T410_SAFE=1 SCHED_DEBUG=0 quick_build..."
T410_SAFE=1 SCHED_DEBUG=0 bash scripts/quick_build.sh > /tmp/t410_kernel.log 2>&1
echo "[t410] quick_build rc=$?; tail:"; tail -2 /tmp/t410_kernel.log
if grep -nE "error:|undefined reference" /tmp/t410_kernel.log; then echo "[t410] KERNEL BUILD ERRORS"; exit 1; fi

echo "[t410] userspace+ISO: DESKTOP_MINIMAL=1 SELFHEAL=1 build_all..."
DESKTOP_MINIMAL=1 SELFHEAL=1 bash scripts/build_all.sh > /tmp/t410_all.log 2>&1
echo "[t410] build_all rc=$?; tail:"; tail -2 /tmp/t410_all.log
if grep -nE "error:|undefined reference" /tmp/t410_all.log; then echo "[t410] BUILD ERRORS"; exit 1; fi

cp -f build/automationos.iso "$OUT"
echo "[t410] SAVED $OUT ($(stat -c%s "$OUT") B)"

echo "[t410] QEMU sanity boot (desktop + 0 panic; minimal init = no selftest storm)..."
SOCK=/tmp/qmp_t410bf.sock; LOG=/tmp/t410bf_serial.log
rm -f "$SOCK" "$LOG" /tmp/t410bf.png
qemu-system-x86_64 -cdrom "$OUT" -m 512 -display none \
  -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" &
QPID=$!
for i in $(seq 1 50); do grep -qiE "desktop|compositor" "$LOG" 2>/dev/null && break; sleep 1; done
sleep 12
python3 - "$SOCK" <<'PY'
import socket, json, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect("/tmp/qmp_t410bf.sock")
f = s.makefile('rw'); f.readline()
def cmd(o):
    f.write(json.dumps(o)+"\n"); f.flush()
    while True:
        l = f.readline()
        if not l: return ""
        if '"return"' in l or '"error"' in l: return l.strip()
cmd({"execute":"qmp_capabilities"})
cmd({"execute":"screendump","arguments":{"filename":"/tmp/t410bf.png","format":"png"}})
PY
sleep 1; kill $QPID 2>/dev/null
[ -f /tmp/t410bf.png ] && { mkdir -p screenshots; cp -f /tmp/t410bf.png screenshots/t410bf.png; echo "SAVED screenshots/t410bf.png ($(stat -c%s screenshots/t410bf.png) B)"; }
echo "=== the initrd mapping (the fix, on the T410 kernel) ==="; grep -nE "INITRD\] Initrd" "$LOG" || echo "(none)"
echo "=== T410 profile markers ==="; grep -nE "T410_SAFE|DESKTOP_MINIMAL|SELFHEAL" "$LOG" | head -4 || echo "(none)"
echo "=== PANIC? ==="; grep -niE "PANIC" "$LOG" | head -3; echo "(panic grep done)"

echo "[t410] restoring the DEFAULT kernel + ISO..."
bash scripts/quick_build.sh > /tmp/t410_restore_k.log 2>&1
echo "[t410] default quick_build rc=$?"
bash scripts/build_all.sh > /tmp/t410_restore_a.log 2>&1
echo "[t410] default build_all rc=$?; tail:"; tail -1 /tmp/t410_restore_a.log
