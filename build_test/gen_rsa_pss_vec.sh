#!/bin/bash
# Generate a fixed RSA-PSS (rsa_pss_rsae_sha256, salt=digest) known-answer
# vector with openssl and emit a C header for the KAT. Run ONCE; commit the
# header so the KAT is deterministic (no network/openssl needed at test time).
set -e
cd /tmp
openssl genrsa -out kpss.pem 2048 >/dev/null 2>&1
openssl rsa -in kpss.pem -pubout -out ppss.pem >/dev/null 2>&1
printf 'TLS 1.3 RSA-PSS KAT message' > mpss.txt
openssl dgst -sha256 -sign kpss.pem \
    -sigopt rsa_padding_mode:pss -sigopt rsa_pss_saltlen:-1 \
    -out spss.bin mpss.txt
[ -s spss.bin ] || { echo "FATAL: signature file empty"; exit 1; }
MOD=$(openssl rsa -pubin -in ppss.pem -modulus -noout | sed 's/Modulus=//' | tr 'A-F' 'a-f')
SIG=$(od -An -v -tx1 spss.bin | tr -d ' \n')
MSG=$(od -An -v -tx1 mpss.txt | tr -d ' \n')
[ -n "$SIG" ] || { echo "FATAL: empty SIG hex"; exit 1; }

OUT=/mnt/c/Users/wilde/Desktop/Kernel/build_test/rsa_pss_vec.h
python3 - "$MOD" "$SIG" "$MSG" > "$OUT" <<'PY'
import sys
mod,sig,msg=sys.argv[1],sys.argv[2],sys.argv[3]
def arr(name,h):
    b=bytes.fromhex(h)
    s=f"static const unsigned char {name}[] = {{\n"
    for i in range(0,len(b),12):
        s+="    "+", ".join(f"0x{x:02x}" for x in b[i:i+12])+",\n"
    s+="};\n"+f"static const unsigned long {name}_len = {len(b)};\n"
    return s
print("/* RSA-PSS (rsa_pss_rsae_sha256, salt=digest) KAT vector -- openssl-generated")
print(" * by build_test/gen_rsa_pss_vec.sh. 2048-bit key; message signed with PSS. */")
print("#ifndef RSA_PSS_VEC_H\n#define RSA_PSS_VEC_H\n")
print(arr("rsapss_modulus",mod))
print('static const unsigned char rsapss_exponent[] = { 0x01, 0x00, 0x01 };')
print('static const unsigned long rsapss_exponent_len = 3;\n')
print(arr("rsapss_sig",sig))
print(arr("rsapss_msg",msg))
print("#endif")
PY
echo "wrote $OUT (modulus $((${#MOD}/2)) bytes, sig $((${#SIG}/2)) bytes)"
