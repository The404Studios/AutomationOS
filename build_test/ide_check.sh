#!/bin/bash
cd /mnt/c/Users/wilde/Desktop/Kernel
CF="-std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2"
for f in tc_driver ide_build ide_project ide; do
  gcc $CF -c userspace/apps/ide/$f.c -o /tmp/ide_$f.o 2>/tmp/e_$f.txt; rc=$?
  errs=$(grep -cE "error:" /tmp/e_$f.txt)
  echo "$f.c rc=$rc errors=$errs"
  grep -E "error:" /tmp/e_$f.txt | head -12
done
