#!/bin/bash
cd /mnt/c/Users/wilde/Desktop/Kernel
bash scripts/quick_build.sh > /tmp/qb.log 2>&1
grep -E "/pty_dev.c" /tmp/qb.log
grep -E "FAIL|error:|LINK FAILED" /tmp/qb.log | head -20
grep -E "Link OK|Results:|SUCCESS|FAILED: No" /tmp/qb.log
