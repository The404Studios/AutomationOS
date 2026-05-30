// Test program for directory syscalls

#include "libc/stdio.h"
#include "libc/dirent.h"
#include "libc/sys_stat.h"
#include "libc/syscall.h"
#include "libc/string.h"

int main(void) {
    printf("=== Directory Syscalls Test ===\n\n");

    // Test 1: stat() on root directory
    printf("Test 1: stat(\"/\")\n");
    struct stat st;
    if (stat("/", &st) == 0) {
        printf("  Root directory inode: %lu\n", st.st_ino);
        printf("  Size: %lu bytes\n", st.st_size);
        printf("  Mode: 0%o\n", st.st_mode);
        printf("  Links: %u\n", st.st_nlink);
        printf("  SUCCESS\n");
    } else {
        printf("  FAILED to stat root\n");
    }
    printf("\n");

    // Test 2: opendir() and readdir() on root
    printf("Test 2: opendir(\"/\") and readdir()\n");
    DIR* dir = opendir("/");
    if (dir) {
        printf("  Directory opened successfully\n");
        printf("  Contents:\n");

        struct dirent* entry;
        int count = 0;
        while ((entry = readdir(dir)) != NULL) {
            char type_str[16];
            switch (entry->d_type) {
                case DT_DIR:  strcpy(type_str, "DIR"); break;
                case DT_REG:  strcpy(type_str, "FILE"); break;
                case DT_LNK:  strcpy(type_str, "LINK"); break;
                case DT_CHR:  strcpy(type_str, "CHR"); break;
                default:      strcpy(type_str, "UNKNOWN"); break;
            }

            printf("    [%s] %s (inode: %lu)\n",
                   type_str, entry->d_name, entry->d_ino);
            count++;
        }

        printf("  Total entries: %d\n", count);

        if (closedir(dir) == 0) {
            printf("  Directory closed successfully\n");
            printf("  SUCCESS\n");
        } else {
            printf("  FAILED to close directory\n");
        }
    } else {
        printf("  FAILED to open root directory\n");
    }
    printf("\n");

    // Test 3: Create a test file, stat it, rename it, unlink it
    printf("Test 3: File operations (create, stat, rename, unlink)\n");

    // Create a test file by writing to it
    int fd = open("/testfile.txt", O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) {
        const char* content = "Hello, directory syscalls!";
        write(fd, content, strlen(content));
        close(fd);
        printf("  Created /testfile.txt\n");

        // Stat the file
        if (stat("/testfile.txt", &st) == 0) {
            printf("  File inode: %lu\n", st.st_ino);
            printf("  File size: %lu bytes\n", st.st_size);
            printf("  File mode: 0%o\n", st.st_mode);
        } else {
            printf("  FAILED to stat /testfile.txt\n");
        }

        // Rename the file
        if (rename("/testfile.txt", "/renamed.txt") == 0) {
            printf("  Renamed /testfile.txt to /renamed.txt\n");

            // Verify the rename worked
            if (stat("/renamed.txt", &st) == 0) {
                printf("  Renamed file exists with inode: %lu\n", st.st_ino);
            }
        } else {
            printf("  FAILED to rename file\n");
        }

        // Delete the file
        if (unlink("/renamed.txt") == 0) {
            printf("  Deleted /renamed.txt\n");

            // Verify deletion
            if (stat("/renamed.txt", &st) < 0) {
                printf("  File successfully deleted (stat fails as expected)\n");
            }
        } else {
            printf("  FAILED to unlink file\n");
        }

        printf("  SUCCESS\n");
    } else {
        printf("  FAILED to create test file\n");
    }
    printf("\n");

    printf("=== All Directory Syscall Tests Complete ===\n");

    return 0;
}
