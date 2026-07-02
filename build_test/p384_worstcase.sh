#!/usr/bin/env bash
# True worst-case stack: force no-inlining so every distinct frame in the
# cert-verify -> p384 chain is counted separately. Also report sizeof(tbs_fields)
# and X509V_MAX_CHAIN to sanity-check the x509_verify_chain frame.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
T=/tmp/p384wc
mkdir -p "$T"
FLAGS="-std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 -w -fstack-usage -fno-inline -fno-inline-small-functions"
INC="-Iuserspace/lib/crypto -Iuserspace/lib/tls -Iuserspace/lib/net"
gcc $FLAGS $INC -c userspace/lib/tls/x509_verify.c -o "$T/x509_verify.o"
gcc $FLAGS $INC -c userspace/lib/crypto/p384.c     -o "$T/p384.o"
gcc $FLAGS $INC -c userspace/lib/crypto/bignum.c   -o "$T/bignum.o"
echo "=== x509_verify frames (no-inline) ==="
cat "$T/x509_verify.su" | sort -t$'\t' -k2 -n -r | head -12
echo "=== p384 frames (no-inline) ==="
cat "$T/p384.su" | sort -t$'\t' -k2 -n -r | head -6
echo ""
echo "=== Worst-case chain (no-inline frames) ==="
python3 - "$T/x509_verify.su" "$T/p384.su" "$T/bignum.su" <<'PY'
import sys
def load(f):
    d={}
    for line in open(f):
        p=line.rstrip("\n").split("\t")
        if len(p)>=2:
            try: d[p[0].split(":")[-1]]=int(p[1])
            except: pass
    return d
xv=load(sys.argv[1]); p=load(sys.argv[2]); b=load(sys.argv[3])
# tls_client_connect frame measured earlier with inline-on (19920). Use it.
chain=[("tls_client_connect",19920),
       ("x509_verify_chain", xv.get("x509_verify_chain.part.0", xv.get("x509_verify_chain",0))),
       ("verify_against_roots", xv.get("verify_against_roots",0)),
       ("verify_cert_signed_by", xv.get("verify_cert_signed_by",0)),
       ("verify_one", xv.get("verify_one",0)),
       ("p384_ecdsa_verify", p.get("p384_ecdsa_verify",0)),
       ("jscalar", p.get("jscalar",0)),
       ("jadd", p.get("jadd",0)),
       ("jdouble", p.get("jdouble",0)),
       ("bn_mod_mul", b.get("bn_mod_mul",0))]
tot=0
for n,s in chain:
    tot+=s; print(f"  {n:24s} {s:7d}  cum={tot}")
print(f"  ---- worst-case chain (root-verify, no-inline) = {tot} bytes ----")
print(f"  64KB user stack = 65536; headroom = {65536-tot} ({100*(65536-tot)/65536:.1f}%)")
PY
