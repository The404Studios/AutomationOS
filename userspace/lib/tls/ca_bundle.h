/*
 * ca_bundle.h -- CA root certificate trust store API.
 * ====================================================
 *
 * Freestanding, pure data + code: NO syscalls, NO libc, NO malloc,
 * NO standard headers.
 *
 * Build flags (NO fs:0x28 stack canary):
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * PURPOSE
 * -------
 * The TLS certificate-chain validator (x509_verify.h) calls ca_get_count()
 * and ca_get_der() to iterate over trusted root CA certificates in DER
 * format.  This module provides those roots and also offers:
 *
 *   ca_get_name()       -- human-readable label for a root slot
 *   ca_bundle_selftest()-- internal consistency check (passes even empty)
 *   ca_add_root_pem()   -- runtime root installation (no recompile needed)
 *
 * SECURITY NOTE -- EMPTY STORE IS THE SAFE DEFAULT
 * -------------------------------------------------
 * An empty trust store means:
 *   - All TLS connections are ENCRYPTED (the cipher suite still works).
 *   - All TLS connections are UNAUTHENTICATED (cert_trusted == 0).
 *   - x509_verify_chain() returns X509V_ERR_NO_ROOT for every chain.
 *   - tls_cert_trusted() returns 0 for every connection.
 *
 * This is the safe default: no fake or misremembered roots means no
 * false sense of security.  Populate via ca_roots_data.h (build-time)
 * or ca_add_root_pem() (runtime) with bit-perfect DER from authoritative
 * sources (openssl x509 -outform DER | xxd -i).
 *
 * INTEGRATION
 * -----------
 * #include "ca_bundle.h" in the same translation unit as x509_verify.c
 * (it already does: see #include "ca_bundle.h" in x509_verify.c).
 * Compile ca_bundle.c alongside the other tls/ objects.
 */

#ifndef TLS_CA_BUNDLE_H
#define TLS_CA_BUNDLE_H

/*
 * ca_get_count -- return number of trusted root CA certificates in the store.
 * Returns 0 when the bundle is empty (safe, means UNAUTHENTICATED -- see above).
 */
int ca_get_count(void);

/*
 * ca_get_der -- return a pointer to root CA certificate i's DER bytes.
 *
 *   i           zero-based index, 0 <= i < ca_get_count().
 *   out_len     on success, *out_len is set to the byte length of the DER blob.
 *
 * Returns a pointer to the DER bytes on success.
 * Returns NULL if i is out of range; *out_len is left unchanged.
 *
 * The pointer is valid for the lifetime of the process (static storage).
 * Do NOT modify the bytes.
 */
const unsigned char *ca_get_der(int i, unsigned long *out_len);

/*
 * ca_get_name -- return a NUL-terminated human label for root CA slot i.
 *
 *   i       zero-based index, 0 <= i < ca_get_count().
 *
 * Returns a pointer to a NUL-terminated string (static storage) on success.
 * Returns NULL if i is out of range.
 *
 * Examples: "ISRG Root X1", "DigiCert Global Root CA"
 */
const char *ca_get_name(int i);

/*
 * ca_bundle_selftest -- verify the internal consistency of the trust store.
 *
 * Checks for every entry that:
 *   - name pointer is non-NULL
 *   - der pointer is non-NULL
 *   - len > 0
 *   - the first byte of the DER blob is 0x30 (SEQUENCE tag -- minimal sanity)
 *
 * Returns 0 if all checks pass.  PASSES even when the bundle is empty
 * (zero roots is consistent -- there is simply nothing to check).
 * Returns -1 on any inconsistency.
 */
int ca_bundle_selftest(void);

/*
 * ca_add_root_pem -- install a root CA certificate at runtime from a PEM string.
 *
 *   name    NUL-terminated human label (stored by pointer -- caller must keep
 *           the string live, or pass a string literal).
 *   pem     pointer to PEM text (need not be NUL-terminated if pemlen is exact).
 *   pemlen  byte length of `pem`.
 *
 * The function locates the first "-----BEGIN CERTIFICATE-----" block in `pem`,
 * base64-decodes the body into an internal static slab (CA_POOL_BYTES total,
 * shared across all runtime-added roots), and appends the entry to the bundle.
 *
 * Returns  0  on success.
 * Returns -1  if the bundle is full (CA_MAX_ROOTS reached, both static + runtime).
 * Returns -2  if the DER slab is full (CA_POOL_BYTES exhausted).
 * Returns -3  if pem_extract_der() failed (malformed PEM / base64 error).
 * Returns -4  if name is NULL.
 *
 * Thread safety: NOT thread-safe; call from a single-threaded init context.
 *
 * DEPENDENCY: requires ../crypto/base64.h (pem_extract_der).
 */
int ca_add_root_pem(const char *name, const char *pem, unsigned long pemlen);

/*
 * Maximum entries the bundle can hold (static + runtime combined).
 * Increase if you need more roots; each entry is just a pointer + length.
 */
#define CA_MAX_ROOTS   32

/*
 * Total byte capacity of the runtime DER slab pool (used by ca_add_root_pem).
 * Sized to hold a modest set of runtime-installed roots (~10 x 2 KB each).
 * Increase if you need larger or more runtime roots.
 */
#define CA_POOL_BYTES  (20 * 1024)   /* 20 KB */

#endif /* TLS_CA_BUNDLE_H */
