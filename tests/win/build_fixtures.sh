#!/bin/bash
# build_fixtures.sh <outdir> -- build the Windows test programs for WIN-MIN (needs mingw-w64 on the host).
#   nocrt_hello.exe   tier 1: no C runtime, 3 kernel32 imports        (expect "WINFIX: nocrt hello", exit 42)
#   crt_hello.exe     tier 2: ordinary MinGW program with the CRT     (measures the remaining import gap)
#   bad_trunc.exe     a valid header cut short                        (must be REJECTED, never run)
#   bad_magic.exe     512 bytes that are not a PE                     (must be REJECTED)
#   bad_machine.exe   a real PE with Machine patched to ARM64         (must be REJECTED)
set -e
OUT="${1:?usage: build_fixtures.sh <outdir>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
CC=x86_64-w64-mingw32-gcc
command -v "$CC" >/dev/null || { echo "build_fixtures: $CC not found (install mingw-w64-gcc)"; exit 2; }
mkdir -p "$OUT"
cd "$HERE"
$CC -O2 -nostdlib -ffreestanding -fno-stack-protector -Wl,-e,start -Wl,--subsystem,console \
    -Wl,--dynamicbase -Wl,--enable-reloc-section -o "$OUT/nocrt_hello.exe" nocrt_hello.c -lkernel32
$CC -O2 -o "$OUT/crt_hello.exe" crt_hello.c
# The tier-1 proof only means something if the image CAN be relocated: refuse a fixture with an empty
# base-relocation directory (an earlier build silently had none, so relocation was never exercised).
python3 - "$OUT/nocrt_hello.exe" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]
rva, size = struct.unpack_from("<II", d, pe + 24 + 112 + 5 * 8)     # PE32+ DataDirectory[5] = base relocations
print("build_fixtures: nocrt_hello.exe base-reloc directory rva=0x%x size=%d" % (rva, size))
sys.exit(0 if size else "build_fixtures: nocrt_hello.exe has NO base relocations -- the proof would not exercise relocation")
PY
head -c 300 "$OUT/nocrt_hello.exe" > "$OUT/bad_trunc.exe"
head -c 512 /dev/zero > "$OUT/bad_magic.exe"
python3 - "$OUT/nocrt_hello.exe" "$OUT/bad_machine.exe" <<'PY'
import struct, sys
d = bytearray(open(sys.argv[1], "rb").read())
pe = struct.unpack_from("<I", d, 0x3C)[0]
struct.pack_into("<H", d, pe + 4, 0xAA64)          # IMAGE_FILE_MACHINE_ARM64
open(sys.argv[2], "wb").write(d)
PY
ls -l "$OUT"
