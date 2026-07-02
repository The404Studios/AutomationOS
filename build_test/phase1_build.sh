#!/bin/bash
cd /mnt/c/Users/wilde/Desktop/Kernel
echo "=== build_all.sh ==="
bash scripts/build_all.sh > /tmp/ba.log 2>&1
echo "build_all exit=$?"
echo "=== IDE / compositor / link lines ==="
grep -nE "ide.elf|comp.elf|cc.elf|undefined|error:|FAIL" /tmp/ba.log | head -40
echo "=== canary check ==="
grep -nE -A12 "canary check" /tmp/ba.log | head -20
echo "=== tail ==="
tail -8 /tmp/ba.log
