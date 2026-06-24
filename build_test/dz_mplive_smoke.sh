#!/bin/bash
# DEADZONE-MP-LIVE-0 verify: prove LIVE networked co-op over the kernel's
# loopback datapath. Build init with -DDZ_MPLIVE so it spawns the authoritative
# deadzoned server + the headless dzclient; the client connects to 127.0.0.1:27015
# (the ip_tx 127/8 short-circuit -- no NIC), plays a scripted session, and the
# server must apply our inputs authoritatively (player moves + scores). Assert:
#   DEADZONED: listening ...      (server hosted)
#   DEADZONED: client joined ...  (real TCP connection accepted over loopback)
#   DEADZONE: mp LIVE PASS ...    (client verified authoritative move + kills)
# Run: wsl -d Arch bash build_test/dz_mplive_smoke.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[dzlive] quick_build (default kernel)..."
bash scripts/quick_build.sh > /tmp/dzlive_qb.log 2>&1
if grep -qiE 'error:|^FAIL:|undefined reference' /tmp/dzlive_qb.log; then
  echo "KERNEL BUILD ERRORS:"; grep -iE 'error:|^FAIL:|undefined reference' /tmp/dzlive_qb.log | head; exit 1
fi
grep -q SUCCESS /tmp/dzlive_qb.log || { echo "kernel no SUCCESS"; tail -6 /tmp/dzlive_qb.log; exit 1; }

# init WAITs on dzclient (SYS_WAITPID) right after spawning it, so the client runs
# its full session BEFORE init unleashes the ~70-process self-test storm -- no CPU
# contention, no reliance on DESKTOP_MINIMAL (which is independently broken here).
echo "[dzlive] build_all DZ_MPLIVE=1 (init spawns deadzoned + dzclient, waits on client)..."
DZ_MPLIVE=1 bash scripts/build_all.sh > /tmp/dzlive_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/dzlive_ba.log; then
  echo "USERSPACE BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/dzlive_ba.log | head; exit 1
fi
grep -q 'DZ_MPLIVE build' /tmp/dzlive_ba.log || echo "WARN: DZ_MPLIVE flag not echoed by build_all"
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

SER=build_test/dzlive_ser.log; rm -f "$SER"
echo "[dzlive] booting (90s)..."
timeout 90 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot 2>/dev/null

echo "=== DeadZone MP markers ==="
grep -aE 'DZCLIENT:|DEADZONED:|DEADZONE: mp' "$SER" | head -30

LISTEN=0; grep -qaE 'DEADZONED: listening on' "$SER" && LISTEN=1
JOINED=0; grep -qaE 'DEADZONED: client joined' "$SER" && JOINED=1
PASSLN=$(grep -aoE 'DEADZONE: mp LIVE PASS[^\n]*' "$SER" | head -1)
LIVE=0; [ -n "$PASSLN" ] && LIVE=1

echo ""
echo "listening=$LISTEN client_joined=$JOINED live_pass=$LIVE"
[ -n "$PASSLN" ] && echo "  $PASSLN"

if [ "$LISTEN" = "1" ] && [ "$JOINED" = "1" ] && [ "$LIVE" = "1" ]; then
  echo "DZ-MP-LIVE: PASS (server hosted, client connected over loopback, authoritative co-op proven)"
  exit 0
else
  echo "DZ-MP-LIVE: FAIL (listening=$LISTEN joined=$JOINED live=$LIVE)"
  echo "--- all DeadZone lines ---"; grep -aE 'DEADZONE|deadzone|dzclient|DZ_MPLIVE' "$SER" | head -40
  echo "--- tail ---"; tail -20 "$SER"
  exit 1
fi
