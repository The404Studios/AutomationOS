#!/bin/bash
set -e
cd /tmp/htest

echo "=== syncing latest sources ==="
cp /mnt/c/Users/wilde/Desktop/Kernel/userspace/lib/html/html_parse.h userspace/lib/html/
cp /mnt/c/Users/wilde/Desktop/Kernel/userspace/lib/html/html_parse.c userspace/lib/html/
cp /mnt/c/Users/wilde/Desktop/Kernel/userspace/lib/css/css.h userspace/lib/css/
cp /mnt/c/Users/wilde/Desktop/Kernel/userspace/lib/css/css.c userspace/lib/css/
cp /mnt/c/Users/wilde/Desktop/Kernel/userspace/lib/layout/layout.h userspace/lib/layout/
cp /mnt/c/Users/wilde/Desktop/Kernel/userspace/lib/layout/layout.c userspace/lib/layout/

echo "=== compile (host, with warnings) ==="
HOSTFLAGS="-std=gnu11 -Wall -Wextra -Wno-unused-parameter -O2 -I."
gcc $HOSTFLAGS \
  userspace/lib/html/html_parse.c \
  userspace/lib/dom/dom.c \
  test_html.c -o test_html

gcc $HOSTFLAGS \
  userspace/lib/css/css.c \
  userspace/lib/dom/dom.c \
  test_css.c -o test_css

gcc $HOSTFLAGS \
  userspace/lib/html/html_parse.c \
  userspace/lib/css/css.c \
  userspace/lib/layout/layout.c \
  userspace/lib/dom/dom.c \
  test_layout.c -o test_layout

echo "=== run host selftests ==="
./test_html
./test_css
./test_layout

echo "=== compile freestanding (no libc, no canary) ==="
FREE="-std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 -Wall -Wextra -Wno-unused-parameter"
gcc $FREE -c userspace/lib/html/html_parse.c -o html_parse_free.o
gcc $FREE -c userspace/lib/css/css.c -o css_free.o
gcc $FREE -c userspace/lib/layout/layout.c -o layout_free.o

echo "=== canary check (objdump fs:0x28 must be 0) ==="
for o in html_parse_free.o css_free.o layout_free.o; do
  c=$(objdump -d $o | grep -c 'fs:0x28' || true)
  echo "$o: $c fs:0x28 refs"
done
for o in html_parse_free.o css_free.o layout_free.o; do
  c=$(objdump -dr $o | grep -c '__stack_chk' || true)
  echo "$o: $c __stack_chk refs"
done

echo "=== freestanding object sizes ==="
ls -la html_parse_free.o css_free.o layout_free.o
