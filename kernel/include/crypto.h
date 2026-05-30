#ifndef CRYPTO_H
#define CRYPTO_H

#include "types.h"

// ========================================
// SHA-256 Hashing
// ========================================

#define SHA256_BLOCK_SIZE 64
#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[SHA256_BLOCK_SIZE];
} sha256_ctx_t;

void sha256_init(sha256_ctx_t* ctx);
void sha256_update(sha256_ctx_t* ctx, const uint8_t* data, size_t len);
void sha256_final(sha256_ctx_t* ctx, uint8_t* digest);
void sha256_hash(const uint8_t* data, size_t len, uint8_t* digest);

// ========================================
// RSA Signature Verification
// ========================================

#define RSA_2048_BITS 2048
#define RSA_2048_BYTES 256
#define RSA_4096_BITS 4096
#define RSA_4096_BYTES 512

// RSA public key
typedef struct {
    uint32_t bits;           // Key size in bits (2048 or 4096)
    uint32_t e;              // Public exponent (typically 65537)
    uint8_t n[RSA_4096_BYTES]; // Modulus (big-endian)
} rsa_public_key_t;

// RSA signature
typedef struct {
    uint32_t size;           // Signature size in bytes
    uint8_t data[RSA_4096_BYTES]; // Signature data (big-endian)
} rsa_signature_t;

// Verify RSA-PKCS#1 v1.5 signature with SHA-256 hash
int rsa_verify_pkcs1_sha256(const rsa_public_key_t* key,
                            const uint8_t* hash,
                            const rsa_signature_t* signature);

// ========================================
// X.509 Certificate Parsing (Simple Subset)
// ========================================

#define X509_MAX_DN_SIZE 256
#define X509_MAX_SERIAL_SIZE 32

typedef struct {
    // Subject Distinguished Name
    char subject_cn[128];        // Common Name
    char subject_o[128];         // Organization

    // Issuer Distinguished Name
    char issuer_cn[128];
    char issuer_o[128];

    // Serial number
    uint8_t serial[X509_MAX_SERIAL_SIZE];
    uint32_t serial_len;

    // Validity period (Unix timestamps)
    uint64_t not_before;
    uint64_t not_after;

    // Public key
    rsa_public_key_t public_key;

    // Signature
    rsa_signature_t signature;
} x509_cert_t;

// Parse DER-encoded X.509 certificate (simplified subset)
int x509_parse_cert(const uint8_t* der_data, size_t der_len, x509_cert_t* cert);

// Verify certificate signature (self-signed or chain)
int x509_verify_cert(const x509_cert_t* cert, const rsa_public_key_t* issuer_key);

// ========================================
// Secure Boot Structures
// ========================================

// Signature header (appended to kernel/module)
#define SIGNATURE_MAGIC 0x5349474E  // "SIGN"

typedef struct {
    uint32_t magic;              // SIGNATURE_MAGIC
    uint32_t version;            // Signature format version (1)
    uint32_t hash_algo;          // Hash algorithm (1 = SHA-256)
    uint32_t sig_algo;           // Signature algorithm (1 = RSA-2048, 2 = RSA-4096)
    uint32_t key_id;             // Key ID (for key rotation)
    uint8_t hash[SHA256_DIGEST_SIZE];  // SHA-256 hash of data
    rsa_signature_t signature;   // RSA signature of hash
} signature_header_t;

// Verify signed data (kernel or module)
int verify_signed_data(const uint8_t* data, size_t data_len,
                      const signature_header_t* sig_hdr,
                      const rsa_public_key_t* trusted_key);

// ========================================
// Trusted Key Storage
// ========================================

#define MAX_TRUSTED_KEYS 16

typedef struct {
    uint32_t key_id;
    rsa_public_key_t key;
    bool revoked;
} trusted_key_entry_t;

typedef struct {
    trusted_key_entry_t keys[MAX_TRUSTED_KEYS];
    uint32_t count;
} trusted_keyring_t;

// Initialize trusted keyring
void crypto_init_keyring(void);

// Add trusted key
int crypto_add_trusted_key(uint32_t key_id, const rsa_public_key_t* key);

// Revoke key
int crypto_revoke_key(uint32_t key_id);

// Get trusted key by ID
const rsa_public_key_t* crypto_get_trusted_key(uint32_t key_id);

// ========================================
// Utility Functions
// ========================================

// Compare two byte arrays in constant time (timing-safe)
int crypto_memcmp_const(const uint8_t* a, const uint8_t* b, size_t len);

// Print hash in hex format (for debug)
void crypto_print_hash(const uint8_t* hash, size_t len);

#endif
