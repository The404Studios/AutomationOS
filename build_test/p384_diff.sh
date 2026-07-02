#!/usr/bin/env bash
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
C=userspace/lib/crypto
BIN="/tmp/p384diff_$$"
python3 build_test/p384_ref.py > build_test/p384_vectors.h || { echo "gen FAIL"; exit 1; }
gcc -std=gnu11 -O2 -w -o "$BIN" \
    build_test/p384_diff_main.c "$C/p384.c" "$C/bignum.c" "$C/sha512.c" || {
        echo "P384DIFF: FAIL harness_build=1"; exit 1; }
"$BIN"; rc=$?
rm -f "$BIN"
exit $rc
