#!/bin/bash
cd /mnt/c/Users/wilde/Desktop/Kernel
T410_SAFE=1 SCHED_DEBUG=0 bash scripts/build_all.sh > /tmp/ba410.log 2>&1
echo "build_all exit=$?"
grep -nE "error:|undefined|FAIL|T410_SAFE" /tmp/ba410.log | head -20
tail -4 /tmp/ba410.log
ls -la build/automationos.iso
