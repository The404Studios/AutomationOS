#!/usr/bin/env bash
# Build + verify the Image Viewer freestanding ELF (run under WSL ELF gcc).
# Compiles imageviewer.c + de-POSIX'd image module + wl_client + bitfont + the
# needed libc objects, links with the userspace linker script, then checks:
#   - no fs:0x28 stack-canary references  (objdump)
#   - no undefined symbols                (nm -u)
set -euo pipefail

K=/mnt/c/Users/wilde/Desktop/Kernel
US="$K/userspace"
OUT=/tmp/ivbuild
rm -rf "$OUT"
mkdir -p "$OUT"
cd "$OUT"

# Flags passed DIRECTLY (never via a shell var that could drop -fno-stack-protector).
# -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 stops Ubuntu/Arch gcc's hardened string.h
# from rewriting memcpy/memset into __memcpy_chk/__memset_chk (glibc-only symbols
# that don't exist in our -nostdlib link).
compile() {
  gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
      -fno-pic -fno-pie -mno-red-zone -O2 \
      -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 "$@"
}

echo "== compile app =="
compile -DIMAGE_FREESTANDING -c "$US/apps/imageviewer/imageviewer.c" -o imageviewer.o
echo "== compile image module (de-POSIX'd) =="
compile -DIMAGE_FREESTANDING -c "$US/lib/image/image.c" -o image.o
echo "== compile wl_client =="
compile -c "$US/lib/wl/wl_client.c" -o wl_client.o
echo "== compile bitfont =="
compile -c "$US/lib/font/bitfont.c" -o bitfont.o
echo "== compile libc objects (stdlib/string/math/syscall) =="
compile -c "$US/libc/stdlib.c"  -o stdlib.o
compile -c "$US/libc/string.c"  -o string.o
compile -c "$US/libc/math.c"    -o math.o
compile -c "$US/libc/syscall.c" -o syscall.o

echo "== link =="
ld -nostdlib -static -n -no-pie -e _start -T "$US/userspace.ld" \
   imageviewer.o image.o wl_client.o bitfont.o \
   stdlib.o string.o math.o syscall.o \
   -o /tmp/iv.elf
echo "LINK OK -> /tmp/iv.elf"

echo "== check: fs:0x28 stack canary (must be EMPTY) =="
if objdump -d /tmp/iv.elf | grep -n 'fs:0x28'; then
  echo "FAIL: stack-canary reference present"; exit 1
else
  echo "PASS: no fs:0x28 references"
fi

echo "== check: undefined symbols (nm -u, must be EMPTY) =="
if nm -u /tmp/iv.elf | grep -q .; then
  echo "FAIL: undefined symbols:"; nm -u /tmp/iv.elf; exit 1
else
  echo "PASS: no undefined symbols"
fi

echo "== artifact =="
ls -la /tmp/iv.elf
file /tmp/iv.elf || true
echo "ALL CHECKS PASSED"
