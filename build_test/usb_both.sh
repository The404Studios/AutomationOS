#!/bin/bash
cd /mnt/c/Users/wilde/Desktop/Kernel
echo "=== USB_UHCI=1 build (gated, init+poll wired) ==="
USB_UHCI=1 bash scripts/quick_build.sh > /tmp/usb1.log 2>&1
grep -E "USB_UHCI build|uhci.c|c_pit|c_kernel|Link OK|LINK FAILED|Results:|SUCCESS|error:|undefined" /tmp/usb1.log | head -12
echo "=== default build (USB OFF -> #ifdefs vanish, unchanged) ==="
bash scripts/quick_build.sh > /tmp/usb0.log 2>&1
grep -E "Link OK|LINK FAILED|Results:|SUCCESS|error:|undefined" /tmp/usb0.log | head -6
