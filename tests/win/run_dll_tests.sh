#!/usr/bin/env bash
# run_dll_tests.sh -- build + run the HOST-ONLY tests for the DLL loader library
#                     (userspace/lib/pe/pe_exports.c + pe_modules.c, on top of pe.c).
#
#   wsl.exe -d Arch -- bash /mnt/c/Users/wilde/Desktop/Kernel/tests/win/run_dll_tests.sh
#
# Prints (and exits nonzero unless ALL of them are produced):
#   PE-DLL-FIXTURES: PASS               MinGW fixtures (DLLs + exes, import libs via dlltool) built
#   PE-DLL-FREESTANDING-BUILD: PASS     pe.c + pe_exports.c + pe_modules.c compile freestanding (-Werror), link with no
#                                       undefined symbols and no stack canary (fs:0x28), plain and kernel-style flags
#   PE-DLL-HOST-TEST: PASS checks=N     export / forwarder / delay-import / module-graph tests, REAL MinGW DLLs executed
#                                       through ms_abi, under ASan + UBSan
#   PE-DLL-HOST-TEST-OSFLAGS: PASS ...  the same tests with the library compiled with the OS userspace flags (-O2 freestanding)
#   PE-DLL-FUZZ: PASS iterations=N ...  deterministic mutation fuzz of the export directory (+ the module loader), ASan+UBSan
#   PE-DLL-FUZZ-OSFLAGS: PASS ...       100000 more iterations against the OS-flag library build (invariants + oracle)
#   PE-DLL-FUZZ-HARNESS-SELFTEST: PASS  the fuzzer really aborts and saves its input on (a) an injected harness bug and
#                                       (b) a bounds check removed from a COPY of pe_exports.c
#
# Environment knobs: PE_DLL_WORKDIR (default /tmp/pe_dll_dev), CC, MINGW_CC, PE_DLL_FUZZ_ITERS (default 200000),
# PE_DLL_FUZZ_SEED.  Nothing under the repo is modified; every build product goes to $PE_DLL_WORKDIR (it never touches
# /tmp/ird, /tmp/*.o or any other shared build directory).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC=$ROOT/userspace/lib/pe
WORK=${PE_DLL_WORKDIR:-/tmp/pe_dll_dev}
FIX=$WORK/fix
CC=${CC:-gcc}
SEED=${PE_DLL_FUZZ_SEED:-0xD11F00D5EED5EED1}
ITERS=${PE_DLL_FUZZ_ITERS:-200000}
CRASH=$WORK/pe_dll_fuzz_crash.bin

fail() { echo "FAIL: $*" >&2; exit 1; }

mkdir -p "$WORK" || fail "cannot create $WORK"

# ---------------------------------------------------------------------------------------------------------------
# 1. fixtures (always rebuilt: they are tiny and the DLL sources are the test's ground truth)
# ---------------------------------------------------------------------------------------------------------------
rm -rf "$FIX"
bash "$HERE/dll_fix/build_dll_fixtures.sh" "$FIX" > "$WORK/fixtures.log" 2>&1 || { cat "$WORK/fixtures.log" >&2; fail "cannot build the MinGW fixtures"; }
for f in dll_lib_a.dll dll_lib_b.dll dll_main.exe dll_bad.dll dll_main_bad.exe dll_cyc_x.dll dll_cyc_y.dll dll_main_cyc.exe dll_main_missing.exe; do
    [ -s "$FIX/$f" ] || fail "fixture $f missing"
done
echo "PE-DLL-FIXTURES: PASS"

# ---------------------------------------------------------------------------------------------------------------
# 2. freestanding build gate
# ---------------------------------------------------------------------------------------------------------------
FSFLAGS="-std=gnu11 -ffreestanding -nostdlib -fno-builtin -O2 -Wall -Wextra -Werror"
FS=$WORK/fs
mkdir -p "$FS"
for variant in plain kernel; do
    # "kernel" variant == the OS userspace CF of scripts/build_all.sh (minus -O2/-std, set in FSFLAGS)
    if [ $variant = plain ]; then X=""; else X="-fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mstackrealign"; fi
    for f in pe pe_exports pe_modules; do
        "$CC" $FSFLAGS $X -c "$SRC/$f.c" -o "$FS/${f}_$variant.o" || fail "$f.c does not build freestanding ($variant flags)"
    done
    ld -r "$FS/pe_$variant.o" "$FS/pe_exports_$variant.o" "$FS/pe_modules_$variant.o" -o "$FS/all_$variant.o" || fail "ld -r ($variant)"
    UNDEF=$(nm -u "$FS/all_$variant.o" 2>&1)
    [ -z "$UNDEF" ] || fail "freestanding objects ($variant) have undefined symbols (needs libc?): $UNDEF"
    if objdump -d "$FS/all_$variant.o" | grep -q 'fs:0x28'; then fail "freestanding objects ($variant) contain a stack-protector canary (fs:0x28)"; fi
