#!/bin/bash
# USB-MOUSE-0 smoke harness (commit 5/5).
# Regression gate for the gated UHCI boot-mouse: proves a USB_UHCI=1 kernel
#   (A) boots clean with NO USB controller present (no hang, graceful "no controller"), and
#   (C) enumerates ONE wired boot-protocol HID mouse and still boots clean.
# Both cases must reach the desktop with zero PANIC. A hang during enumeration
# would freeze before desktop -> desktop=0 -> FAIL (that is the regression we guard).
#
# Usage:  MSYS_NO_PATHCONV=1 wsl.exe -d Arch bash /mnt/c/Users/wilde/Desktop/Kernel/build_test/usb_smoke.sh
# Exit:   0 = both cases PASS, 1 = any assert failed (CI-friendly).
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel

ISO=build/automationos-t410-usb.iso
FAILED=0

echo "=== [1/3] build USB_UHCI=1 kernel ==="
USB_UHCI=1 bash scripts/quick_build.sh > /tmp/usb_smoke_build.log 2>&1
if ! grep -qE "Link OK|LINK OK|SUCCESS" /tmp/usb_smoke_build.log; then
  echo "FAIL: USB_UHCI=1 kernel did not link"; tail -20 /tmp/usb_smoke_build.log; exit 1
fi
grep -E "Results:|compiled|Link OK|LINK OK" /tmp/usb_smoke_build.log | tail -3

echo "=== [2/3] stage into a SEPARATE usb ISO (stable t410 ISO stays untouched) ==="
cp build/kernel.elf iso/boot/kernel.elf
if ! grub-mkrescue -o "$ISO" iso/ > /tmp/usb_smoke_iso.log 2>&1; then
  echo "FAIL: grub-mkrescue"; tail -10 /tmp/usb_smoke_iso.log; exit 1
fi
ls -la "$ISO"

# assert PATTERN LOGFILE LABEL  -> increments FAILED unless PATTERN present
assert() {
  if grep -qiF "$1" "$2"; then
    echo "  PASS: $3"
  else
    echo "  FAIL: $3   (expected to find: '$1')"; FAILED=1
  fi
}
# assert_absent PATTERN LOGFILE LABEL  -> FAIL if PATTERN present
assert_absent() {
  if grep -qiF "$1" "$2"; then
    echo "  FAIL: $3   (unexpected: '$1')"; FAILED=1
  else
    echo "  PASS: $3"
  fi
}

run_qemu() {
  tag="$1"; shift
  timeout 55 qemu-system-x86_64 -cdrom "$ISO" -m 512 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:/tmp/usb_smoke_$tag.log" -display none "$@" >/dev/null 2>&1
  sleep 1
}

echo "=== [3/3] boot cases ==="

echo "--- CASE A: no USB hardware (no -usb) ---"
run_qemu A
LOG=/tmp/usb_smoke_A.log
echo "  [UHCI] log:"; grep -F "[UHCI]" "$LOG" | head -6 | sed 's/^/    /'
assert        "No UHCI controller"  "$LOG" "A: graceful no-controller path taken"
assert        "desktop"             "$LOG" "A: reached desktop (no enumeration hang)"
assert_absent "PANIC"               "$LOG" "A: no panic"

echo "--- CASE C: -usb -device usb-mouse ---"
run_qemu C -usb -device usb-mouse
LOG=/tmp/usb_smoke_C.log
echo "  [UHCI] log:"; grep -F "[UHCI]" "$LOG" | head -10 | sed 's/^/    /'
assert        "Controller"            "$LOG" "C: UHCI controller found"
assert        "Registered input device" "$LOG" "C: HID mouse registered into shared input layer"
assert        "desktop"               "$LOG" "C: reached desktop"
assert_absent "PANIC"                 "$LOG" "C: no panic"

echo "=== RESULT ==="
if [ "$FAILED" = "0" ]; then
  echo "USB SMOKE: PASS (no-device boots clean; boot-mouse enumerates; neither hangs)"
  exit 0
else
  echo "USB SMOKE: FAIL"
  exit 1
fi
