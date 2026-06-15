#!/bin/bash
# SMP-THREAD-INHERIT-0 full soak: the threadinherit smoke (rebuilds the atomic
# kernel + default byte-identity + a 30-minute -smp 2 boot). The world (initrd
# with threadprobe) is already current. Writes /tmp/ti_run.log, ends with the
# TI_RUN_COMPLETE sentinel.
cd /mnt/c/Users/wilde/Desktop/Kernel
for f in scripts/quick_build.sh scripts/threadinherit_smoke.sh; do sed -i 's/\r$//' "$f"; done
bash scripts/threadinherit_smoke.sh > /tmp/ti_run.log 2>&1
echo TI_RUN_COMPLETE >> /tmp/ti_run.log
