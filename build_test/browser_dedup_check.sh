#!/bin/bash
# BROWSER-DEDUP verify (both directions):
#   default build (no flag)      -> exactly ONE browser window, NO about:imgtest run
#   SMOKE_SELFTEST=1 build       -> the bounded self-tests run (BROWSER2-IMG marker present)
# Only init/main.c + build_all.sh changed (userspace) -> no kernel rebuild needed.
# Run: wsl -d Arch bash build_test/browser_dedup_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

boot() {  # $1=iso-tag serial-suffix
  local SER="build_test/dedup_$1_ser.log"; rm -f "$SER"
  timeout 80 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$SER" -display none -no-reboot 2>/dev/null
  echo "$SER"
}

# ---- direction 1: DEFAULT (no SMOKE_SELFTEST) => one browser -----------------
echo "[dedup] build_all DEFAULT (no flag)..."
bash scripts/build_all.sh > /tmp/dedup_def.log 2>&1
grep -qiE 'error:|undefined reference|unresolved' /tmp/dedup_def.log && { echo "BUILD ERRORS (default)"; grep -iE 'error:|undefined reference' /tmp/dedup_def.log | head; exit 1; }
[ -s build/automationos.iso ] || { echo "no iso (default)"; exit 1; }
echo "[dedup] booting DEFAULT (80s)..."
SER_DEF="$(boot def)"
UIREADY=$(grep -acE 'BROWSER2: ui ready' "$SER_DEF")
IMG=$(grep -acE 'BROWSER2-IMG:' "$SER_DEF")
RENDERED=$(grep -acE 'BROWSER2: (rendered [0-9]|BOUND)' "$SER_DEF")
echo "  default: ui_ready=$UIREADY imgtest=$IMG rendered=$RENDERED"

# ---- direction 2: SMOKE_SELFTEST=1 => self-tests run ------------------------
echo "[dedup] build_all SMOKE_SELFTEST=1..."
SMOKE_SELFTEST=1 bash scripts/build_all.sh > /tmp/dedup_smoke.log 2>&1
grep -qiE 'error:|undefined reference|unresolved' /tmp/dedup_smoke.log && { echo "BUILD ERRORS (smoke)"; grep -iE 'error:|undefined reference' /tmp/dedup_smoke.log | head; exit 1; }
echo "[dedup] booting SMOKE_SELFTEST (80s)..."
SER_SM="$(boot smoke)"
IMG_SM=$(grep -acE 'BROWSER2-IMG:' "$SER_SM")
echo "  smoke:   imgtest=$IMG_SM"

# rebuild the default ISO so the tree is left in the shipping (one-browser) state
echo "[dedup] restoring DEFAULT iso..."
bash scripts/build_all.sh > /tmp/dedup_restore.log 2>&1

echo ""
echo "=== VERDICT ==="
echo "default: ui_ready=$UIREADY imgtest_windows=$IMG rendered=$RENDERED   smoke: imgtest=$IMG_SM"
# default: persistent browser is up (rendered>=1) with NO imgtest self-test window (IMG==0)
# smoke:   the gated self-test ran (IMG_SM>=1)
if [ "$IMG" = "0" ] && [ "$RENDERED" -ge 1 ] && [ "$IMG_SM" -ge 1 ]; then
  echo "BROWSER-DEDUP: PASS  one_browser_default=1 (no imgtest window)  selftests_under_flag=1"
  exit 0
else
  echo "BROWSER-DEDUP: CHECK  (expected default imgtest=0 rendered>=1, smoke imgtest>=1)"
  echo "--- default tail ---"; tail -15 "$SER_DEF"
  exit 1
fi
