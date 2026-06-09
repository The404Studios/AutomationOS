#!/bin/bash
# INITRD-ALIAS-0: kernel fix -- initrd accessed via the DIRECT MAP so big-image
# user processes can no longer shadow it. build_all (KERNEL changes this time),
# boot headless, capture the three verdict lines + the whole rail + screenshot.
#
# Composite acceptance (assembled from the on-device lines):
#   INITRD-ALIAS: PASS pristine_read=1 mmapheavy_read=1 same_bytes=1 zero_bug_gone=1
#   BROWSER2-IMG-FILE: PASS initrd_img=1            (browser_file_img)
#   BROWSER2-IMG: PASS ... (frozen brick line still green)
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
OUT="${1:-iacheck}"

echo "[ia] quick_build (the KERNEL -- this brick changes kernel/init/initrd.c)..."
bash scripts/quick_build.sh > /tmp/ia_kernel.log 2>&1
echo "[ia] quick_build rc=$?; tail:"; tail -2 /tmp/ia_kernel.log
if grep -nE "error:|undefined reference" /tmp/ia_kernel.log; then echo "[ia] KERNEL BUILD ERRORS"; exit 1; fi

echo "[ia] build_all (userspace + ISO, packages build/kernel.elf)..."
bash scripts/build_all.sh > /tmp/ia_all.log 2>&1
echo "[ia] build_all rc=$?; tail:"; tail -2 /tmp/ia_all.log
if grep -nE "error:|undefined reference" /tmp/ia_all.log; then echo "[ia] BUILD ERRORS"; exit 1; fi

SOCK=/tmp/qmp_$OUT.sock; LOG=/tmp/ia_serial_$OUT.log
rm -f "$SOCK" "$LOG" /tmp/$OUT.png
qemu-system-x86_64 -cdrom build/automationos.iso -m 512 -netdev user,id=n0 -device e1000,netdev=n0 \
  -display none -qmp "unix:$SOCK,server,nowait" -serial "file:$LOG" &
QPID=$!
for i in $(seq 1 75); do grep -qE "INITRD-ALIAS:" "$LOG" 2>/dev/null && grep -qE "BROWSER2-IMG-FILE:" "$LOG" 2>/dev/null && break; sleep 1; done
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

echo "=== the initrd mapping (boot line) ==="; grep -nE "INITRD\] Initrd" "$LOG" || echo "(none)"
echo "=== INITRD-ALIAS (big-image + pristine pair) ==="; grep -nE "INITRD-ALIAS:|INITRDP:" "$LOG" || echo "(none)"
echo "=== browser file-img probe ==="; grep -nE "BROWSER2-IMG-FILE:|BROWSER2: img (loaded|failed) /etc" "$LOG" || echo "(none)"
echo "=== frozen BROWSER2-IMG line still green ==="; grep -nE "BROWSER2-IMG: " "$LOG" || echo "(none)"
echo "=== browser pipeline still green ==="; grep -nE "DOMTEST:|HTMLTEST:|CSSTEST:|LAYOUTTEST:|WEBTEST:|WEBAPITEST:|BROWSER2: rendered" "$LOG" || echo "(none)"
echo "=== rail still green ==="; grep -nE "MODELBRIDGE:|CHAINHOST:|TOOLSET:|AGENTHOST:|TOOLRUN:|RPCTEST:|\[CHAN\]" "$LOG" || echo "(none)"
echo "=== PANIC? ==="; grep -niE "PANIC" "$LOG" | head -3; echo "(panic grep done)"

ok=1
grep -qF "INITRD-ALIAS: PASS pristine_read=1 mmapheavy_read=1 same_bytes=1 zero_bug_gone=1" "$LOG" || ok=0
grep -qF "BROWSER2-IMG-FILE: PASS initrd_img=1" "$LOG" || ok=0
grep -qF "BROWSER2-IMG: PASS png=1 gif=1 bmp=1 missing_safe=1 bounded=1" "$LOG" || ok=0
if [ $ok -eq 1 ]; then
  echo "COMPOSITE: INITRD-ALIAS: PASS pristine_read=1 mmapheavy_read=1 same_bytes=1 browser_file_img=1 zero_bug_gone=1"
else
  echo "COMPOSITE: INITRD-ALIAS: FAIL (see lines above)"
fi
