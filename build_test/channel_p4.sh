#!/bin/bash
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
echo "=== kernel (channel P0-P3, T410-safe) ==="
T410_SAFE=1 SCHED_DEBUG=0 bash scripts/quick_build.sh > /tmp/p4k.log 2>&1
grep -q "SUCCESS: build/kernel.elf" /tmp/p4k.log && echo "kernel OK ($(stat -c%s build/kernel.elf) B)" || { echo "KERNEL FAIL"; tail /tmp/p4k.log; exit 1; }
echo "=== build_all (userspace + ISO) -- slow ==="
bash scripts/build_all.sh > /tmp/p4all.log 2>&1
echo "--- terminal/channel compile check ---"
grep -iE "error:|undefined reference|channel\.h" /tmp/p4all.log | head -15
if grep -qiE "error:|undefined reference" /tmp/p4all.log; then echo "!! build_all ERRORS (see /tmp/p4all.log)"; else echo "build_all: no error:/undefined"; fi
ls -la build/automationos.iso 2>/dev/null
echo "=== screenshot ==="
bash build_test/shot.sh build/automationos.iso p4demo
echo "=== serial markers ==="
printf "desktop=%s panic=%s CHAN=%s\n" "$(grep -ciF desktop /tmp/shot_p4demo.log 2>/dev/null)" "$(grep -ciF PANIC /tmp/shot_p4demo.log 2>/dev/null)" "$(grep -cF '[CHAN]' /tmp/shot_p4demo.log 2>/dev/null)"
grep -F "[CHAN]" /tmp/shot_p4demo.log 2>/dev/null | head -3
