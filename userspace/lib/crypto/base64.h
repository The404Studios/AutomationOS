/*
 * Base64 Encode/Decode + PEM Block Extraction
 * ============================================
 *
 * Freestanding, pure-computation library.
 * No syscalls, no libc, no malloc, no standard headers.
 *
 * Build flags expected:
 *   -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
 *   -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
 *
 * All functions operate on caller-provided buffers.
 */

#ifndef USERSPACE_LIB_CRYPTO_BASE64_H
#define USERSPACE_LIB_CRYPTO_BASE64_H

/* Freestanding integer types — mirror the kernel's types.h convention */
typedef unsigned char      b64_u8;
typedef unsigned long      b64_ulong;   /* guaranteed >= 32 bits; use long for ABI compat */
typedef long               b64_long;

/*
 * base64_encode
 *
 * Encodes `inlen` bytes from `in` into standard RFC 4648 Base64 (alphabet
 * A-Z a-z 0-9 +/ with '=' padding) and writes the NUL-terminated result
 * into `out` (capacity `outcap` bytes).
 *
 * Returns the encoded length (excluding the NUL terminator) on success,
 * or -1 if `outcap` is too small to hold the result.
 *
 * Required outcap: ((inlen + 2) / 3) * 4 + 1  (for the NUL).
 */
b64_long base64_encode(const b64_u8 *in, b64_ulong inlen,
                       char *out, b64_ulong outcap);

/*
 * base64_decode
 *
 * Decodes Base64 data from `in` (length `inlen`) into `out` (capacity
 * `outcap` bytes).
 *
 * Accepts both standard alphabet (+/) and URL-safe alphabet (-_).
 * Whitespace characters (space, \t, \r, \n) are silently ignored,
 * allowing PEM-wrapped input.
 * Trailing '=' padding characters are handled correctly.
 *
 * Returns the number of decoded bytes on success, or -1 on error
 * (bad character, truncated input, or output buffer too small).
 */
b64_long base64_decode(const char *in, b64_ulong inlen,
                       b64_u8 *out, b64_ulong outcap);

/*
 * pem_extract_der
 *
 * Locates the first PEM block matching `type` inside the NUL-terminated
 * (or length-bounded) buffer `pem` / `pemlen`, base64-decodes its body
 * into `out` (capacity `outcap` bytes), and returns the number of DER
 * bytes written.
 *
 * The function searches for:
 *   -----BEGIN <type>-----
 *   ... base64 body ...
 *   -----END <type>-----
 *
 * `type` examples: "CERTIFICATE", "RSA PRIVATE KEY", "PUBLIC KEY"
 *
 * Returns the DER byte count on success, or -1 if:
 *   - the BEGIN/END markers are not found
 *   - the base64 body is invalid
 *   - `outcap` is too small
 */
b64_long pem_extract_der(const char *pem, b64_ulong pemlen,
                         const char *type,
                         b64_u8 *out, b64_ulong outcap);

/*
 * base64_selftest
 *
 * Runs the RFC 4648 test vectors, decode round-trips, and a PEM
 * round-trip.  Returns 0 if all tests pass, -1 otherwise.
 */
int base64_selftest(void);

#endif /* USERSPACE_LIB_CRYPTO_BASE64_H */
