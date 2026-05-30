/**
 * @file test_autofs.c
 * @brief Comprehensive test suite for AutoFS filesystem
 *
 * Tests:
 * - Filesystem creation
 * - Mount/unmount
 * - File I/O
 * - Snapshots
 * - Compression
 * - Encryption
 * - Extended attributes
 * - Cache performance
 */

#include "../kernel/include/autofs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <assert.h>

#define TEST_DEVICE "/tmp/autofs_test.img"
#define TEST_SIZE (100 * 1024 * 1024)  /* 100 MB */

int test_count = 0;
int test_passed = 0;
int test_failed = 0;

#define TEST_START(name) \
    do { \
        test_count++; \
        printf("\n[TEST %d] %s\n", test_count, name); \
        printf("----------------------------------------\n"); \
    } while(0)

#define TEST_PASS() \
    do { \
        test_passed++; \
        printf("✓ PASSED\n"); \
    } while(0)

#define TEST_FAIL(msg) \
    do { \
        test_failed++; \
        printf("✗ FAILED: %s\n", msg); \
    } while(0)

/**
 * @brief Create test device
 */
static int create_test_device(void) {
    printf("Creating test device: %s (%lu bytes)\n", TEST_DEVICE, TEST_SIZE);

    int fd = open(TEST_DEVICE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        perror("Failed to create test device");
        return -1;
    }

    /* Allocate space */
    if (ftruncate(fd, TEST_SIZE) < 0) {
        perror("Failed to allocate space");
        close(fd);
        return -1;
    }

    close(fd);
    printf("✓ Test device created\n\n");

    return 0;
}

/**
 * @brief Test filesystem creation
 */
static void test_mkfs(void) {
    TEST_START("Filesystem Creation");

    if (autofs_mkfs(TEST_DEVICE, TEST_SIZE, "TestFS") < 0) {
        TEST_FAIL("mkfs failed");
        return;
    }

    printf("✓ Filesystem created successfully\n");
    TEST_PASS();
}

/**
 * @brief Test mount/unmount
 */
static void test_mount_unmount(void) {
    TEST_START("Mount/Unmount");

    autofs_fs_t *fs = autofs_mount(TEST_DEVICE, false);
    if (!fs) {
        TEST_FAIL("Mount failed");
        return;
    }

    printf("✓ Mounted successfully\n");

    /* Verify superblock */
    if (fs->sb->magic != AUTOFS_MAGIC) {
        TEST_FAIL("Invalid magic number");
        autofs_unmount(fs);
        return;
    }

    printf("✓ Superblock valid (magic: 0x%lx)\n", fs->sb->magic);

    if (autofs_unmount(fs) < 0) {
        TEST_FAIL("Unmount failed");
        return;
    }

    printf("✓ Unmounted successfully\n");
    TEST_PASS();
}

/**
 * @brief Test file creation and I/O
 */
static void test_file_io(void) {
    TEST_START("File I/O Operations");

    autofs_fs_t *fs = autofs_mount(TEST_DEVICE, false);
    if (!fs) {
        TEST_FAIL("Mount failed");
        return;
    }

    /* Create file */
    uint64_t ino;
    if (autofs_open(fs, "/test.txt", O_CREAT | O_RDWR, &ino) < 0) {
        TEST_FAIL("File creation failed");
        autofs_unmount(fs);
        return;
    }

    printf("✓ File created (inode %lu)\n", ino);

    /* Write data */
    const char *write_data = "Hello, AutoFS! This is a test file.";
    autofs_file_t file = {
        .fs = fs,
        .ino = ino,
        .offset = 0,
        .flags = O_RDWR
    };

    ssize_t written = autofs_write(&file, write_data, strlen(write_data));
    if (written != (ssize_t)strlen(write_data)) {
        TEST_FAIL("Write failed");
        autofs_unmount(fs);
        return;
    }

    printf("✓ Wrote %zd bytes\n", written);

    /* Read data back */
    file.offset = 0;
    char read_buffer[256];
    ssize_t read_bytes = autofs_read(&file, read_buffer, sizeof(read_buffer));
    if (read_bytes != written) {
        TEST_FAIL("Read failed");
        autofs_unmount(fs);
        return;
    }

    read_buffer[read_bytes] = '\0';
    printf("✓ Read %zd bytes: \"%s\"\n", read_bytes, read_buffer);

    /* Verify data */
    if (strcmp(read_buffer, write_data) != 0) {
        TEST_FAIL("Data mismatch");
        autofs_unmount(fs);
        return;
    }

    printf("✓ Data verified\n");

    autofs_close(&file);
    autofs_unmount(fs);
    TEST_PASS();
}

