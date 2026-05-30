/*
 * Signature Verification for Kernel and Modules
 */

#include "../include/crypto.h"
#include "../include/kernel.h"
#include "../include/mem.h"

// Global trusted keyring
static trusted_keyring_t g_keyring;
static bool g_keyring_initialized = false;

// Root public key (embedded in kernel at build time)
// This is a placeholder - actual key will be embedded during build
static const rsa_public_key_t g_root_key = {
    .bits = RSA_2048_BITS,
    .e = 65537,
    .n = {
        // This would be replaced with actual key during build
        // For now, placeholder values
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // ... (256 bytes total for RSA-2048)
    }
};

// Initialize trusted keyring
void crypto_init_keyring(void) {
    if (g_keyring_initialized) return;

    kprintf("[CRYPTO] Initializing trusted keyring...\n");

    for (uint32_t i = 0; i < MAX_TRUSTED_KEYS; i++) {
        g_keyring.keys[i].key_id = 0;
        g_keyring.keys[i].revoked = false;
    }
    g_keyring.count = 0;

    // Add root key (key ID 1)
    crypto_add_trusted_key(1, &g_root_key);

    g_keyring_initialized = true;
    kprintf("[CRYPTO] Trusted keyring initialized (%u keys)\n", g_keyring.count);
}

// Add trusted key
int crypto_add_trusted_key(uint32_t key_id, const rsa_public_key_t* key) {
    if (!key) return -1;

    if (g_keyring.count >= MAX_TRUSTED_KEYS) {
        kprintf("[CRYPTO] Keyring full\n");
        return -1;
    }

    // Check if key already exists
    for (uint32_t i = 0; i < g_keyring.count; i++) {
        if (g_keyring.keys[i].key_id == key_id) {
            kprintf("[CRYPTO] Key ID %u already exists\n", key_id);
            return -1;
        }
    }

    // Add key
    g_keyring.keys[g_keyring.count].key_id = key_id;
    g_keyring.keys[g_keyring.count].key = *key;
    g_keyring.keys[g_keyring.count].revoked = false;
    g_keyring.count++;

    kprintf("[CRYPTO] Added trusted key ID %u\n", key_id);
    return 0;
}

// Revoke key
int crypto_revoke_key(uint32_t key_id) {
    for (uint32_t i = 0; i < g_keyring.count; i++) {
        if (g_keyring.keys[i].key_id == key_id) {
            g_keyring.keys[i].revoked = true;
            kprintf("[CRYPTO] Revoked key ID %u\n", key_id);
            return 0;
        }
    }

    kprintf("[CRYPTO] Key ID %u not found\n", key_id);
    return -1;
}

// Get trusted key by ID
const rsa_public_key_t* crypto_get_trusted_key(uint32_t key_id) {
    for (uint32_t i = 0; i < g_keyring.count; i++) {
        if (g_keyring.keys[i].key_id == key_id && !g_keyring.keys[i].revoked) {
            return &g_keyring.keys[i].key;
        }
    }
    return NULL;
}

// Verify signed data (kernel or module)
int verify_signed_data(const uint8_t* data, size_t data_len,
                      const signature_header_t* sig_hdr,
                      const rsa_public_key_t* trusted_key) {
    if (!data || !sig_hdr || !trusted_key) {
        kprintf("[VERIFY] Invalid parameters\n");
        return -1;
    }

    kprintf("[VERIFY] Verifying signature (data_len=%zu, key_id=%u)\n",
            data_len, sig_hdr->key_id);

    // Check signature magic
    if (sig_hdr->magic != SIGNATURE_MAGIC) {
        kprintf("[VERIFY] Invalid signature magic: 0x%08x\n", sig_hdr->magic);
        return -1;
    }

    // Check signature version
    if (sig_hdr->version != 1) {
        kprintf("[VERIFY] Unsupported signature version: %u\n", sig_hdr->version);
        return -1;
    }

    // Check hash algorithm
    if (sig_hdr->hash_algo != 1) {
        kprintf("[VERIFY] Unsupported hash algorithm: %u\n", sig_hdr->hash_algo);
        return -1;
    }

    // Check signature algorithm
    if (sig_hdr->sig_algo != 1 && sig_hdr->sig_algo != 2) {
        kprintf("[VERIFY] Unsupported signature algorithm: %u\n", sig_hdr->sig_algo);
        return -1;
    }

    // Compute SHA-256 hash of data
    uint8_t computed_hash[SHA256_DIGEST_SIZE];
    kprintf("[VERIFY] Computing SHA-256 hash...\n");
    sha256_hash(data, data_len, computed_hash);

    // Compare with embedded hash
    if (crypto_memcmp_const(computed_hash, sig_hdr->hash, SHA256_DIGEST_SIZE) != 0) {
        kprintf("[VERIFY] Hash mismatch\n");
        kprintf("[VERIFY] Expected: ");
        crypto_print_hash(sig_hdr->hash, SHA256_DIGEST_SIZE);
        kprintf("[VERIFY] Computed: ");
        crypto_print_hash(computed_hash, SHA256_DIGEST_SIZE);
        return -1;
    }

    kprintf("[VERIFY] Hash verification PASSED\n");

    // Verify RSA signature
    kprintf("[VERIFY] Verifying RSA signature...\n");
    int result = rsa_verify_pkcs1_sha256(trusted_key, computed_hash, &sig_hdr->signature);

    if (result == 0) {
        kprintf("[VERIFY] Signature verification PASSED\n");
    } else {
        kprintf("[VERIFY] Signature verification FAILED\n");
    }

    return result;
}

// Constant-time memory comparison (timing-safe)
int crypto_memcmp_const(const uint8_t* a, const uint8_t* b, size_t len) {
    int result = 0;
    for (size_t i = 0; i < len; i++) {
        result |= a[i] ^ b[i];
    }
    return result;
}

// Print hash in hex format
void crypto_print_hash(const uint8_t* hash, size_t len) {
    for (size_t i = 0; i < len; i++) {
        kprintf("%02x", hash[i]);
    }
    kprintf("\n");
}
