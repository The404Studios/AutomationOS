#!/usr/bin/env bash
# run_pe_tests.sh -- build + run the host-side tests for userspace/lib/pe (PE32+ parser/mapper/loader library).
#
#   wsl.exe -d Arch -- bash /mnt/c/Users/wilde/Desktop/Kernel/tests/win/run_pe_tests.sh
#
# Prints (and exits nonzero unless ALL of them are produced):
#   PE-FREESTANDING-BUILD: PASS      pe.c compiles freestanding, -Werror, no undefined symbols, no stack canary
#   PE-HOST-TEST: PASS ...           hand-built malicious PEs + real MinGW fixtures, under ASan+UBSan
#   PE-FUZZ: PASS iterations=N ...   300000-iteration deterministic mutation fuzz, under ASan+UBSan
#   PE-FUZZ-HARNESS-SELFTEST: PASS   the fuzzer really aborts and saves /tmp/pe_fuzz_crash.bin on a bug
#
# Environment knobs: PE_FIXDIR (default /tmp/winfix), PE_WORKDIR (default /tmp/pe_tests_work), CC, MINGW_CC,
# PE_FUZZ_SEED, PE_FUZZ_ITERS.  Nothing under the repo is modified; all build products go to $PE_WORKDIR.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC=$ROOT/userspace/lib/pe
FIX=${PE_FIXDIR:-/tmp/winfix}
WORK=${PE_WORKDIR:-/tmp/pe_tests_work}
CC=${CC:-gcc}
MINGW=${MINGW_CC:-x86_64-w64-mingw32-gcc}
MINGW_NM=${MINGW_NM:-x86_64-w64-mingw32-nm}
SEED=${PE_FUZZ_SEED:-0x9E3779B97F4A7C15}
CRASH=/tmp/pe_fuzz_crash.bin

fail() { echo "FAIL: $*" >&2; exit 1; }

mkdir -p "$FIX" "$WORK" || fail "cannot create $FIX / $WORK"

# ---------------------------------------------------------------------------------------------------------------
# 1. fixtures (rebuilt only when missing, with exactly the commands documented in the fixture sources)
# ---------------------------------------------------------------------------------------------------------------
if [ ! -f "$FIX/nocrt_hello.exe" ]; then
    echo "[fixtures] building nocrt_hello.exe"
    "$MINGW" -O2 -nostdlib -ffreestanding -fno-stack-protector \
        -Wl,-e,start -Wl,--subsystem,console -Wl,--dynamicbase -Wl,--enable-reloc-section \
        -o "$FIX/nocrt_hello.exe" "$HERE/nocrt_hello.c" -lkernel32 || fail "cannot build nocrt_hello.exe"
fi
if [ ! -f "$FIX/crt_hello.exe" ]; then
    echo "[fixtures] building crt_hello.exe"
    "$MINGW" -O2 -o "$FIX/crt_hello.exe" "$HERE/crt_hello.c" || fail "cannot build crt_hello.exe"
fi

# The shipped nocrt fixture has NO .reloc (GCC constant-folds its `static` g_msg_ptr away), so it cannot prove a
# rebase.  Build a private variant whose g_msg_ptr is a real, writable .data pointer (=> a DIR64 relocation) into
# $WORK.  Optional: skipped (loudly) when MinGW is not installed.
RH=""
GMSG=""
if command -v "$MINGW" >/dev/null 2>&1 && command -v "$MINGW_NM" >/dev/null 2>&1; then
    cat > "$WORK/reloc_hello.c" <<'EOF'
/* reloc_hello: nocrt_hello with g_msg_ptr in .data (not folded) so the image carries a DIR64 base relocation. */
typedef void *HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD id);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov);
__declspec(dllimport) void __stdcall ExitProcess(unsigned int code) __attribute__((noreturn));
static const char g_msg[] = "WINFIX: reloc hello\n";
const char *volatile g_msg_ptr = g_msg;
void __attribute__((noreturn)) start(void)
{
    DWORD written = 0;
    HANDLE out = GetStdHandle((DWORD)-11);
    WriteFile(out, g_msg_ptr, (DWORD)(sizeof(g_msg) - 1), &written, 0);
    ExitProcess(written == sizeof(g_msg) - 1 ? 42 : 1);
}
EOF
    if "$MINGW" -O2 -nostdlib -ffreestanding -fno-stack-protector \
        -Wl,-e,start -Wl,--subsystem,console -Wl,--dynamicbase -Wl,--enable-reloc-section \
        -o "$WORK/reloc_hello.exe" "$WORK/reloc_hello.c" -lkernel32; then
        GMSG=$("$MINGW_NM" "$WORK/reloc_hello.exe" | awk '$3 == "g_msg_ptr" { print $1 }')
        if [ -n "$GMSG" ]; then RH="$WORK/reloc_hello.exe"; else echo "[fixtures] note: g_msg_ptr symbol not found; reloc_hello check skipped"; fi
    else
        echo "[fixtures] note: reloc_hello.exe did not build; reloc_hello check skipped"
    fi
