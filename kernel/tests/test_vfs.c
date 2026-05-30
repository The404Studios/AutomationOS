/**
 * VFS Subsystem Tests
 *
 * Tests basic VFS functionality
 */

#include "../include/vfs.h"
#include "../include/kernel.h"
#include "../include/mem.h"
#include <string.h>

/**
 * Test 1: VFS initialization
 */
static int test_vfs_init(void) {
    kprintf("[TEST] VFS: Testing initialization...\n");

    vfs_init();

    kprintf("[TEST] VFS: Initialization complete\n");
    return 0;
}

/**
 * Test 2: Mount ramfs
 */
static int test_vfs_mount(void) {
    kprintf("[TEST] VFS: Testing mount...\n");

    int result = vfs_mount("none", "/", "ramfs");
    if (result < 0) {
        kprintf("[TEST] VFS: Mount failed!\n");
        return -1;
    }

    kprintf("[TEST] VFS: Mount successful\n");
    return 0;
}

/**
 * Test 3: Create and read file
 */
static int test_vfs_file_ops(void) {
    kprintf("[TEST] VFS: Testing file operations...\n");

    // Get root inode
    vfs_inode_t* root = vfs_path_lookup("/");
    if (!root) {
        kprintf("[TEST] VFS: Failed to lookup root!\n");
        return -1;
    }

    // Create a test file
    const char* test_data = "Hello, VFS!";
    int result = vfs_ramfs_create_file(root, "test.txt",
                                       test_data, strlen(test_data), 0644);
    vfs_inode_put(root);

    if (result < 0) {
        kprintf("[TEST] VFS: Failed to create file!\n");
        return -1;
    }

    // Open the file
    int fd = vfs_open("/test.txt", O_RDONLY, 0);
    if (fd < 0) {
        kprintf("[TEST] VFS: Failed to open file!\n");
        return -1;
    }

    // Read the file
    char buffer[256];
    ssize_t bytes_read = vfs_read(fd, buffer, sizeof(buffer));
    if (bytes_read < 0) {
        kprintf("[TEST] VFS: Failed to read file!\n");
        vfs_close(fd);
        return -1;
    }

    buffer[bytes_read] = '\0';

    // Verify content
    if (strcmp(buffer, test_data) != 0) {
        kprintf("[TEST] VFS: Content mismatch! Expected '%s', got '%s'\n",
                test_data, buffer);
        vfs_close(fd);
        return -1;
    }

    // Close the file
    vfs_close(fd);

    kprintf("[TEST] VFS: File operations successful\n");
    return 0;
}

/**
 * Test 4: Write to file
 */
static int test_vfs_write(void) {
    kprintf("[TEST] VFS: Testing write operations...\n");

    // Get root inode
    vfs_inode_t* root = vfs_path_lookup("/");
    if (!root) {
        kprintf("[TEST] VFS: Failed to lookup root!\n");
        return -1;
    }

    // Create an empty file
    int result = vfs_ramfs_create_file(root, "write_test.txt", NULL, 0, 0644);
    vfs_inode_put(root);

    if (result < 0) {
        kprintf("[TEST] VFS: Failed to create file!\n");
        return -1;
    }

    // Open for writing
    int fd = vfs_open("/write_test.txt", O_RDWR, 0);
    if (fd < 0) {
        kprintf("[TEST] VFS: Failed to open file for writing!\n");
        return -1;
    }

    // Write data
    const char* write_data = "Test write data";
    ssize_t bytes_written = vfs_write(fd, write_data, strlen(write_data));
    if (bytes_written != (ssize_t)strlen(write_data)) {
        kprintf("[TEST] VFS: Write failed! Expected %zu, got %zd\n",
                strlen(write_data), bytes_written);
        vfs_close(fd);
        return -1;
    }

    // Seek to beginning
    vfs_lseek(fd, 0, SEEK_SET);

    // Read back
    char buffer[256];
    ssize_t bytes_read = vfs_read(fd, buffer, sizeof(buffer));
    if (bytes_read < 0) {
        kprintf("[TEST] VFS: Failed to read back!\n");
        vfs_close(fd);
        return -1;
    }

    buffer[bytes_read] = '\0';

    // Verify
    if (strcmp(buffer, write_data) != 0) {
        kprintf("[TEST] VFS: Content mismatch! Expected '%s', got '%s'\n",
                write_data, buffer);
        vfs_close(fd);
        return -1;
    }

    vfs_close(fd);

    kprintf("[TEST] VFS: Write operations successful\n");
    return 0;
}

/**
 * Test 5: File stat
 */
static int test_vfs_stat(void) {
    kprintf("[TEST] VFS: Testing stat operations...\n");

    vfs_stat_t st;
    int result = vfs_stat("/test.txt", &st);
    if (result < 0) {
        kprintf("[TEST] VFS: stat failed!\n");
        return -1;
    }

    kprintf("[TEST] VFS: File size: %lu bytes\n", st.st_size);
    kprintf("[TEST] VFS: Inode: %lu\n", st.st_ino);

    if (st.st_size != strlen("Hello, VFS!")) {
        kprintf("[TEST] VFS: Size mismatch!\n");
        return -1;
    }

    kprintf("[TEST] VFS: Stat operations successful\n");
    return 0;
}

/**
 * Run all VFS tests
 */
int test_vfs_run_all(void) {
    kprintf("\n");
    kprintf("====================================\n");
    kprintf("    VFS Test Suite\n");
    kprintf("====================================\n");
    kprintf("\n");

    int failed = 0;

    if (test_vfs_init() < 0) {
        kprintf("[FAIL] VFS initialization\n");
        failed++;
    } else {
        kprintf("[PASS] VFS initialization\n");
    }

    if (test_vfs_mount() < 0) {
        kprintf("[FAIL] VFS mount\n");
        failed++;
    } else {
        kprintf("[PASS] VFS mount\n");
    }

    if (test_vfs_file_ops() < 0) {
        kprintf("[FAIL] VFS file operations\n");
        failed++;
    } else {
        kprintf("[PASS] VFS file operations\n");
    }

    if (test_vfs_write() < 0) {
        kprintf("[FAIL] VFS write operations\n");
        failed++;
    } else {
        kprintf("[PASS] VFS write operations\n");
    }

    if (test_vfs_stat() < 0) {
        kprintf("[FAIL] VFS stat operations\n");
        failed++;
    } else {
        kprintf("[PASS] VFS stat operations\n");
    }

    kprintf("\n");
    kprintf("====================================\n");
    kprintf("    Test Results: %d failed\n", failed);
    kprintf("====================================\n");
    kprintf("\n");

    return failed;
}
