#!/usr/bin/env bash
# run_res_tests.sh -- build + run the host-side tests for userspace/lib/pe/pe_resources.[ch] and pe_manifest.[ch]
# (PE resource directory walker, VS_VERSIONINFO reader, application-manifest scanner).  Host only: needs gcc and
# MinGW (x86_64-w64-mingw32-gcc + x86_64-w64-mingw32-windres) for the real fixture.
#
#   wsl.exe -d Arch -- bash /mnt/c/Users/wilde/Desktop/Kernel/tests/win/run_res_tests.sh
#
# Prints (and exits nonzero unless ALL of them are produced):
#   RES-FREESTANDING-BUILD: PASS   pe.c + pe_resources.c + pe_manifest.c compile freestanding, -Werror, link with NO undefined
#                                  symbols, no stack canary (fs:0x28), under both the spec flags and the kernel-style flags
#   RES-HOST-TEST: PASS checks=N   exact-value tests on a real windres/MinGW fixture + hand-built hostile images, under ASan+UBSan
#   RES-FUZZ: PASS iterations=N    300000-iteration deterministic mutation fuzz (resource tree, manifest text, version blobs,
#                                  dependency hints), under ASan+UBSan
#   RES-FUZZ-HARNESS-SELFTEST: PASS  the fuzzer aborts and saves the failing input when (a) the harness itself overruns / overflows
#                                  and (b) FIVE different bugs are planted in the library (-DPE_MAN_INJECT_BUG=1..3,
#                                  -DPE_RES_INJECT_BUG=1..2)
#
# Environment knobs: RES_WORKDIR (default /tmp/res_tests_work), RES_FUZZ_ITERS, RES_FUZZ_SEED, CC, MINGW_CC, MINGW_WINDRES,
# RES_SELFTEST_ITERS (iterations allowed per planted-bug run, default 200000).  Nothing under the repo is modified; every build
# product goes to $RES_WORKDIR.  An optional real-binary scan is a separate script (tests/win/res_corpus.sh).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC=$ROOT/userspace/lib/pe
WORK=${RES_WORKDIR:-/tmp/res_tests_work}
FIX=$WORK/fix
CC=${CC:-gcc}
MINGW=${MINGW_CC:-x86_64-w64-mingw32-gcc}
WINDRES=${MINGW_WINDRES:-x86_64-w64-mingw32-windres}
SEED=${RES_FUZZ_SEED:-0x9E3779B97F4A7C15}
CRASH=$WORK/res_fuzz_crash.bin

fail() { echo "FAIL: $*" >&2; exit 1; }

mkdir -p "$FIX" "$WORK/hostile" "$WORK/gate" || fail "cannot create $WORK"

# ---------------------------------------------------------------------------------------------------------------
# 1. fixture: tiny no-CRT MinGW program carrying res_good.rc (manifest, version, multi-language, named resources)
# ---------------------------------------------------------------------------------------------------------------
command -v "$MINGW" >/dev/null 2>&1 || fail "$MINGW not found (install mingw-w64-gcc)"
command -v "$WINDRES" >/dev/null 2>&1 || fail "$WINDRES not found (install mingw-w64-binutils)"
"$WINDRES" -O coff -I "$HERE" -i "$HERE/res_good.rc" -o "$FIX/res_good.res.o" || fail "windres failed on res_good.rc"
"$MINGW" -O2 -nostdlib -ffreestanding -fno-stack-protector -Wl,-e,start -Wl,--subsystem,console \
    -Wl,--dynamicbase -Wl,--enable-reloc-section -o "$FIX/res_hello.exe" "$HERE/res_hello.c" "$FIX/res_good.res.o" -lkernel32 \
    || fail "cannot link res_hello.exe"
echo "[fixtures] res_hello.exe ($(wc -c < "$FIX/res_hello.exe") bytes) built with $WINDRES + $MINGW"

# ---------------------------------------------------------------------------------------------------------------
# 2. freestanding build gate
# ---------------------------------------------------------------------------------------------------------------
FSFLAGS="-std=gnu11 -ffreestanding -nostdlib -fno-builtin -O2 -Wall -Wextra -Werror"
for variant in plain kernel; do
    if [ "$variant" = plain ]; then X=""; else X="-fno-stack-protector -fno-pic -fno-pie -mno-red-zone"; fi
    objs=""
    for f in pe pe_resources pe_manifest; do
        "$CC" $FSFLAGS $X -I"$SRC" -c "$SRC/$f.c" -o "$WORK/gate/${f}_$variant.o" || fail "$f.c does not build freestanding ($variant flags)"
        if objdump -d "$WORK/gate/${f}_$variant.o" | grep -q 'fs:0x28'; then fail "$f.c ($variant) contains a stack-protector canary (fs:0x28)"; fi
        objs="$objs $WORK/gate/${f}_$variant.o"
    done
    ld -r -o "$WORK/gate/all_$variant.o" $objs || fail "partial link failed ($variant)"
    UNDEF=$(nm -u "$WORK/gate/all_$variant.o" 2>&1)
    [ -z "$UNDEF" ] || fail "freestanding library ($variant) has undefined symbols (needs libc?): $UNDEF"
done
echo "RES-FREESTANDING-BUILD: PASS"