else
    echo "[fixtures] note: MinGW not available; reloc_hello check skipped"
fi

# ---------------------------------------------------------------------------------------------------------------
# 2. freestanding build gate
# ---------------------------------------------------------------------------------------------------------------
FSFLAGS="-std=gnu11 -ffreestanding -nostdlib -fno-builtin -O2 -Wall -Wextra -Werror"
"$CC" $FSFLAGS -c "$SRC/pe.c" -o "$WORK/pe_fs_plain.o" || fail "pe.c does not build freestanding (spec flags)"
"$CC" $FSFLAGS -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -c "$SRC/pe.c" -o "$WORK/pe_fs_kernel.o" \
    || fail "pe.c does not build freestanding (kernel flags)"
for o in "$WORK/pe_fs_plain.o" "$WORK/pe_fs_kernel.o"; do
    UNDEF=$(nm -u "$o" 2>&1)
    [ -z "$UNDEF" ] || fail "freestanding object $o has undefined symbols (needs libc?): $UNDEF"
    if objdump -d "$o" | grep -q 'fs:0x28'; then fail "freestanding object $o contains a stack-protector canary (fs:0x28)"; fi
done
echo "PE-FREESTANDING-BUILD: PASS"

# ---------------------------------------------------------------------------------------------------------------
# 3. host test
# ---------------------------------------------------------------------------------------------------------------
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
export ASAN_OPTIONS=${ASAN_OPTIONS:-strict_string_checks=1:detect_stack_use_after_return=1:abort_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:abort_on_error=1}

"$CC" -std=gnu11 -O1 -g $SAN -Wall -Wextra -I"$SRC" -o "$WORK/pe_host_test" "$HERE/pe_host_test.c" "$SRC/pe.c" \
    || fail "cannot build pe_host_test"
"$WORK/pe_host_test" "$FIX/nocrt_hello.exe" "$FIX/crt_hello.exe" "$RH" "$GMSG" "$WORK/synthetic.exe" \
    > "$WORK/host_test.out" 2>&1
HT=$?
cat "$WORK/host_test.out"
[ $HT -eq 0 ] || fail "pe_host_test exited with status $HT"
grep -q '^PE-HOST-TEST: PASS' "$WORK/host_test.out" || fail "pe_host_test did not print PE-HOST-TEST: PASS"

# ---------------------------------------------------------------------------------------------------------------
# 4. fuzz
# ---------------------------------------------------------------------------------------------------------------
"$CC" -std=gnu11 -O1 -g $SAN -Wall -Wextra -I"$SRC" -o "$WORK/pe_fuzz" "$HERE/pe_fuzz.c" "$SRC/pe.c" \
    || fail "cannot build pe_fuzz"
FUZZ_FIX=("$FIX/nocrt_hello.exe" "$FIX/crt_hello.exe")
[ -n "$RH" ] && FUZZ_FIX+=("$RH")
[ -f "$WORK/synthetic.exe" ] && FUZZ_FIX+=("$WORK/synthetic.exe")
rm -f "$CRASH"
"$WORK/pe_fuzz" "$SEED" "${FUZZ_FIX[@]}" > "$WORK/fuzz.out" 2> "$WORK/fuzz.err"
FZ=$?
cat "$WORK/fuzz.err"
cat "$WORK/fuzz.out"
if [ $FZ -ne 0 ]; then
    [ -f "$CRASH" ] && echo "failing input: $CRASH ($(wc -c < "$CRASH") bytes)" >&2
    fail "pe_fuzz exited with status $FZ"
fi
grep -q '^PE-FUZZ: PASS iterations=' "$WORK/fuzz.out" || fail "pe_fuzz did not print PE-FUZZ: PASS"

# ---------------------------------------------------------------------------------------------------------------
# 5. prove the harness itself: an injected overflow / UB must abort AND leave the input in $CRASH
# ---------------------------------------------------------------------------------------------------------------
for inj in PE_FUZZ_INJECT PE_FUZZ_INJECT_UB; do
    rm -f "$CRASH"
    { env "$inj=7" PE_FUZZ_ITERS=50 "$WORK/pe_fuzz" "$SEED" "${FUZZ_FIX[@]}" > /dev/null 2> "$WORK/selftest.err"; } 2>/dev/null
    ST=$?
    if [ $ST -eq 0 ] || [ ! -s "$CRASH" ]; then
        cat "$WORK/selftest.err" >&2
        fail "fuzz harness self-test ($inj): exit=$ST, crash file $( [ -s "$CRASH" ] && echo present || echo MISSING )"
    fi
done
rm -f "$CRASH"
echo "PE-FUZZ-HARNESS-SELFTEST: PASS"

echo "PE-TESTS: ALL PASS"
exit 0
