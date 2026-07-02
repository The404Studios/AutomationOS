#!/bin/bash
cd /mnt/c/Users/wilde/Desktop/Kernel
USB_UHCI=1 bash scripts/quick_build.sh > /tmp/usbqb.log 2>&1
echo "=== USB compile lines ==="
grep -E "usb_core|uhci|hid.c|USB_UHCI build" /tmp/usbqb.log
echo "=== failures / link ==="
grep -E "FAIL|error:|LINK FAILED|undefined" /tmp/usbqb.log | head -20
echo "=== result ==="
grep -E "Link OK|Results:|SUCCESS|FAILED: No" /tmp/usbqb.log
