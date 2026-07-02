#!/bin/bash
cd /mnt/c/Users/wilde/Desktop/Kernel
CF="-std=gnu11 -ffreestanding -nostdlib -nostdinc -fno-pic -fno-pie -fno-stack-protector -mno-red-zone -mcmodel=kernel -DBOOT_QUIET -Wno-builtin-declaration-mismatch -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types -Ikernel/include -Ikernel/include/compat"
for f in usb_core uhci hid; do
  gcc $CF -c kernel/drivers/usb/$f.c -o /tmp/u_$f.o 2>/tmp/eu_$f.txt
  echo "=== $f.c rc=$? errors=$(grep -cE 'error:' /tmp/eu_$f.txt) ==="
  grep -E "error:" /tmp/eu_$f.txt | head -8
done
