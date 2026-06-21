#!/usr/bin/env bash
# TLS 1.3 CertificateVerify KAT (CI-friendly). Verifies the real RFC 8448
# Section 3 server CertificateVerify (RSA-PSS over the CH..Cert transcript hash).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
C=userspace/lib/crypto
T=userspace/lib/tls
BIN="/tmp/tls13cvkat_$$"

gcc -std=gnu11 -O2 -w -o "$BIN" \
    build_test/tls13_certverify_kat_main.c "$T/tls13_certverify.c" \
    "$C/rsa_pss.c" "$C/rsa.c" "$C/bignum.c" "$C/sha256.c" "$C/sha512.c" \
    "$C/p256.c" "$C/p384.c" || { echo "TLS13CVKAT: FAIL build=1"; exit 1; }

"$BIN"; rc=$?
rm -f "$BIN"
exit $rc
