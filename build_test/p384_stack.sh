#!/usr/bin/env bash
# Measure per-function stack frame sizes (-fstack-usage).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
C=userspace/lib/crypto
T=/tmp/p384su
mkdir -p "$T"
gcc -std=gnu11 -O2 -w -fstack-usage -c "$C/p384.c"   -o "$T/p384.o"   -I "$C"
gcc -std=gnu11 -O2 -w -fstack-usage -c "$C/bignum.c" -o "$T/bignum.o" -I "$C"
echo "=== p384 frames (name / bytes / type) ==="
cat "$T/p384.su" | sort -t$'\t' -k2 -n -r
echo "=== bignum frames ==="
cat "$T/bignum.su" | sort -t$'\t' -k2 -n -r
