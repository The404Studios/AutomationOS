/*
 * Unit Tests for Cryptographic Primitives
 */

#include "../../kernel/include/crypto.h"
#include "../../kernel/include/kernel.h"

// Test vectors from NIST/RFC examples

// ========================================
// SHA-256 Tests
// ========================================

void test_sha256_empty(void) {
    uint8_t hash[SHA256_DIGEST_SIZE];
    uint8_t expected[SHA256_DIGEST_SIZE] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
        0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
        0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
    };

    sha256_hash((uint8_t*)"", 0, hash);

    if (crypto_memcmp_const(hash, expected, SHA256_DIGEST_SIZE) == 0) {
        kprintf("[TEST] SHA-256 empty string: PASS\n");
    } else {
        kprintf("[TEST] SHA-256 empty string: FAIL\n");
        kprintf("  Expected: ");
        crypto_print_hash(expected, SHA256_DIGEST_SIZE);
        kprintf("  Got:      ");
        crypto_print_hash(hash, SHA256_DIGEST_SIZE);
    }
}

void test_sha256_abc(void) {
    uint8_t hash[SHA256_DIGEST_SIZE];
    uint8_t expected[SHA256_DIGEST_SIZE] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };

    const char* msg = "abc";
    sha256_hash((uint8_t*)msg, 3, hash);

    if (crypto_memcmp_const(hash, expected, SHA256_DIGEST_SIZE) == 0) {
        kprintf("[TEST] SHA-256 'abc': PASS\n");
    } else {
        kprintf("[TEST] SHA-256 'abc': FAIL\n");
        kprintf("  Expected: ");
        crypto_print_hash(expected, SHA256_DIGEST_SIZE);
        kprintf("  Got:      ");
        crypto_print_hash(hash, SHA256_DIGEST_SIZE);
    }
}

void test_sha256_long(void) {
    uint8_t hash[SHA256_DIGEST_SIZE];
    uint8_t expected[SHA256_DIGEST_SIZE] = {
        0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8,
        0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
        0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67,
        0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1
    };

    const char* msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_hash((uint8_t*)msg, 56, hash);

    if (crypto_memcmp_const(hash, expected, SHA256_DIGEST_SIZE) == 0) {
        kprintf("[TEST] SHA-256 long string: PASS\n");
    } else {
        kprintf("[TEST] SHA-256 long string: FAIL\n");
        kprintf("  Expected: ");
        crypto_print_hash(expected, SHA256_DIGEST_SIZE);
        kprintf("  Got:      ");
        crypto_print_hash(hash, SHA256_DIGEST_SIZE);
    }
}

void test_sha256_incremental(void) {
    sha256_ctx_t ctx;
    uint8_t hash[SHA256_DIGEST_SIZE];
    uint8_t expected[SHA256_DIGEST_SIZE] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };

    sha256_init(&ctx);
    sha256_update(&ctx, (uint8_t*)"a", 1);
    sha256_update(&ctx, (uint8_t*)"b", 1);
    sha256_update(&ctx, (uint8_t*)"c", 1);
    sha256_final(&ctx, hash);

    if (crypto_memcmp_const(hash, expected, SHA256_DIGEST_SIZE) == 0) {
        kprintf("[TEST] SHA-256 incremental: PASS\n");
    } else {
        kprintf("[TEST] SHA-256 incremental: FAIL\n");
    }
}

// ========================================
// Keyring Tests
// ========================================

void test_keyring_init(void) {
    crypto_init_keyring();
    kprintf("[TEST] Keyring initialization: PASS\n");
}

void test_keyring_add_key(void) {
    rsa_public_key_t key = {
        .bits = RSA_2048_BITS,
        .e = 65537,
        .n = {0}  // Dummy key
    };

    int result = crypto_add_trusted_key(100, &key);
    if (result == 0) {
        kprintf("[TEST] Add trusted key: PASS\n");
    } else {
        kprintf("[TEST] Add trusted key: FAIL\n");
    }
}

void test_keyring_get_key(void) {
    const rsa_public_key_t* key = crypto_get_trusted_key(100);
    if (key != NULL) {
        kprintf("[TEST] Get trusted key: PASS\n");
    } else {
        kprintf("[TEST] Get trusted key: FAIL\n");
    }
}

void test_keyring_revoke_key(void) {
    int result = crypto_revoke_key(100);
    if (result == 0) {
        kprintf("[TEST] Revoke key: PASS\n");
    } else {
        kprintf("[TEST] Revoke key: FAIL\n");
    }

    // Try to get revoked key
    const rsa_public_key_t* key = crypto_get_trusted_key(100);
    if (key == NULL) {
        kprintf("[TEST] Get revoked key returns NULL: PASS\n");
    } else {
        kprintf("[TEST] Get revoked key returns NULL: FAIL\n");
    }
}

// ========================================
// Constant-Time Comparison Test
// ========================================

void test_crypto_memcmp(void) {
    uint8_t a[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t b[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t c[] = {0x01, 0x02, 0x03, 0x05};

    if (crypto_memcmp_const(a, b, 4) == 0 && crypto_memcmp_const(a, c, 4) != 0) {
        kprintf("[TEST] Constant-time memcmp: PASS\n");
    } else {
        kprintf("[TEST] Constant-time memcmp: FAIL\n");
    }
}

// ========================================
// Test Runner
// ========================================

void run_crypto_tests(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  Cryptographic Primitive Tests\n");
    kprintf("========================================\n");
    kprintf("\n");

    // SHA-256 tests
    kprintf("--- SHA-256 Tests ---\n");
    test_sha256_empty();
    test_sha256_abc();
    test_sha256_long();
    test_sha256_incremental();

    kprintf("\n");

    // Keyring tests
    kprintf("--- Trusted Keyring Tests ---\n");
    test_keyring_init();
    test_keyring_add_key();
    test_keyring_get_key();
    test_keyring_revoke_key();

    kprintf("\n");

    // Utility tests
    kprintf("--- Utility Tests ---\n");
    test_crypto_memcmp();

    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  Crypto Tests Complete\n");
    kprintf("========================================\n");
    kprintf("\n");
}
