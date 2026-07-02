#!/usr/bin/env bash
# Measure stack frames across the X.509/TLS verification chain that calls
# p384_ecdsa_verify, to estimate the FULL stack depth vs the 64KB user stack.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
T=/tmp/p384chain
mkdir -p "$T"
FLAGS="-std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 -w -fstack-usage"
INC="-Iuserspace/lib/crypto -Iuserspace/lib/tls -Iuserspace/lib/net"
for f in userspace/lib/tls/x509_verify.c userspace/lib/tls/x509.c userspace/lib/tls/tls.c userspace/lib/tls/asn1.c; do
  base=$(basename "$f" .c)
  gcc $FLAGS $INC -c "$f" -o "$T/$base.o" 2>/dev/null && echo "compiled $base"
done
echo ""
echo "=== Largest frames across the cert-verify chain (top 25) ==="
cat "$T"/*.su 2>/dev/null | sort -t$'\t' -k2 -n -r | head -25
