#!/usr/bin/env python3
"""
Extract a curated set of real CA root certificates from the build host's
system trust store and generate userspace/lib/tls/ca_roots_data.h with
authentic bit-exact DER bytes (so HTTPS can actually authenticate peers).

Run from the repo root inside WSL (the system CA bundle is at
/etc/ssl/certs/ca-certificates.crt on Arch and most Linux distros).

The roots picked cover the dominant set of HTTPS sites:
  ISRG Root X1                                  -- Let's Encrypt (most of the web)
  GTS Root R1                                   -- Google Trust Services (Google, YouTube)
  DigiCert Global Root CA / G2                  -- DigiCert (enterprise, news, banks)
  GlobalSign Root CA                            -- GlobalSign (Cloudflare and many others)
  USERTrust RSA Certification Authority         -- Sectigo / Comodo
  Amazon Root CA 1                              -- AWS-hosted services
"""
import subprocess, os, re, sys, datetime, pathlib

BUNDLE = "/etc/ssl/certs/ca-certificates.crt"
OUT    = "userspace/lib/tls/ca_roots_data.h"

TARGETS = [
    ("ISRG Root X1",                                 "isrg_root_x1",          "ISRG Root X1"),
    ("GTS Root R1",                                  "gts_root_r1",           "GTS Root R1"),
    ("DigiCert Global Root CA",                      "digicert_root_ca",      "DigiCert Global Root CA"),
    ("DigiCert Global Root G2",                      "digicert_root_g2",      "DigiCert Global Root G2"),
    ("GlobalSign Root CA",                           "globalsign_root_ca",    "GlobalSign Root CA"),
    ("USERTrust RSA Certification Authority",        "usertrust_rsa_ca",      "USERTrust RSA Certification Authority"),
    ("Amazon Root CA 1",                             "amazon_root_ca_1",      "Amazon Root CA 1"),
]


def get_subject(pem: str) -> str:
    r = subprocess.run(
        ["openssl", "x509", "-noout", "-subject"],
        input=pem, capture_output=True, text=True, check=False,
    )
    return r.stdout


def pem_to_der(pem: str) -> bytes:
    r = subprocess.run(
        ["openssl", "x509", "-outform", "DER"],
        input=pem.encode(), capture_output=True, check=True,
    )
    return r.stdout


def c_array(ident: str, data: bytes) -> str:
    lines = []
    for i in range(0, len(data), 12):
        chunk = data[i : i + 12]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    body = "\n".join(lines).rstrip(",")
    return (
        f"static const unsigned char {ident}_der[] = {{\n"
        f"{body}\n}};\n"
        f"static const unsigned long {ident}_der_len = {len(data)};\n"
    )


def main() -> int:
    if not os.path.exists(BUNDLE):
        print(f"FATAL: CA bundle not found at {BUNDLE}", file=sys.stderr)
        return 1

    with open(BUNDLE, "r") as fh:
        pem_text = fh.read()

    pem_blocks = re.findall(
        r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----",
        pem_text,
        re.S,
    )

    arrays = []
    entries = []
    for cn_sub, ident, label in TARGETS:
        found = None
        for pem in pem_blocks:
            if cn_sub in get_subject(pem):
                found = pem
                break
        if not found:
            print(f"WARN: '{cn_sub}' not in {BUNDLE}; skipping", file=sys.stderr)
            continue
        der = pem_to_der(found)
        arrays.append(c_array(ident, der))
        entries.append(f'    {{ "{label}", {ident}_der, {ident}_der_len }},')

    ts = datetime.datetime.utcnow().isoformat(timespec="seconds") + "Z"
    header = (
        f"/* ca_roots_data.h -- Generated {ts} by scripts/extract_ca_roots.py\n"
        f" * Source: {BUNDLE}\n"
        f" *\n"
        f" * REAL, bit-exact root CA DER bytes extracted from the system trust\n"
        f" * store via openssl. Lets the TLS layer authenticate real server\n"
        f" * chains. Re-run the script to refresh.\n"
        f" */\n"
        f"#ifndef CA_ROOTS_DATA_H\n"
        f"#define CA_ROOTS_DATA_H\n\n"
    )

    macro = "#define CA_ROOTS_DATA \\\n" + " \\\n".join(entries) + "\n"

    pathlib.Path(OUT).parent.mkdir(parents=True, exist_ok=True)
    with open(OUT, "w") as fh:
        fh.write(header)
        for arr in arrays:
            fh.write(arr + "\n")
        fh.write("\n" + macro)
        fh.write("\n#endif /* CA_ROOTS_DATA_H */\n")

    print(f"extracted {len(entries)} root CA(s) into {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
