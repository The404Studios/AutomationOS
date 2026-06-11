#!/bin/bash
set -e
cd /tmp/htest

FLAGS="-std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2"

gcc $FLAGS -c userspace/lib/html/html_parse.c -o html_parse.o
gcc $FLAGS -c userspace/lib/css/css.c -o css.o
gcc $FLAGS -c userspace/lib/layout/layout.c -o layout.o

echo "=== Per-object fs:0x28 (stack canary) counts ==="
for o in html_parse.o css.o layout.o; do
  c=$(objdump -d $o | grep -c 'fs:0x28' || true)
  echo "$o: $c"
done

echo "=== Per-object __stack_chk references ==="
for o in html_parse.o css.o layout.o; do
  c=$(objdump -dr $o | grep -c '__stack_chk' || true)
  echo "$o: $c"
done

echo "=== Sizes ==="
ls -la html_parse.o css.o layout.o
