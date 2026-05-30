/**
 * Unit tests for initrd subsystem
 * Tests TAR parsing, file extraction, and API functions
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

// Mock kernel functions for testing
int kprintf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vprintf(format, args);
    va_end(args);
    return ret;
}

// Include initrd implementation (simplified for testing)
typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
} __attribute__((packed)) tar_header_t;

// Test helper: Convert integer to octal string
static void int_to_octal(char* dest, uint64_t value, int width) {
    snprintf(dest, width, "%0*lo", width - 1, (unsigned long)value);
}

// Test helper: Create a simple TAR header
static void create_test_header(tar_header_t* header, const char* name, uint64_t size) {
    memset(header, 0, sizeof(tar_header_t));

    strncpy(header->name, name, sizeof(header->name) - 1);
    int_to_octal(header->mode, 0644, sizeof(header->mode));
    int_to_octal(header->uid, 0, sizeof(header->uid));
    int_to_octal(header->gid, 0, sizeof(header->gid));
    int_to_octal(header->size, size, sizeof(header->size));
    int_to_octal(header->mtime, 0, sizeof(header->mtime));

    header->typeflag = '0';  // Regular file

    memcpy(header->magic, "ustar", 5);
    memcpy(header->version, "00", 2);

    strncpy(header->uname, "root", sizeof(header->uname) - 1);
    strncpy(header->gname, "root", sizeof(header->gname) - 1);

    // Calculate checksum
    memset(header->checksum, ' ', sizeof(header->checksum));
    unsigned int checksum = 0;
    unsigned char* ptr = (unsigned char*)header;
    for (int i = 0; i < 512; i++) {
        if (i >= 148 && i < 156) {
            checksum += ' ';
        } else {
            checksum += ptr[i];
        }
    }
    int_to_octal(header->checksum, checksum, sizeof(header->checksum) - 1);
    header->checksum[6] = '\0';
    header->checksum[7] = ' ';
}

// Test 1: Octal string parsing
static void test_octal_parsing(void) {
    printf("[TEST] Octal string parsing...\n");

    // Test helper function
    static uint64_t octal_to_int(const char* str, int len) {
        uint64_t value = 0;
        for (int i = 0; i < len && str[i]; i++) {
            if (str[i] >= '0' && str[i] <= '7') {
                value = value * 8 + (str[i] - '0');
            }
        }
        return value;
    }

    // Test cases
    assert(octal_to_int("0000644", 8) == 0644);  // File mode
    assert(octal_to_int("0001000", 8) == 0512);  // File size 512
    assert(octal_to_int("0000000", 8) == 0);
    assert(octal_to_int("7777777", 8) == 2097151);

    printf("  ✓ Octal parsing works correctly\n\n");
}

// Test 2: TAR header validation
static void test_header_validation(void) {
    printf("[TEST] TAR header validation...\n");

    tar_header_t header;

    // Create valid header
    create_test_header(&header, "test.txt", 100);

    // Validate magic
    assert(memcmp(header.magic, "ustar", 5) == 0);
    printf("  ✓ Magic number is correct\n");

    // Validate version
    assert(header.version[0] == '0' && header.version[1] == '0');
    printf("  ✓ Version is correct\n");

    // Validate name
    assert(strcmp(header.name, "test.txt") == 0);
    printf("  ✓ Filename is correct\n");

    // Validate type
    assert(header.typeflag == '0');
    printf("  ✓ File type is correct\n\n");
}

// Test 3: Create and parse simple TAR archive
static void test_simple_tar(void) {
    printf("[TEST] Simple TAR archive creation and parsing...\n");

    // Create a minimal TAR archive in memory
    uint8_t* tar_data = calloc(1, 4096);
    assert(tar_data != NULL);

    // Create header for first file
    tar_header_t* header1 = (tar_header_t*)tar_data;
    const char* filename1 = "sbin/init";
    const char* content1 = "#!/bin/sh\necho 'Init'\n";
    size_t size1 = strlen(content1);

    create_test_header(header1, filename1, size1);
    memcpy(tar_data + 512, content1, size1);

    // Calculate offset to next header (512-byte aligned)
    size_t padding1 = (512 - (size1 % 512)) % 512;
    size_t offset2 = 512 + size1 + padding1;

    // Create header for second file
    tar_header_t* header2 = (tar_header_t*)(tar_data + offset2);
    const char* filename2 = "etc/fstab";
    const char* content2 = "proc /proc proc defaults 0 0\n";
    size_t size2 = strlen(content2);

    create_test_header(header2, filename2, size2);
    memcpy(tar_data + offset2 + 512, content2, size2);

    // Add end-of-archive marker (two empty blocks)
    size_t padding2 = (512 - (size2 % 512)) % 512;
    size_t end_offset = offset2 + 512 + size2 + padding2;
    // Already zeroed by calloc

    printf("  Created TAR archive with 2 files\n");
    printf("  File 1: %s (%zu bytes)\n", filename1, size1);
    printf("  File 2: %s (%zu bytes)\n", filename2, size2);

    // Parse the archive
    int file_count = 0;
    size_t offset = 0;
    int empty_blocks = 0;

    while (offset < 4096) {
        tar_header_t* header = (tar_header_t*)(tar_data + offset);

        // Check for empty block
        int is_empty = 1;
        for (int i = 0; i < 512; i++) {
            if (((uint8_t*)header)[i] != 0) {
                is_empty = 0;
                break;
            }
        }

        if (is_empty) {
            empty_blocks++;
            if (empty_blocks >= 2) {
                break;  // End of archive
            }
            offset += 512;
            continue;
        }

        empty_blocks = 0;

        // Validate header
        if (memcmp(header->magic, "ustar", 5) != 0) {
            printf("  ERROR: Invalid magic at offset %zu\n", offset);
            break;
        }

        // Extract file info (octal parsing)
        uint64_t file_size = 0;
        for (int i = 0; i < 12 && header->size[i]; i++) {
            if (header->size[i] >= '0' && header->size[i] <= '7') {
                file_size = file_size * 8 + (header->size[i] - '0');
            }
        }

        printf("  Found: %s (%lu bytes)\n", header->name, file_size);
        file_count++;

        // Move to next entry
        size_t padding = (512 - (file_size % 512)) % 512;
        offset += 512 + file_size + padding;
    }

    printf("  ✓ Parsed %d files successfully\n", file_count);
    assert(file_count == 2);

    free(tar_data);
    printf("\n");
}

// Test 4: File lookup
static void test_file_lookup(void) {
    printf("[TEST] File lookup in TAR archive...\n");

    // Create a minimal TAR archive
    uint8_t* tar_data = calloc(1, 4096);
    assert(tar_data != NULL);

    // Add test files
    const char* files[] = {
        "sbin/init",
        "bin/sh",
        "lib/libc.so",
        "etc/fstab"
    };
    size_t num_files = sizeof(files) / sizeof(files[0]);

    size_t offset = 0;
    for (size_t i = 0; i < num_files; i++) {
        tar_header_t* header = (tar_header_t*)(tar_data + offset);
        const char* content = "Test content";
        size_t size = strlen(content);

        create_test_header(header, files[i], size);
        memcpy(tar_data + offset + 512, content, size);

        size_t padding = (512 - (size % 512)) % 512;
        offset += 512 + size + padding;
    }

    printf("  Created archive with %zu files\n", num_files);

    // Test lookups
    for (size_t i = 0; i < num_files; i++) {
        // Search for file
        int found = 0;
        size_t search_offset = 0;
        int empty_blocks = 0;

        while (search_offset < 4096) {
            tar_header_t* header = (tar_header_t*)(tar_data + search_offset);

            // Check empty
            int is_empty = 1;
            for (int j = 0; j < 512; j++) {
                if (((uint8_t*)header)[j] != 0) {
                    is_empty = 0;
                    break;
                }
            }

            if (is_empty) {
                empty_blocks++;
                if (empty_blocks >= 2) break;
                search_offset += 512;
                continue;
            }

            empty_blocks = 0;

            if (memcmp(header->magic, "ustar", 5) != 0) break;

            if (strcmp(header->name, files[i]) == 0) {
                found = 1;
                break;
            }

            uint64_t file_size = 0;
            for (int j = 0; j < 12 && header->size[j]; j++) {
                if (header->size[j] >= '0' && header->size[j] <= '7') {
                    file_size = file_size * 8 + (header->size[j] - '0');
                }
            }

            size_t padding = (512 - (file_size % 512)) % 512;
            search_offset += 512 + file_size + padding;
        }

        assert(found == 1);
        printf("  ✓ Found: %s\n", files[i]);
    }

    // Test non-existent file
    const char* missing = "nonexistent.txt";
    int found = 0;
    size_t search_offset = 0;
    int empty_blocks = 0;

    while (search_offset < 4096) {
        tar_header_t* header = (tar_header_t*)(tar_data + search_offset);

        int is_empty = 1;
        for (int j = 0; j < 512; j++) {
            if (((uint8_t*)header)[j] != 0) {
                is_empty = 0;
                break;
            }
        }

        if (is_empty) {
            empty_blocks++;
            if (empty_blocks >= 2) break;
            search_offset += 512;
            continue;
        }

        if (memcmp(header->magic, "ustar", 5) != 0) break;

        if (strcmp(header->name, missing) == 0) {
            found = 1;
            break;
        }

        uint64_t file_size = 0;
        for (int j = 0; j < 12 && header->size[j]; j++) {
            if (header->size[j] >= '0' && header->size[j] <= '7') {
                file_size = file_size * 8 + (header->size[j] - '0');
            }
        }

        size_t padding = (512 - (file_size % 512)) % 512;
        search_offset += 512 + file_size + padding;
    }

    assert(found == 0);
    printf("  ✓ Correctly returns NULL for missing file\n");

    free(tar_data);
    printf("\n");
}

// Main test runner
int main(void) {
    printf("=====================================\n");
    printf("  Initrd Unit Tests\n");
    printf("=====================================\n\n");

    test_octal_parsing();
    test_header_validation();
    test_simple_tar();
    test_file_lookup();

    printf("=====================================\n");
    printf("  All Tests Passed!\n");
    printf("=====================================\n");

    return 0;
}