done
echo "PE-DLL-FREESTANDING-BUILD: PASS"

# ---------------------------------------------------------------------------------------------------------------
# 3. host test
# ---------------------------------------------------------------------------------------------------------------
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
export ASAN_OPTIONS=${ASAN_OPTIONS:-strict_string_checks=1:detect_stack_use_after_return=1:abort_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:abort_on_error=1}
LIBSRC="$SRC/pe.c $SRC/pe_exports.c $SRC/pe_modules.c"

"$CC" -std=gnu11 -O1 -g $SAN -Wall -Wextra -I"$SRC" -I"$HERE" -o "$WORK/pe_dll_test" "$HERE/pe_dll_test.c" $LIBSRC \
    || fail "cannot build pe_dll_test"
"$WORK/pe_dll_test" "$FIX" > "$WORK/host_test.out" 2>&1
HT=$?
cat "$WORK/host_test.out"
[ $HT -eq 0 ] || fail "pe_dll_test exited with status $HT"
grep -q '^PE-DLL-HOST-TEST: PASS' "$WORK/host_test.out" || fail "pe_dll_test did not print PE-DLL-HOST-TEST: PASS"
# ---------------------------------------------------------------------------------------------------------------
# 3b. the SAME tests against the library built with the OS's real userspace flags (scripts/build_all.sh CF: -O2,
#     freestanding, no canary, no PIC, -mno-red-zone, -mstackrealign): optimiser-dependent behaviour shows up here
# ---------------------------------------------------------------------------------------------------------------
OSCF="-std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2"
OSOBJ=""
for f in pe pe_exports pe_modules; do
    "$CC" $OSCF -c "$SRC/$f.c" -o "$WORK/os_$f.o" || fail "cannot build $f.c with the OS flags"
    OSOBJ="$OSOBJ $WORK/os_$f.o"
done
"$CC" -std=gnu11 -O1 -g -no-pie -Wall -Wextra -I"$SRC" -I"$HERE" -o "$WORK/pe_dll_test_os" "$HERE/pe_dll_test.c" $OSOBJ     || fail "cannot build pe_dll_test against the OS-flag objects"
"$WORK/pe_dll_test_os" "$FIX" > "$WORK/host_test_os.out" 2>&1 || { cat "$WORK/host_test_os.out"; fail "pe_dll_test (OS flags) failed"; }
OSCHK=$(grep -o '^PE-DLL-HOST-TEST: PASS checks=[0-9]*' "$WORK/host_test_os.out") || { cat "$WORK/host_test_os.out"; fail "pe_dll_test (OS flags) did not print PASS"; }
echo "PE-DLL-HOST-TEST-OSFLAGS: PASS ${OSCHK#*checks=} checks (library built with -O2 freestanding OS flags)"

# ---------------------------------------------------------------------------------------------------------------
# 4. fuzz (export directory + module loader), deterministic, >= 100000 iterations
# ---------------------------------------------------------------------------------------------------------------
FUZZ_FIX=("$FIX/dll_lib_a.dll" "$FIX/dll_lib_b.dll" "$FIX/dll_cyc_x.dll" "$FIX/dll_bad.dll" "$FIX/dll_main.exe")
export PE_DLL_FUZZ_CRASH="$CRASH"
"$CC" -std=gnu11 -O1 -g $SAN -Wall -Wextra -I"$SRC" -I"$HERE" -o "$WORK/pe_dll_fuzz" "$HERE/pe_dll_fuzz.c" $LIBSRC \
    || fail "cannot build pe_dll_fuzz"
rm -f "$CRASH"
PE_DLL_FUZZ_ITERS=$ITERS "$WORK/pe_dll_fuzz" "$SEED" "${FUZZ_FIX[@]}" > "$WORK/fuzz.out" 2> "$WORK/fuzz.err"
FZ=$?
cat "$WORK/fuzz.err"
cat "$WORK/fuzz.out"
if [ $FZ -ne 0 ]; then
    [ -f "$CRASH" ] && echo "failing input: $CRASH ($(wc -c < "$CRASH") bytes)" >&2
    fail "pe_dll_fuzz exited with status $FZ"
fi
grep -q '^PE-DLL-FUZZ: PASS iterations=' "$WORK/fuzz.out" || fail "pe_dll_fuzz did not print PE-DLL-FUZZ: PASS"
[ "$ITERS" -ge 100000 ] || fail "PE_DLL_FUZZ_ITERS=$ITERS is below the required 100000"

