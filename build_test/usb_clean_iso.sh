#!/bin/bash
# Build automationos-t410-usb-clean.iso = KNOWN-GOOD userspace (FIXED.iso's
# initrd) + the USB_UHCI kernel ONLY. Isolates USB-MOUSE-0 from the in-progress
# IDE-PROJECT-0 desktop regression (path-titles / green stray window / lag) so
# USB can be judged on a clean desktop.
#
# Mechanism: the userspace is a SEPARATE grub module (iso/boot/initrd.img), so
# we keep the current USB kernel.elf and swap ONLY initrd.img for FIXED's.
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel

FIXED=build/automationos-t410-FIXED.iso
OUT=build/automationos-t410-usb-clean.iso
STAGE=/tmp/usbclean_iso
FAILED=0

echo "=== 1) USB kernel built with FIXED's EXACT profile + USB only ==="
echo "    T410_SAFE=1 SCHED_DEBUG=0 USB_UHCI=1  (no yellow on-screen debug markers, Westmere-safe)"
T410_SAFE=1 SCHED_DEBUG=0 USB_UHCI=1 bash scripts/quick_build.sh > /tmp/usbclean_k.log 2>&1
grep -E "T410_SAFE build|Results:|Link OK|SUCCESS" /tmp/usbclean_k.log | tail -3
grep -qa "USB Mouse (UHCI)" build/kernel.elf && echo "  OK: USB kernel confirmed (UHCI strings present)" || { echo "  FAIL: not a USB kernel"; exit 1; }

echo "=== 2) extract KNOWN-GOOD initrd from FIXED.iso (6/7, pre-regression) ==="
rm -f /tmp/fixed_initrd.img
xorriso -osirrox on -indev "$FIXED" -extract /boot/initrd.img /tmp/fixed_initrd.img 2>/tmp/usbclean_xorriso.log
[ -s /tmp/fixed_initrd.img ] || { echo "  EXTRACT FAIL"; tail /tmp/usbclean_xorriso.log; exit 1; }
FISIZE=$(stat -c%s /tmp/fixed_initrd.img)
CURSIZE=$(stat -c%s iso/boot/initrd.img)
echo "  FIXED initrd = $FISIZE bytes ; current(regressed) initrd = $CURSIZE bytes"
[ "$FISIZE" != "$CURSIZE" ] && echo "  (sizes differ -> confirmed a different, older userspace)" || echo "  (sizes equal -> same userspace; check dates)"

echo "=== 3) stage: current grub + USB kernel + FIXED initrd ==="
rm -rf "$STAGE"; mkdir -p "$STAGE/boot"
cp -r iso/boot/grub "$STAGE/boot/grub"
cp build/kernel.elf "$STAGE/boot/kernel.elf"
cp /tmp/fixed_initrd.img "$STAGE/boot/initrd.img"
echo "  staged kernel=$(stat -c%s $STAGE/boot/kernel.elf) (USB)  initrd=$(stat -c%s $STAGE/boot/initrd.img) (FIXED)"

echo "=== 4) grub-mkrescue ==="
grub-mkrescue -o "$OUT" "$STAGE" > /tmp/usbclean_iso.log 2>&1 || { echo "  ISO FAIL"; tail /tmp/usbclean_iso.log; exit 1; }
ls -la "$OUT"

# --- boot validation (serial markers; visual confirmation is the T410 step) ---
assert()        { if grep -qiF "$1" "$2"; then echo "  PASS: $3"; else echo "  FAIL: $3 (want '$1')"; FAILED=1; fi; }
assert_absent() { if grep -qiF "$1" "$2"; then echo "  FAIL: $3 (unexpected '$1')"; FAILED=1; else echo "  PASS: $3"; fi; }
run_qemu() { tag="$1"; shift; timeout 55 qemu-system-x86_64 -cdrom "$OUT" -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 -serial "file:/tmp/usbclean_$tag.log" -display none "$@" >/dev/null 2>&1; sleep 1; }

echo "=== 5) boot CASE A (no USB) ==="
run_qemu A
assert        "desktop" /tmp/usbclean_A.log "A: reached desktop"
assert_absent "PANIC"   /tmp/usbclean_A.log "A: no panic"
grep -F "[UHCI]" /tmp/usbclean_A.log | head -3 | sed 's/^/    /'

echo "=== 6) boot CASE C (-usb -device usb-mouse) ==="
run_qemu C -usb -device usb-mouse
assert        "Registered input device" /tmp/usbclean_C.log "C: USB mouse enumerated + registered"
assert        "desktop"                  /tmp/usbclean_C.log "C: reached desktop"
assert_absent "PANIC"                    /tmp/usbclean_C.log "C: no panic"
grep -F "[UHCI]" /tmp/usbclean_C.log | head -10 | sed 's/^/    /'

echo "=== RESULT ==="
[ "$FAILED" = "0" ] && { echo "USB-CLEAN ISO: PASS (known-good userspace + USB kernel; both boots clean)"; exit 0; } || { echo "USB-CLEAN ISO: FAIL"; exit 1; }
