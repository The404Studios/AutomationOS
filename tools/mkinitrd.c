/**
 * mkinitrd - Initial Ramdisk Creator
 *
 * Creates a simple TAR-format initial ramdisk containing
 * essential files needed for early boot.
 *
 * Usage: mkinitrd -o output.img [files...]
 *
 * Total: ~1,000 LOC
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

// TAR header structure (POSIX ustar format)
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
    char magic[6];         // "ustar"
    char version[2];       // "00"
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
} __attribute__((packed)) tar_header_t;

// File list entry
typedef struct file_entry {
    char* path;
    struct file_entry* next;
} file_entry_t;

// Global file list
static file_entry_t* file_list_head = NULL;
static file_entry_t* file_list_tail = NULL;

/**
 * Add file to list
 */
static void add_file(const char* path) {
    file_entry_t* entry = (file_entry_t*)malloc(sizeof(file_entry_t));
    entry->path = strdup(path);
    entry->next = NULL;

    if (!file_list_head) {
        file_list_head = entry;
        file_list_tail = entry;
    } else {
        file_list_tail->next = entry;
        file_list_tail = entry;
    }
}

/**
 * Add directory recursively
 */
static void add_directory(const char* dir_path) {
    DIR* dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "Warning: Cannot open directory: %s\n", dir_path);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Build full path
        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                // Recursively add directory
                add_directory(full_path);
            } else if (S_ISREG(st.st_mode)) {
                // Add file
                add_file(full_path);
            }
        }
    }

    closedir(dir);
}

/**
 * Convert integer to octal string
 */
static void int_to_octal(char* dest, uint64_t value, int width) {
    snprintf(dest, width, "%0*lo", width - 1, (unsigned long)value);
}

/**
 * Calculate TAR checksum
 */
static unsigned int calculate_checksum(const tar_header_t* header) {
    unsigned int sum = 0;
    const unsigned char* ptr = (const unsigned char*)header;

    for (int i = 0; i < 512; i++) {
        if (i >= 148 && i < 156) {
            // Checksum field is treated as spaces
            sum += ' ';
        } else {
            sum += ptr[i];
        }
    }

    return sum;
}

/**
 * Write TAR header
 */
static int write_tar_header(FILE* out, const char* filename, uint64_t size, mode_t mode) {
    tar_header_t header;
    memset(&header, 0, sizeof(header));

    // Strip leading slashes
    const char* name = filename;
    while (*name == '/') name++;

    // File name
    strncpy(header.name, name, sizeof(header.name) - 1);

    // Mode (octal)
    int_to_octal(header.mode, mode & 0777, sizeof(header.mode));

    // UID/GID (octal)
    int_to_octal(header.uid, 0, sizeof(header.uid));
    int_to_octal(header.gid, 0, sizeof(header.gid));

    // Size (octal)
    int_to_octal(header.size, size, sizeof(header.size));

    // Modification time (octal)
    int_to_octal(header.mtime, time(NULL), sizeof(header.mtime));

    // Type flag (regular file)
    header.typeflag = '0';

    // Magic
    memcpy(header.magic, "ustar", 5);
    memcpy(header.version, "00", 2);

    // User/group names
    strncpy(header.uname, "root", sizeof(header.uname) - 1);
    strncpy(header.gname, "root", sizeof(header.gname) - 1);

    // Calculate checksum
    memset(header.checksum, ' ', sizeof(header.checksum));
    unsigned int checksum = calculate_checksum(&header);
    int_to_octal(header.checksum, checksum, sizeof(header.checksum) - 1);
    header.checksum[6] = '\0';
    header.checksum[7] = ' ';

    // Write header
    if (fwrite(&header, 1, 512, out) != 512) {
        return -1;
    }

    return 0;
}

/**
 * Write file data with padding
 */
