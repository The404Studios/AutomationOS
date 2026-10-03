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

BUNDLE = os.environ.get("CA_BUNDLE_SRC", "/etc/ssl/certs/ca-certificates.crt")
OUT    = "userspace/lib/tls/ca_roots_data.h"

TARGETS = [
    ("ISRG Root X1",                                 "isrg_root_x1",          "ISRG Root X1"),
    ("GTS Root R1",                                  "gts_root_r1",           "GTS Root R1"),
    ("DigiCert Global Root CA",                      "digicert_root_ca",      "DigiCert Global Root CA"),
    ("DigiCert Global Root G2",                      "digicert_root_g2",      "DigiCert Global Root G2"),
    ("GlobalSign Root CA",                           "globalsign_root_ca",    "GlobalSign Root CA"),
    ("USERTrust RSA Certification Authority",        "usertrust_rsa_ca",      "USERTrust RSA Certification Authority"),
    ("Amazon Root CA 1",                             "amazon_root_ca_1",      "Amazon Root CA 1"),
    # ECDSA roots -- now useful since P-256 + P-384 ECDSA verification works.
    # SSL.com TLS ECC Root CA 2022 is example.com's modern trust anchor (its
    # chain: example.com <- Cloudflare TLS Issuing ECC CA 3 <- SSL.com TLS
    # Transit ECC CA R2 <- SSL.com TLS ECC Root CA 2022).
    ("SSL.com TLS ECC Root CA 2022",                 "sslcom_tls_ecc_root_2022", "SSL.com TLS ECC Root CA 2022"),
    ("DigiCert Global Root G3",                      "digicert_root_g3",      "DigiCert Global Root G3"),
    ("USERTrust ECC Certification Authority",        "usertrust_ecc_ca",      "USERTrust ECC Certification Authority"),
    ("GTS Root R4",                                  "gts_root_r4",           "GTS Root R4"),

    # ---- CERT-REAL-0: the rest of the roots the public web actually anchors to. The first 11 left out
    # ISRG Root X2 (Let's Encrypt's ECDSA chains: every modern LE site on an E5/E6 intermediate), the other
    # Google roots, the Amazon/Microsoft/Cloudflare-adjacent roots and the older-but-still-live DigiCert,
    # Sectigo, GlobalSign, GoDaddy/Starfield, SSL.com and Entrust families. All are taken bit-exact from the
    # host's system trust store (the Mozilla set); a missing one is skipped with a WARN, never invented.
    ("ISRG Root X2",                                 "isrg_root_x2",          "ISRG Root X2"),
    ("GTS Root R2",                                  "gts_root_r2",           "GTS Root R2"),
    ("GTS Root R3",                                  "gts_root_r3",           "GTS Root R3"),
    ("Amazon Root CA 2",                             "amazon_root_ca_2",      "Amazon Root CA 2"),
    ("Amazon Root CA 3",                             "amazon_root_ca_3",      "Amazon Root CA 3"),
    ("Amazon Root CA 4",                             "amazon_root_ca_4",      "Amazon Root CA 4"),
    ("DigiCert Assured ID Root CA",                  "digicert_assured_ca",   "DigiCert Assured ID Root CA"),
    ("DigiCert Assured ID Root G2",                  "digicert_assured_g2",   "DigiCert Assured ID Root G2"),
    ("DigiCert Assured ID Root G3",                  "digicert_assured_g3",   "DigiCert Assured ID Root G3"),
    ("DigiCert High Assurance EV Root CA",           "digicert_hv_ev",        "DigiCert High Assurance EV Root CA"),
    ("DigiCert Trusted Root G4",                     "digicert_trusted_g4",   "DigiCert Trusted Root G4"),
    ("DigiCert TLS ECC P384 Root G5",                "digicert_tls_ecc_g5",   "DigiCert TLS ECC P384 Root G5"),
    ("DigiCert TLS RSA4096 Root G5",                 "digicert_tls_rsa_g5",   "DigiCert TLS RSA4096 Root G5"),
    ("Baltimore CyberTrust Root",                    "baltimore_cybertrust",  "Baltimore CyberTrust Root"),
    ("Microsoft RSA Root Certificate Authority 2017","microsoft_rsa_2017",    "Microsoft RSA Root Certificate Authority 2017"),
    ("Microsoft ECC Root Certificate Authority 2017","microsoft_ecc_2017",    "Microsoft ECC Root Certificate Authority 2017"),
    ("GlobalSign Root R46",                          "globalsign_r46",        "GlobalSign Root R46"),
    ("GlobalSign Root E46",                          "globalsign_e46",        "GlobalSign Root E46"),
    ("GlobalSign Root CA - R3",                      "globalsign_r3",         "GlobalSign Root CA - R3"),
    ("GlobalSign ECC Root CA - R5",                  "globalsign_ecc_r5",     "GlobalSign ECC Root CA - R5"),
    ("Go Daddy Root Certificate Authority - G2",     "godaddy_root_g2",       "Go Daddy Root Certificate Authority - G2"),
    ("Starfield Root Certificate Authority - G2",    "starfield_root_g2",     "Starfield Root Certificate Authority - G2"),
    ("Starfield Services Root Certificate Authority - G2", "starfield_services_g2", "Starfield Services Root Certificate Authority - G2"),
    ("COMODO RSA Certification Authority",           "comodo_rsa_ca",         "COMODO RSA Certification Authority"),
    ("COMODO ECC Certification Authority",           "comodo_ecc_ca",         "COMODO ECC Certification Authority"),
    ("Sectigo Public Server Authentication Root R46","sectigo_server_r46",    "Sectigo Public Server Authentication Root R46"),
    ("Sectigo Public Server Authentication Root E46","sectigo_server_e46",    "Sectigo Public Server Authentication Root E46"),
    ("SSL.com Root Certification Authority RSA",     "sslcom_root_rsa",       "SSL.com Root Certification Authority RSA"),
    ("SSL.com Root Certification Authority ECC",     "sslcom_root_ecc",       "SSL.com Root Certification Authority ECC"),
    ("SSL.com TLS RSA Root CA 2022",                 "sslcom_tls_rsa_2022",   "SSL.com TLS RSA Root CA 2022"),
    ("Entrust Root Certification Authority - G2",    "entrust_root_g2",       "Entrust Root Certification Authority - G2"),
    ("AAA Certificate Services",                     "comodo_aaa",            "AAA Certificate Services"),
    ("HARICA TLS RSA Root CA 2021",                  "harica_tls_rsa_2021",   "HARICA TLS RSA Root CA 2021"),
    ("HARICA TLS ECC Root CA 2021",                  "harica_tls_ecc_2021",   "HARICA TLS ECC Root CA 2021"),
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
