#!/usr/bin/env bash
# RSA-PSS verify KAT (CI-friendly, no QEMU). Verifies a fixed openssl-generated
# rsa_pss_rsae_sha256 signature + rejects tampered. Run: bash build_test/rsa_pss_kat.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
C=userspace/lib/crypto
BIN="/tmp/rsapsskat_$$"

[ -f build_test/rsa_pss_vec.h ] || bash build_test/gen_rsa_pss_vec.sh || {
    echo "RSAPSSKAT: FAIL vector_gen=1"; exit 1; }

gcc -std=gnu11 -O2 -w -o "$BIN" \
    build_test/rsa_pss_kat_main.c "$C/rsa_pss.c" "$C/rsa.c" "$C/bignum.c" \
    "$C/sha256.c" "$C/sha512.c" || { echo "RSAPSSKAT: FAIL build=1"; exit 1; }

"$BIN"; rc=$?
rm -f "$BIN"
exit $rc