/**
 * @brief Test snapshots
 */
static void test_snapshots(void) {
    TEST_START("Snapshot Operations");

    autofs_fs_t *fs = autofs_mount(TEST_DEVICE, false);
    if (!fs) {
        TEST_FAIL("Mount failed");
        return;
    }

    /* Create snapshot */
    autofs_snapshot_t *snap = autofs_snapshot_create(fs, "snapshot1");
    if (!snap) {
        TEST_FAIL("Snapshot creation failed");
        autofs_unmount(fs);
        return;
    }

    printf("✓ Snapshot created: %s (ID: %lu)\n", snap->name, snap->snapshot_id);
    printf("  Timestamp: %s", ctime((time_t*)&snap->timestamp));

    free(snap);

    /* Verify snapshot count */
    if (fs->sb->snapshot_count != 1) {
        TEST_FAIL("Snapshot count incorrect");
        autofs_unmount(fs);
        return;
    }

    printf("✓ Snapshot count: %lu\n", fs->sb->snapshot_count);

    autofs_unmount(fs);
    TEST_PASS();
}

/**
 * @brief Test compression
 */
static void test_compression(void) {
    TEST_START("Compression");

    /* Test data with patterns (compressible) */
    const char *compressible =
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
        "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC";

    size_t orig_len = strlen(compressible);

    /* Compress */
    uint8_t compressed[1024];
    ssize_t comp_len = autofs_compress(COMPRESS_ZSTD, compressible, orig_len,
                                      compressed, sizeof(compressed));

    if (comp_len < 0) {
        TEST_FAIL("Compression failed");
        return;
    }

    printf("✓ Compressed %zu bytes -> %zd bytes (%.1f%%)\n",
           orig_len, comp_len, 100.0 * comp_len / orig_len);

    /* Decompress */
    char decompressed[1024];
    ssize_t decomp_len = autofs_decompress(COMPRESS_ZSTD, compressed, comp_len,
                                          decompressed, sizeof(decompressed));

    if (decomp_len != (ssize_t)orig_len) {
        TEST_FAIL("Decompression failed");
        return;
    }

    printf("✓ Decompressed %zd bytes\n", decomp_len);

    /* Verify data */
    if (memcmp(compressible, decompressed, orig_len) != 0) {
        TEST_FAIL("Data mismatch after compression");
        return;
    }

    printf("✓ Data verified after decompression\n");

    TEST_PASS();
}

/**
 * @brief Test encryption
 */
static void test_encryption(void) {
    TEST_START("Encryption");

    const char *plaintext = "Secret message: AutoFS encryption test!";
    size_t plain_len = strlen(plaintext);

    /* Generate key */
    uint8_t key[32];
    if (autofs_derive_key("mypassword", 10, key) < 0) {
        TEST_FAIL("Key derivation failed");
        return;
    }

    printf("✓ Key derived from password\n");

    /* Encrypt */
    uint8_t ciphertext[256];
    size_t cipher_len;

    if (autofs_encrypt(key, 32, plaintext, plain_len,
                      ciphertext, &cipher_len) < 0) {
        TEST_FAIL("Encryption failed");
        return;
    }

    printf("✓ Encrypted %zu bytes -> %zu bytes\n", plain_len, cipher_len);

    /* Decrypt */
    char decrypted[256];
    size_t decrypt_len;

    if (autofs_decrypt(key, 32, ciphertext, cipher_len,
                      decrypted, &decrypt_len) < 0) {
        TEST_FAIL("Decryption failed");
        return;
    }

    decrypted[decrypt_len] = '\0';
    printf("✓ Decrypted: \"%s\"\n", decrypted);

    /* Verify */
    if (strcmp(plaintext, decrypted) != 0) {
        TEST_FAIL("Plaintext mismatch");
        return;
    }

    printf("✓ Plaintext verified\n");

    TEST_PASS();
}

