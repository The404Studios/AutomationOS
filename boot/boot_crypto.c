/*
 * Minimal Crypto for Bootloader
 * Standalone SHA-256 and RSA verification
 */

#include "boot_crypto.h"

// ========================================
// SHA-256 Implementation
// ========================================

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_transform(boot_sha256_ctx_t* ctx, const uint8_t* data) {
    uint32_t W[64];
    uint32_t a, b, c, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++) {
        W[i] = ((uint32_t)data[i * 4] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) |
               ((uint32_t)data[i * 4 + 3]);
    }

    for (i = 16; i < 64; i++) {
        W[i] = SIG1(W[i - 2]) + W[i - 7] + SIG0(W[i - 15]) + W[i - 16];
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + K[i] + W[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void boot_sha256_hash(const uint8_t* data, uint64_t len, uint8_t* digest) {
    boot_sha256_ctx_t ctx;

    // Initialize
    ctx.state[0] = 0x6a09e667;
    ctx.state[1] = 0xbb67ae85;
    ctx.state[2] = 0x3c6ef372;
    ctx.state[3] = 0xa54ff53a;
    ctx.state[4] = 0x510e527f;
    ctx.state[5] = 0x9b05688c;
    ctx.state[6] = 0x1f83d9ab;
    ctx.state[7] = 0x5be0cd19;
    ctx.count = 0;

    // Process complete blocks
    while (len >= SHA256_BLOCK_SIZE) {
        sha256_transform(&ctx, data);
        data += SHA256_BLOCK_SIZE;
        len -= SHA256_BLOCK_SIZE;
        ctx.count += SHA256_BLOCK_SIZE;
    }

    // Process remaining data
    uint64_t i;
    for (i = 0; i < len; i++) {
        ctx.buffer[i] = data[i];
    }
    ctx.count += len;

    // Padding
    ctx.buffer[i++] = 0x80;
    if (i > 56) {
        while (i < 64) ctx.buffer[i++] = 0;
        sha256_transform(&ctx, ctx.buffer);
        i = 0;
    }
    while (i < 56) ctx.buffer[i++] = 0;

    // Append length
    uint64_t bit_count = ctx.count * 8;
    ctx.buffer[56] = (uint8_t)(bit_count >> 56);
    ctx.buffer[57] = (uint8_t)(bit_count >> 48);
    ctx.buffer[58] = (uint8_t)(bit_count >> 40);
    ctx.buffer[59] = (uint8_t)(bit_count >> 32);
    ctx.buffer[60] = (uint8_t)(bit_count >> 24);
    ctx.buffer[61] = (uint8_t)(bit_count >> 16);
    ctx.buffer[62] = (uint8_t)(bit_count >> 8);
    ctx.buffer[63] = (uint8_t)(bit_count);

    sha256_transform(&ctx, ctx.buffer);

    // Output
    for (i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t)(ctx.state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx.state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx.state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(ctx.state[i]);
    }
}

// ========================================
// RSA Verification (Simplified)
// ========================================

static int memcmp_const(const uint8_t* a, const uint8_t* b, uint64_t len) {
    int result = 0;
    for (uint64_t i = 0; i < len; i++) {
        result |= a[i] ^ b[i];
    }
    return result;
}

// PKCS#1 v1.5 DigestInfo for SHA-256
static const uint8_t PKCS1_SHA256_DIGESTINFO[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
    0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
    0x00, 0x04, 0x20
};

int boot_rsa_verify(const boot_rsa_key_t* key, const uint8_t* hash,
                   const boot_rsa_sig_t* signature) {
    // This is a SIMPLIFIED verification
    // In production, use proper RSA implementation or TinyRSA

    // For now, we'll do basic structural checks
    // Full RSA modexp is complex for bootloader environment

    if (!key || !hash || !signature) return -1;
    if (signature->size != key->bits / 8) return -1;

    // TODO: Implement proper RSA verification
    // For MVP, we can:
    // 1. Use a crypto library (mbedtls minimal build)
    // 2. Implement simple modexp for e=65537
    // 3. Or rely on UEFI Secure Boot signature validation

    // No actual cryptographic verification implemented yet.
    // SECURITY: return FAILURE (-1) until real RSA modexp is implemented.
    // Returning 0 (success) here would let any signature pass.
    return -1;
}

// Embedded trusted key (placeholder - will be replaced at build time)
const boot_rsa_key_t BOOT_TRUSTED_KEY = {
    .bits = RSA_2048_BYTES * 8,
    .e = 65537,
    .n = { /* Will be filled by build system */ }
};

// Verify kernel signature
int boot_verify_kernel(const uint8_t* kernel_data, uint64_t kernel_size,
                      const boot_signature_header_t* sig_hdr) {
    if (!kernel_data || !sig_hdr) return -1;

    // Check magic
    if (sig_hdr->magic != SIGNATURE_MAGIC) return -1;

    // Check version
    if (sig_hdr->version != 1) return -1;

    // Compute hash
    uint8_t computed_hash[SHA256_DIGEST_SIZE];
    boot_sha256_hash(kernel_data, kernel_size, computed_hash);

    // Compare hashes
    if (memcmp_const(computed_hash, sig_hdr->hash, SHA256_DIGEST_SIZE) != 0) {
        return -1;
    }

    // Verify RSA signature
    return boot_rsa_verify(&BOOT_TRUSTED_KEY, computed_hash, &sig_hdr->signature);
}
