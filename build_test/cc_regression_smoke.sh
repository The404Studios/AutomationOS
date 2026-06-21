#!/usr/bin/env bash
# cc regression smoke (B4a / CC-REGRESSION-SUITE-0)
#
# Host-compiles the REAL on-device compiler front+back end (the IDE's
# lexer/parser + cc_codegen/cc_expr/cc_type) and asserts codegen for the
# programs in tests/cc/. Proves the global-initializer fix (B3) and grows one
# permanent case per codegen bug fixed. No QEMU needed -> CI-friendly.
#
# Run: wsl -d Arch bash -lc 'cd /mnt/c/Users/wilde/Desktop/Kernel && bash build_test/cc_regression_smoke.sh'
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
S=userspace/apps/ide
DUMP="/tmp/ccdump_$$"
DIAG="/tmp/ccdiag_$$"

# Build the host harness against the real compiler sources (NOT ide_parse.c --
# that is the IDE's model/blueprint parser and pulls in GUI string helpers; the
# C parser entry parse_translation_unit lives in ide_pcore.c).
gcc -w -I"$S" -o "$DUMP" build_test/cc_host_harness.c \
    "$S/ide_lex.c" "$S/ide_ast.c" "$S/ide_pcore.c" "$S/ide_pdecl.c" \
    "$S/ide_pstmt.c" "$S/ide_pexpr.c" \
    "$S/cc_codegen.c" "$S/cc_expr.c" "$S/cc_type.c" "$S/ide_astprint.c" || {
        echo "CCREGRESSION: FAIL harness_build=1"; exit 1; }

pass=0; fail=0

# check NAME FILE WANT-line-regex [MUSTNOT-line-regex]
check() {
    name="$1"; file="$2"; want="$3"; notwant="${4:-}"
    asm="$("$DUMP" "tests/cc/$file" 2>"$DIAG")"; rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  FAIL $name: cc_compile error"; sed 's/^/    /' "$DIAG"; fail=$((fail+1)); return; fi
    if ! printf '%s\n' "$asm" | grep -qE "$want"; then
        echo "  FAIL $name: expected line /$want/ in generated asm"; fail=$((fail+1)); return; fi
    if [ -n "$notwant" ] && printf '%s\n' "$asm" | grep -qE "$notwant"; then
        echo "  FAIL $name: unwanted line /$notwant/ present"; fail=$((fail+1)); return; fi
    echo "  ok   $name"; pass=$((pass+1))
}

echo "== cc regression: B3 global scalar initializers =="
# int g = 5;  must emit `g: dq 5` (the pre-B3 bug emitted `dq 0`).
check global_scalar_init global_scalar_init.c '^dq 5$' '^dq 0$'

echo "== cc regression: B4 global array brace-init + indexing =="
# int a[3]={10,20,30};  ->  a: dq 10 / dq 20 / dq 30  (was a single `dq 0`).
# Last value present + NO stray `dq 0` proves every element was emitted.
check global_array_init global_array_init.c '^dq 30$' '^dq 0$'
# int a[4]={1,2,3,4}; summed via a[i] in a loop -> all four values in .data.
check init_list init_list.c '^dq 4$' '^dq 0$'

echo "== cc regression: CC-MULTIDECL-0 multi-declarator =="
# int x=10, y=20;  BOTH globals must survive (pre-fix dropped the 2nd, so y
# became an unknown identifier and the program failed to compile).
check multi_decl multi_decl.c '^dq 20$' ''

echo "== cc regression: B4 follow-ons (const array, pointer arithmetic) =="
# const int t[5]={2,4,6,8,10}; -> t[3]'s value (8) present in .data.
check const_table const_table.c '^dq 8$' ''
# int a[3]={3,6,9}; int *p=a; *(p+2) -> the array (incl. 9) emits + compiles clean.
check pointer_arith pointer_arith.c '^dq 9$' ''

echo "== cc regression: CC-STRUCTINIT-0 struct brace-init (by layout) =="
# struct {int x;int y}={11,22} -> p.y present as `dq 22`; no stray `dq 0` (the
# pre-fix bug emitted a single `dq 0`).
check struct_init struct_init.c '^dq 22$' '^dq 0$'
# mixed {char c;int i}={65,7} -> the char field emits `db 65` (layout-aware,
# not `dq 65`), so the int lands at offset 8.
check struct_mixed struct_mixed.c '^db 65$' ''
# packed {char a;char b}={65,66} -> the 2nd char emits `db 66` at offset 1.
check struct_packed struct_packed.c '^db 66$' ''

echo "== cc regression: CC-SWITCH-0 switch/case/default codegen =="
# switch(x=2){case 2: r=20; ...} -> dispatch emits `cmp rcx, 2` (case-2 compare)
# and compiles clean (pre-fix: "unsupported statement" error on the switch).
check switch_basic switch_basic.c 'cmp rcx, 2' ''

rm -f "$DUMP" "$DIAG"
echo "CCREGRESSION: PASS tests=$pass failures=$fail"
[ "$fail" -eq 0 ] || { echo "CCREGRESSION: FAIL"; exit 1; }