# ---------------------------------------------------------------------------------------------------------------
# 3. host test
# ---------------------------------------------------------------------------------------------------------------
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
export ASAN_OPTIONS=${ASAN_OPTIONS:-strict_string_checks=1:detect_stack_use_after_return=1:abort_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:abort_on_error=1}
LIB="$SRC/pe.c $SRC/pe_resources.c $SRC/pe_manifest.c"

"$CC" -std=gnu11 -O1 -g $SAN -Wall -Wextra -I"$SRC" -I"$HERE" -o "$WORK/res_host_test" "$HERE/res_host_test.c" $LIB \
    || fail "cannot build res_host_test"
RES_DUMP_DIR="$WORK/hostile" "$WORK/res_host_test" "$FIX/res_hello.exe" "$HERE/res_good.manifest" > "$WORK/host_test.out" 2>&1
HT=$?
cat "$WORK/host_test.out"
[ $HT -eq 0 ] || fail "res_host_test exited with status $HT"
grep -q '^RES-HOST-TEST: PASS' "$WORK/host_test.out" || fail "res_host_test did not print RES-HOST-TEST: PASS"

# ---------------------------------------------------------------------------------------------------------------
# 4. fuzz
# ---------------------------------------------------------------------------------------------------------------
"$CC" -std=gnu11 -O1 -g $SAN -Wall -Wextra -I"$SRC" -I"$HERE" -o "$WORK/res_fuzz" "$HERE/res_fuzz.c" $LIB || fail "cannot build res_fuzz"
rm -f "$CRASH"
RES_FUZZ_CRASH="$CRASH" "$WORK/res_fuzz" "$SEED" "$FIX/res_hello.exe" "$HERE/res_good.manifest" > "$WORK/fuzz.out" 2> "$WORK/fuzz.err"
FZ=$?
cat "$WORK/fuzz.err"
cat "$WORK/fuzz.out"
if [ $FZ -ne 0 ]; then
    [ -f "$CRASH" ] && echo "failing input: $CRASH ($(wc -c < "$CRASH") bytes)" >&2
    fail "res_fuzz exited with status $FZ"
fi
grep -q '^RES-FUZZ: PASS iterations=' "$WORK/fuzz.out" || fail "res_fuzz did not print RES-FUZZ: PASS"

# ---------------------------------------------------------------------------------------------------------------
# 5. prove the harness itself
# ---------------------------------------------------------------------------------------------------------------
# (a) the harness' own deliberate overrun / signed overflow must abort AND leave the input in $CRASH
for inj in RES_FUZZ_INJECT RES_FUZZ_INJECT_UB; do
    rm -f "$CRASH"
    { env "$inj=7" RES_FUZZ_ITERS=200 RES_FUZZ_CRASH="$CRASH" "$WORK/res_fuzz" "$SEED" "$FIX/res_hello.exe" "$HERE/res_good.manifest" > /dev/null 2> "$WORK/selftest.err"; } 2>/dev/null
    ST=$?
    if [ $ST -eq 0 ] || [ ! -s "$CRASH" ]; then
        cat "$WORK/selftest.err" >&2
        fail "fuzz harness self-test ($inj): exit=$ST, crash file $( [ -s "$CRASH" ] && echo present || echo MISSING )"
    fi
done

# (b) deliberately plant real bugs IN THE LIBRARY: the fuzzer must catch every one
SELF_ITERS=${RES_SELFTEST_ITERS:-200000}
selftest_lib() {   # <label> <-D flag> <min-iteration-note>
    local label=$1 flag=$2
    "$CC" -std=gnu11 -O1 -g $SAN -Wall -Wextra "$flag" -I"$SRC" -I"$HERE" -o "$WORK/res_fuzz_inj" "$HERE/res_fuzz.c" $LIB \
        || fail "cannot build the planted-bug fuzzer ($label)"
    rm -f "$CRASH"
    { env RES_FUZZ_ITERS="$SELF_ITERS" RES_FUZZ_CRASH="$CRASH" "$WORK/res_fuzz_inj" "$SEED" "$FIX/res_hello.exe" "$HERE/res_good.manifest" \
        > /dev/null 2> "$WORK/selftest_lib.err"; } 2>/dev/null
    local st=$?
    if [ $st -eq 0 ] || [ ! -s "$CRASH" ]; then
        tail -20 "$WORK/selftest_lib.err" >&2
        fail "planted library bug NOT detected ($label): exit=$st, crash file $( [ -s "$CRASH" ] && echo present || echo MISSING )"
    fi
    echo "[selftest] planted bug detected: $label -> $(grep -o 'iteration [0-9]*, target [A-Z]*' "$WORK/selftest_lib.err" | head -1) ($(grep -o 'ERROR: AddressSanitizer: [a-z-]*\|runtime error: [a-z ]*\|invariant violated[^:]*' "$WORK/selftest_lib.err" | head -1))"
}
selftest_lib "manifest bug 1 (read one unit past the end)"   -DPE_MAN_INJECT_BUG=1
selftest_lib "manifest bug 2 (nesting-depth cap removed)"    -DPE_MAN_INJECT_BUG=2
selftest_lib "manifest bug 3 (entity-name cap removed)"      -DPE_MAN_INJECT_BUG=3
selftest_lib "resources bug 1 (name-string bounds removed)"  -DPE_RES_INJECT_BUG=1
selftest_lib "resources bug 2 (version-node bound removed)"  -DPE_RES_INJECT_BUG=2
rm -f "$CRASH"
echo "RES-FUZZ-HARNESS-SELFTEST: PASS"

echo "RES-TESTS: ALL PASS"
exit 0
