#!/usr/bin/env bash
# build_dll_fixtures.sh <outdir> -- build the DLL-loader test fixtures with MinGW (freestanding, no CRT).
#
#   dll_lib_b.dll  leaf DLL            exports sub, b_id
#   dll_lib_a.dll  imports dll_lib_b   exports add, mul, ordfn(@7 NONAME), a_sub_twice, get_via_ptr,
#                                      fwd_add -> dll_lib_b.sub, fwd_k32 -> KERNEL32.SetLastError
#   dll_main.exe   imports dll_lib_a (name / ordinal / forwarder) + kernel32
#   dll_bad.dll    DllMain(ATTACH) returns FALSE; imports dll_lib_b
#   dll_main_bad.exe        imports dll_bad
#   dll_cyc_x.dll / dll_cyc_y.dll   import each other        dll_main_cyc.exe imports both
#   dll_main_missing.exe    imports foo from the non-existent nosuch.dll
# Import libraries are generated from the .def files with dlltool (so mutually-importing DLLs can be linked).
set -e
OUT="${1:?usage: build_dll_fixtures.sh <outdir>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
CC=${MINGW_CC:-x86_64-w64-mingw32-gcc}
DT=${MINGW_DLLTOOL:-x86_64-w64-mingw32-dlltool}
command -v "$CC" >/dev/null || { echo "build_dll_fixtures: $CC not found (install mingw-w64-gcc)"; exit 2; }
command -v "$DT" >/dev/null || { echo "build_dll_fixtures: $DT not found (install mingw-w64-binutils)"; exit 2; }
mkdir -p "$OUT"
cd "$OUT"
CF="-O2 -ffreestanding -nostdlib -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra"
LF="-Wl,--enable-reloc-section -Wl,--dynamicbase"

implib() { "$DT" -d "$HERE/$1.def" -l "lib$1.a" >/dev/null; }
dll()    { local n=$1; shift; "$CC" -shared $CF $LF -Wl,-e,DllMain -o "$n.dll" "$HERE/$n.c" "$HERE/$n.def" "$@" -L. -lkernel32; }
exe()    { local n=$1; shift; "$CC" $CF $LF -Wl,-e,start -Wl,--subsystem,console -o "$n.exe" "$HERE/$n.c" "$@" -L. -lkernel32; }

for n in dll_lib_a dll_lib_b dll_bad dll_cyc_x dll_cyc_y dll_nosuch; do implib $n; done

dll dll_lib_b
dll dll_lib_a -ldll_lib_b
dll dll_bad   -ldll_lib_b
dll dll_cyc_x -ldll_cyc_y
dll dll_cyc_y -ldll_cyc_x

exe dll_main         -ldll_lib_a
exe dll_main_bad     -ldll_bad
exe dll_main_cyc     -ldll_cyc_x -ldll_cyc_y
exe dll_main_missing -ldll_nosuch
ls -l "$OUT"/*.dll "$OUT"/*.exe
