#!/usr/bin/env bash
# TLS 1.3 record-layer KAT (CI-friendly, no QEMU). Decrypts the real RFC 8448
# Section 3 server encrypted flight + a seal/open round-trip.
# Run: wsl -d Arch bash build_test/tls13_record_kat.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
C=userspace/lib/crypto
T=userspace/lib/tls
BIN="/tmp/tls13reckat_$$"

# refresh the RFC 8448 record vector header if missing
[ -f build_test/rfc8448_record_vec.h ] || python3 build_test/extract_rfc8448_record.py || {
    echo "TLS13RECKAT: FAIL vector_extract=1"; exit 1; }

gcc -std=gnu11 -O2 -w -o "$BIN" \
    build_test/tls13_record_kat_main.c "$T/tls13_record.c" \
    "$C/aes.c" "$C/chacha20poly1305.c" || {
        echo "TLS13RECKAT: FAIL harness_build=1"; exit 1; }

"$BIN"; rc=$?
rm -f "$BIN"
exit $rc
