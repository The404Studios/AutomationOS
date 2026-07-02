#!/bin/bash
cd /mnt/c/Users/wilde/Desktop/Kernel
echo "=== build USB_UHCI=1 kernel + stage into a SEPARATE usb ISO ==="
USB_UHCI=1 bash scripts/quick_build.sh > /tmp/usbk.log 2>&1
grep -E "Results:|SUCCESS|LINK FAILED" /tmp/usbk.log
cp build/kernel.elf iso/boot/kernel.elf
grub-mkrescue -o build/automationos-t410-usb.iso iso/ > /tmp/usbiso.log 2>&1 && echo "USB ISO OK" || { echo "ISO FAIL"; tail /tmp/usbiso.log; exit 1; }
ls -la build/automationos-t410-usb.iso

run_case() {
  tag="$1"; shift
  timeout 55 qemu-system-x86_64 -cdrom build/automationos-t410-usb.iso -m 512 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:/tmp/usb_$tag.log" -display none "$@" >/dev/null 2>&1
  sleep 2
  echo "--- case $tag : [UHCI] log ---"
  grep -F "[UHCI]" "/tmp/usb_$tag.log" | head -16
  echo "--- case $tag : markers ---"
  printf 'desktop=%s compositor=%s PIDs=%s PANIC=%s pagefault=%s\n' \
    "$(grep -ciF desktop /tmp/usb_$tag.log)" "$(grep -ciF compositor /tmp/usb_$tag.log)" \
    "$(grep -ciF 'Created with PID' /tmp/usb_$tag.log)" "$(grep -ciF PANIC /tmp/usb_$tag.log)" \
    "$(grep -ciF 'Page Fault' /tmp/usb_$tag.log)"
}

echo "=== CASE A: no USB hardware (no -usb) -> uhci_init finds no controller, boots clean ==="
run_case A
echo "=== CASE C: -usb -device usb-mouse -> controller + mouse enumerated, boots clean ==="
run_case C -usb -device usb-mouse
echo "=== DONE ==="
