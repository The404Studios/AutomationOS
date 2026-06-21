#!/usr/bin/env bash
# P-384 ECDSA verify KAT (CI-friendly, no QEMU). Host-compiles the real crypto
# sources and runs RFC 6979 A.2.6 vectors + edge cases.
# Run: wsl -d Arch bash build_test/p384_kat.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
C=userspace/lib/crypto
BIN="/tmp/p384kat_$$"

gcc -std=gnu11 -O2 -w -o "$BIN" \
    build_test/p384_kat_main.c "$C/p384.c" "$C/bignum.c" "$C/sha512.c" || {
        echo "P384KAT: FAIL harness_build=1"; exit 1; }

"$BIN"; rc=$?
rm -f "$BIN"
exit $rc
