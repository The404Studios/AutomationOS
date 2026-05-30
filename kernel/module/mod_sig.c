/*
 * Kernel Module Signature Verification
 * Verifies signatures on kernel modules before loading
 */

#include "../include/crypto.h"
#include "../include/kernel.h"
#include "../include/mem.h"

// Module signature format (appended to .ko file)
typedef struct {
    uint32_t magic;              // SIGNATURE_MAGIC
    uint32_t version;
    uint32_t hash_algo;
    uint32_t sig_algo;
    uint32_t key_id;
    uint8_t hash[SHA256_DIGEST_SIZE];
    rsa_signature_t signature;
} module_signature_t;

#define MODULE_SIG_SIZE sizeof(module_signature_t)

// Extract signature from module
static int extract_module_signature(const uint8_t* module_data, size_t module_size,
                                   const uint8_t** code, size_t* code_size,
                                   module_signature_t** sig) {
    if (!module_data || !code || !code_size || !sig) {
        return -1;
    }

    // Signature is appended to end of module
    if (module_size < MODULE_SIG_SIZE) {
        kprintf("[MODULE] Module too small to contain signature\n");
        return -1;
    }

    *code_size = module_size - MODULE_SIG_SIZE;
    *code = module_data;
    *sig = (module_signature_t*)(module_data + *code_size);

    // Verify magic
    if ((*sig)->magic != SIGNATURE_MAGIC) {
        kprintf("[MODULE] Invalid signature magic: 0x%08x\n", (*sig)->magic);
        return -1;
    }

    return 0;
}

// Verify module signature
int module_verify_signature(const uint8_t* module_data, size_t module_size) {
    if (!module_data) {
        kprintf("[MODULE] Invalid module data\n");
        return -1;
    }

    kprintf("[MODULE] Verifying module signature...\n");

    // Extract signature
    const uint8_t* code = NULL;
    size_t code_size = 0;
    module_signature_t* sig = NULL;

    if (extract_module_signature(module_data, module_size, &code, &code_size, &sig) != 0) {
        kprintf("[MODULE] Failed to extract signature\n");
        return -1;
    }

    kprintf("[MODULE] Module size: %zu bytes, code size: %zu bytes\n",
            module_size, code_size);
    kprintf("[MODULE] Signature: version=%u, key_id=%u\n",
            sig->version, sig->key_id);

    // Get trusted key
    const rsa_public_key_t* trusted_key = crypto_get_trusted_key(sig->key_id);
    if (!trusted_key) {
        kprintf("[MODULE] Trusted key ID %u not found or revoked\n", sig->key_id);
        return -1;
    }

    // Verify signature
    signature_header_t sig_hdr = {
        .magic = sig->magic,
        .version = sig->version,
        .hash_algo = sig->hash_algo,
        .sig_algo = sig->sig_algo,
        .key_id = sig->key_id,
        .signature = sig->signature
    };

    // Copy hash
    for (int i = 0; i < SHA256_DIGEST_SIZE; i++) {
        sig_hdr.hash[i] = sig->hash[i];
    }

    int result = verify_signed_data(code, code_size, &sig_hdr, trusted_key);

    if (result == 0) {
        kprintf("[MODULE] Signature verification PASSED\n");
    } else {
        kprintf("[MODULE] Signature verification FAILED\n");
    }

    return result;
}

// Check if module is signed
bool module_is_signed(const uint8_t* module_data, size_t module_size) {
    if (!module_data || module_size < MODULE_SIG_SIZE) {
        return false;
    }

    const module_signature_t* sig = (const module_signature_t*)
        (module_data + module_size - MODULE_SIG_SIZE);

    return (sig->magic == SIGNATURE_MAGIC);
}

// Policy: Require all modules to be signed
static bool g_require_module_signatures = true;

void module_set_signature_enforcement(bool enforce) {
    g_require_module_signatures = enforce;
    kprintf("[MODULE] Signature enforcement: %s\n",
            enforce ? "ENABLED" : "DISABLED");
}

bool module_signature_enforcement_enabled(void) {
    return g_require_module_signatures;
}
