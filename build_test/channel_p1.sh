#!/bin/bash
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
echo "=== build (CHANNEL-0 P0/P1) ==="
T410_SAFE=1 SCHED_DEBUG=0 bash scripts/quick_build.sh > /tmp/ch_build.log 2>&1
grep -E "c_channel|SUCCESS: build/kernel.elf|=== Results:|LINK FAILED" /tmp/ch_build.log | tail -4
if ! grep -q "SUCCESS: build/kernel.elf" /tmp/ch_build.log; then
  echo "BUILD FAIL:"; grep -iE "error:|undefined reference|FAIL:" /tmp/ch_build.log | head -25; exit 1
fi
echo "=== boot ==="
cp build/kernel.elf iso/boot/kernel.elf
grub-mkrescue -o build/automationos-ch.iso iso/ > /tmp/ch_iso.log 2>&1 || { echo "ISO FAIL"; tail /tmp/ch_iso.log; exit 1; }
timeout 50 qemu-system-x86_64 -cdrom build/automationos-ch.iso -m 512 -netdev user,id=n0 -device e1000,netdev=n0 -serial file:/tmp/ch_boot.log -display none >/dev/null 2>&1
sleep 1
echo "--- [CHAN] ---"; grep -F "[CHAN]" /tmp/ch_boot.log | head -3
printf "desktop=%s panic=%s\n" "$(grep -ciF desktop /tmp/ch_boot.log)" "$(grep -ciF PANIC /tmp/ch_boot.log)"
if grep -qF "[CHAN] selftest PASS" /tmp/ch_boot.log && [ "$(grep -ciF PANIC /tmp/ch_boot.log)" = "0" ] && [ "$(grep -ciF desktop /tmp/ch_boot.log)" -ge 1 ]; then
  echo "RESULT: CHANNEL-0 P0/P1 PASS"
else
  echo "RESULT: NEEDS REVIEW"; fi