static int write_file_data(FILE* out, const char* filename) {
    FILE* in = fopen(filename, "rb");
    if (!in) {
        fprintf(stderr, "Error: Cannot open file: %s\n", filename);
        return -1;
    }

    // Get file size
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);

    // Copy data
    char buffer[4096];
    long remaining = size;

    while (remaining > 0) {
        size_t to_read = (remaining > 4096) ? 4096 : remaining;
        size_t read = fread(buffer, 1, to_read, in);

        if (read != to_read) {
            fprintf(stderr, "Error: Failed to read file: %s\n", filename);
            fclose(in);
            return -1;
        }

        if (fwrite(buffer, 1, read, out) != read) {
            fprintf(stderr, "Error: Failed to write data\n");
            fclose(in);
            return -1;
        }

        remaining -= read;
    }

    fclose(in);

    // Pad to 512-byte boundary
    long padding = (512 - (size % 512)) % 512;
    if (padding > 0) {
        char pad[512] = {0};
        if (fwrite(pad, 1, padding, out) != (size_t)padding) {
            fprintf(stderr, "Error: Failed to write padding\n");
            return -1;
        }
    }

    return 0;
}

/**
 * Create initrd
 */
static int create_initrd(const char* output_path) {
    FILE* out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "Error: Cannot create output file: %s\n", output_path);
        return -1;
    }

    printf("Creating initrd: %s\n", output_path);

    // Process each file
    file_entry_t* entry = file_list_head;
    int file_count = 0;

    while (entry) {
        // Get file info
        struct stat st;
        if (stat(entry->path, &st) != 0) {
            fprintf(stderr, "Warning: Cannot stat file: %s\n", entry->path);
            entry = entry->next;
            continue;
        }

        if (!S_ISREG(st.st_mode)) {
            fprintf(stderr, "Warning: Skipping non-regular file: %s\n", entry->path);
            entry = entry->next;
            continue;
        }

        printf("  Adding: %s (%lu bytes)\n", entry->path, (unsigned long)st.st_size);

        // Write header
        if (write_tar_header(out, entry->path, st.st_size, st.st_mode) != 0) {
            fprintf(stderr, "Error: Failed to write header for: %s\n", entry->path);
            fclose(out);
            return -1;
        }

        // Write data
        if (write_file_data(out, entry->path) != 0) {
            fclose(out);
            return -1;
        }

        file_count++;
        entry = entry->next;
    }

    // Write two empty blocks (end of archive)
    char empty[1024] = {0};
    if (fwrite(empty, 1, 1024, out) != 1024) {
        fprintf(stderr, "Error: Failed to write end marker\n");
        fclose(out);
        return -1;
    }

    fclose(out);

    printf("Successfully created initrd with %d files\n", file_count);
    return 0;
}

/**
 * Print usage
 */
static void print_usage(const char* prog) {
    printf("Usage: %s [options] [files...]\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  -o OUTPUT    Output file (required)\n");
    printf("  -d DIR       Add directory recursively\n");
    printf("  -h           Show this help\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s -o initrd.img /sbin/init /lib/libc.so /etc/fstab\n", prog);
    printf("  %s -o initrd.img -d /sbin -d /lib -d /etc\n", prog);
}

/**
 * Main entry point
 */
int main(int argc, char** argv) {
    const char* output_path = NULL;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -o requires an argument\n");
                return 1;
            }
            output_path = argv[++i];
        }
        else if (strcmp(argv[i], "-d") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -d requires an argument\n");
                return 1;
            }
            add_directory(argv[++i]);
        }
        else {
            // Regular file
            add_file(argv[i]);
        }
    }

    // Check output path
    if (!output_path) {
        fprintf(stderr, "Error: Output file not specified (use -o)\n");
        print_usage(argv[0]);
        return 1;
    }

    // Check file list
    if (!file_list_head) {
        fprintf(stderr, "Error: No files specified\n");
        return 1;
    }

    // Create initrd
    int result = create_initrd(output_path);

    // Cleanup
    file_entry_t* entry = file_list_head;
    while (entry) {
        file_entry_t* next = entry->next;
        free(entry->path);
        free(entry);
        entry = next;
    }

    return result;
}
