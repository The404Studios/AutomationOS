/*
 * Minimal Crypto for Bootloader
 * SHA-256 and RSA verification for kernel signature checking
 */

#ifndef BOOT_CRYPTO_H
#define BOOT_CRYPTO_H

#include <stdint.h>

// SHA-256
#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE 64

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[SHA256_BLOCK_SIZE];
} boot_sha256_ctx_t;

void boot_sha256_hash(const uint8_t* data, uint64_t len, uint8_t* digest);

// RSA
#define RSA_2048_BYTES 256
#define RSA_4096_BYTES 512

typedef struct {
    uint32_t bits;
    uint32_t e;
    uint8_t n[RSA_4096_BYTES];
} boot_rsa_key_t;

typedef struct {
    uint32_t size;
    uint8_t data[RSA_4096_BYTES];
} boot_rsa_sig_t;

int boot_rsa_verify(const boot_rsa_key_t* key, const uint8_t* hash,
                   const boot_rsa_sig_t* signature);

// Signature header (matches kernel crypto.h)
#define SIGNATURE_MAGIC 0x5349474E

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t hash_algo;
    uint32_t sig_algo;
    uint32_t key_id;
    uint8_t hash[SHA256_DIGEST_SIZE];
    boot_rsa_sig_t signature;
} boot_signature_header_t;

// Embedded trusted public key (build-time generated)
extern const boot_rsa_key_t BOOT_TRUSTED_KEY;

// Verify kernel signature
int boot_verify_kernel(const uint8_t* kernel_data, uint64_t kernel_size,
                      const boot_signature_header_t* sig_hdr);

#endif
