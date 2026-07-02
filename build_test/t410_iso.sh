#!/bin/bash
# Build the narrow T410 recovery ISO from the current hardened branch under the
# T410_SAFE profile, ensuring the freshly-built kernel actually lands in the ISO.
cd /mnt/c/Users/wilde/Desktop/Kernel
set -o pipefail

echo "=== [1/4] build kernel: T410_SAFE=1 SCHED_DEBUG=0 (ERMS off, sched-debug off, quiet) ==="
T410_SAFE=1 SCHED_DEBUG=0 bash scripts/quick_build.sh > /tmp/t410_build.log 2>&1
grep -E "T410_SAFE build|Results:|SUCCESS|FAILED: No|LINK FAILED" /tmp/t410_build.log
if ! grep -q "SUCCESS: build/kernel.elf" /tmp/t410_build.log; then
  echo "BUILD FAILED"; tail -20 /tmp/t410_build.log; exit 1
fi

echo "=== [2/4] stage freshly-built kernel into iso/boot/ (the step smoke_boot.sh omits) ==="
cp -v build/kernel.elf iso/boot/kernel.elf
ls -la build/kernel.elf iso/boot/kernel.elf

echo "=== [3/4] grub-mkrescue -> build/automationos-t410.iso ==="
grub-mkrescue -o build/automationos-t410.iso iso/ > /tmp/t410_iso.log 2>&1 && echo "ISO OK" || { echo "ISO FAILED"; tail -20 /tmp/t410_iso.log; exit 1; }
ls -la build/automationos-t410.iso

echo "=== [4/4] boot-smoke the T410 ISO directly (no --build, tests exactly this image) ==="
bash scripts/smoke_boot.sh --iso build/automationos-t410.iso > /tmp/t410_smoke.log 2>&1
echo "--- smoke result ---"
grep -E "Passed:|Failed:|RESULT" /tmp/t410_smoke.log | head -4
echo "--- boot-progression markers reached ---"
for m in 'GDT' 'RTC' 'PAGINGALIAS' 'slab' 'All services started' 'scheduler' 'init' 'desktop' 'compositor' 'Created with PID' 'AUTOSTART'; do
  printf '%-26s %s\n' "$m" "$(grep -ciF "$m" /tmp/smoke_boot.log 2>/dev/null)"
done
