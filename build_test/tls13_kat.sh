#!/usr/bin/env bash
# TLS 1.3 key-schedule KAT (CI-friendly, no QEMU). Host-compiles the real
# tls13_keysched.c + HKDF/SHA/HMAC sources and runs the RFC 8448 Section 3 KAT.
# Run: wsl -d Arch bash build_test/tls13_kat.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
C=userspace/lib/crypto
T=userspace/lib/tls
BIN="/tmp/tls13kat_$$"

gcc -std=gnu11 -O2 -w -o "$BIN" \
    build_test/tls13_kat_main.c "$T/tls13_keysched.c" \
    "$C/hkdf.c" "$C/hmac.c" "$C/sha256.c" "$C/sha512.c" "$C/sha1.c" || {
        echo "TLS13KAT: FAIL harness_build=1"; exit 1; }

"$BIN"; rc=$?
rm -f "$BIN"
exit $rc