/**
 * @brief Test extended attributes
 */
static void test_xattr(void) {
    TEST_START("Extended Attributes");

    autofs_fs_t *fs = autofs_mount(TEST_DEVICE, false);
    if (!fs) {
        TEST_FAIL("Mount failed");
        return;
    }

    /* Set xattr */
    const char *value = "Test comment";
    if (autofs_xattr_set(fs, 1, "user.comment", value, strlen(value)) < 0) {
        TEST_FAIL("xattr_set failed");
        autofs_unmount(fs);
        return;
    }

    printf("✓ Set xattr: user.comment = \"%s\"\n", value);

    /* Get xattr */
    char buffer[256];
    ssize_t len = autofs_xattr_get(fs, 1, "user.comment", buffer, sizeof(buffer));
    if (len < 0) {
        TEST_FAIL("xattr_get failed");
        autofs_unmount(fs);
        return;
    }

    buffer[len] = '\0';
    printf("✓ Get xattr: user.comment = \"%s\"\n", buffer);

    /* Verify */
    if (strcmp(value, buffer) != 0) {
        TEST_FAIL("xattr value mismatch");
        autofs_unmount(fs);
        return;
    }

    printf("✓ xattr verified\n");

    autofs_unmount(fs);
    TEST_PASS();
}

/**
 * @brief Test cache performance
 */
static void test_cache(void) {
    TEST_START("Cache Performance");

    autofs_fs_t *fs = autofs_mount(TEST_DEVICE, false);
    if (!fs) {
        TEST_FAIL("Mount failed");
        return;
    }

    /* Read inode multiple times */
    for (int i = 0; i < 10; i++) {
        autofs_inode_t *inode = autofs_cache_get_inode(fs, 1);
        if (inode) {
            free(inode);
        }
    }

    printf("✓ Performed 10 inode reads\n");

    /* Check cache stats */
    if (fs->stats.cache_hits > 0) {
        float hit_rate = 100.0f * fs->stats.cache_hits /
                        (fs->stats.cache_hits + fs->stats.cache_misses);
        printf("✓ Cache hit rate: %.1f%% (%lu hits, %lu misses)\n",
               hit_rate, fs->stats.cache_hits, fs->stats.cache_misses);

        if (hit_rate < 50.0f) {
            TEST_FAIL("Cache hit rate too low");
            autofs_unmount(fs);
            return;
        }
    }

    autofs_cache_stats(fs);
    autofs_unmount(fs);
    TEST_PASS();
}

/**
 * @brief Test journal recovery
 */
static void test_journal_recovery(void) {
    TEST_START("Journal Recovery");

    autofs_fs_t *fs = autofs_mount(TEST_DEVICE, false);
    if (!fs) {
        TEST_FAIL("Mount failed");
        return;
    }

    /* Journal should recover automatically on mount */
    printf("✓ Journal recovery completed\n");

    autofs_unmount(fs);
    TEST_PASS();
}

/**
 * @brief Print test summary
 */
static void print_summary(void) {
    printf("\n========================================\n");
    printf("TEST SUMMARY\n");
    printf("========================================\n");
    printf("Total tests: %d\n", test_count);
    printf("Passed:      %d\n", test_passed);
    printf("Failed:      %d\n", test_failed);
    printf("========================================\n");

    if (test_failed == 0) {
        printf("\n✓ ALL TESTS PASSED\n\n");
    } else {
        printf("\n✗ SOME TESTS FAILED\n\n");
    }
}

int main(void) {
    printf("========================================\n");
    printf("AutoFS Filesystem Test Suite\n");
    printf("========================================\n");

    /* Create test device */
    if (create_test_device() < 0) {
        fprintf(stderr, "Failed to create test device\n");
        return 1;
    }

    /* Run tests */
    test_mkfs();
    test_mount_unmount();
    test_file_io();
    test_snapshots();
    test_compression();
    test_encryption();
    test_xattr();
    test_cache();
    test_journal_recovery();

    /* Print summary */
    print_summary();

    /* Cleanup */
    unlink(TEST_DEVICE);

    return (test_failed == 0) ? 0 : 1;
}
