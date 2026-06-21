#!/usr/bin/env bash
# Offline full TLS 1.3 handshake KAT against RFC 8448 Section 3 (no QEMU).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
C=userspace/lib/crypto
T=userspace/lib/tls
BIN="/tmp/tls13hskat_$$"

[ -f build_test/rfc8448_record_vec.h ] || python3 build_test/extract_rfc8448_record.py || {
    echo "TLS13HSKAT: FAIL vector_extract=1"; exit 1; }

gcc -std=gnu11 -O2 -w -o "$BIN" \
    build_test/tls13_handshake_kat_main.c \
    "$T/tls13_handshake.c" "$T/tls13_keysched.c" "$T/tls13_record.c" "$T/tls13_certverify.c" \
    "$C/rsa_pss.c" "$C/rsa.c" "$C/bignum.c" "$C/x25519.c" "$C/hkdf.c" "$C/hmac.c" \
    "$C/sha256.c" "$C/sha512.c" "$C/sha1.c" "$C/aes.c" "$C/chacha20poly1305.c" \
    "$C/p256.c" "$C/p384.c" || { echo "TLS13HSKAT: FAIL build=1"; exit 1; }

"$BIN"; rc=$?
rm -f "$BIN"
exit $rc