# the fuzz invariants + differential oracle also hold for the OS-flag (-O2, uninstrumented) library build
"$CC" -std=gnu11 -O1 -g -no-pie -Wall -Wextra -I"$SRC" -I"$HERE" -o "$WORK/pe_dll_fuzz_os" "$HERE/pe_dll_fuzz.c" $OSOBJ     || fail "cannot build pe_dll_fuzz against the OS-flag objects"
PE_DLL_FUZZ_ITERS=100000 "$WORK/pe_dll_fuzz_os" "$SEED" "${FUZZ_FIX[@]}" > "$WORK/fuzz_os.out" 2> "$WORK/fuzz_os.err"     || { tail -5 "$WORK/fuzz_os.err" >&2; fail "pe_dll_fuzz (OS flags) failed"; }
grep -q '^PE-DLL-FUZZ: PASS iterations=100000' "$WORK/fuzz_os.out" || fail "pe_dll_fuzz (OS flags) did not print PASS"
echo "PE-DLL-FUZZ-OSFLAGS: PASS iterations=100000 (library built with the OS flags)"

# ---------------------------------------------------------------------------------------------------------------
# 5. prove the harness itself
#    (a) an injected overflow / UB in the harness must abort AND leave the input in $CRASH  (same as pe_fuzz.c)
#    (b) a bounds check REMOVED from a copy of pe_exports.c must be caught by the fuzzer (i.e. the fuzzer reaches the
#        export parser's bounds checks and ASan sees the resulting out-of-bounds read) -- mutation testing
# ---------------------------------------------------------------------------------------------------------------
for inj in PE_DLL_FUZZ_INJECT PE_DLL_FUZZ_INJECT_UB; do
    rm -f "$CRASH"
    { env "$inj=7" PE_DLL_FUZZ_ITERS=50 "$WORK/pe_dll_fuzz" "$SEED" "${FUZZ_FIX[@]}" > /dev/null 2> "$WORK/selftest.err"; } 2>/dev/null
    ST=$?
    if [ $ST -eq 0 ] || [ ! -s "$CRASH" ]; then
        cat "$WORK/selftest.err" >&2
        fail "fuzz harness self-test ($inj): exit=$ST, crash file $( [ -s "$CRASH" ] && echo present || echo MISSING )"
    fi
done

BUG=$WORK/bug
mkdir -p "$BUG"
# bug 1: exp_str() loses its "rva is outside the image" test  (name / forwarder strings may then be read out of bounds)
sed 's/if (rva >= isz || k >= isz - rva) return -1;/if (k >= isz - rva) return -1;/' "$SRC/pe_exports.c" > "$BUG/pe_exports_bug1.c"
# bug 2: exp_dir() loses the EAT range check (a table running off the end of the image is then dereferenced)
sed 's/if (d->nfuncs && (d->eat == 0 || !ex_range_ok(d->eat, 4ull \* d->nfuncs, d->isz))) return PE_E_EXPORT;/ /' "$SRC/pe_exports.c" > "$BUG/pe_exports_bug2.c"
# bug 3: pe_export_find() loses its "ordinal-table value < NumberOfFunctions" check (EAT[idx] is then read out of bounds)
sed 's/^ \{16\}if (oi >= d.nfuncs) return PE_E_EXPORT;/                (void)0;/' "$SRC/pe_exports.c" > "$BUG/pe_exports_bug3.c"
NBUG=0
for b in 1 2 3; do
    cmp -s "$SRC/pe_exports.c" "$BUG/pe_exports_bug$b.c" && fail "self-test bug $b: the sed did not change pe_exports.c (source drifted?)"
    "$CC" -std=gnu11 -O1 -g $SAN -Wall -Wextra -I"$SRC" -I"$HERE" -o "$BUG/pe_dll_fuzz_bug$b" "$HERE/pe_dll_fuzz.c" \
        "$SRC/pe.c" "$BUG/pe_exports_bug$b.c" "$SRC/pe_modules.c" || fail "cannot build the bug-$b fuzzer"
    rm -f "$CRASH"
    { PE_DLL_FUZZ_ITERS=$ITERS "$BUG/pe_dll_fuzz_bug$b" "$SEED" "${FUZZ_FIX[@]}" > /dev/null 2> "$BUG/bug$b.err"; } 2>/dev/null
    ST=$?
    if [ $ST -eq 0 ] || [ ! -s "$CRASH" ]; then
        tail -5 "$BUG/bug$b.err" >&2
        fail "fuzz harness self-test: injected bug $b was NOT detected (exit=$ST)"
    fi
    WHAT=$(grep -m1 -E 'ERROR: AddressSanitizer|runtime error|invariant violated' "$BUG/bug$b.err" | cut -c1-110)
    echo "  injected bug $b detected: $WHAT"
    NBUG=$((NBUG + 1))
done
rm -f "$CRASH"
echo "PE-DLL-FUZZ-HARNESS-SELFTEST: PASS (harness: 2 injections, library: $NBUG removed bounds checks)"

echo "PE-DLL-TESTS: ALL PASS"
exit 0
