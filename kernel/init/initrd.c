/**
 * Initial Ramdisk (initrd) Support
 * Parse and extract TAR-format initrd
 */

#include "../include/kernel.h"
#include "../include/mem.h"
#include "../include/initrd.h"
#include "../include/vfs.h"

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

// Initrd state
static uint64_t initrd_addr = 0;
static uint64_t initrd_size = 0;
static int initrd_mounted = 0;

/**
 * String copy utility
 */
static void vfs_strcpy(char* dst, const char* src, size_t max) {
    size_t i;
    for (i = 0; i < max - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/**
 * Convert octal string to integer
 */
static uint64_t octal_to_int(const char* str, int len) {
    uint64_t value = 0;
    for (int i = 0; i < len && str[i]; i++) {
        if (str[i] >= '0' && str[i] <= '7') {
            value = value * 8 + (str[i] - '0');
        }
    }
    return value;
}

/**
 * Validate TAR header
 */
static int validate_tar_header(const tar_header_t* header) {
    // Check magic
    if (header->magic[0] != 'u' ||
        header->magic[1] != 's' ||
        header->magic[2] != 't' ||
        header->magic[3] != 'a' ||
        header->magic[4] != 'r') {
        return 0;
    }

    // Check version
    if (header->version[0] != '0' || header->version[1] != '0') {
        return 0;
    }

    return 1;
}

/**
 * Check if TAR block is empty
 */
static int is_empty_block(const void* block) {
    const uint8_t* p = (const uint8_t*)block;
    for (int i = 0; i < 512; i++) {
        if (p[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/**
 * Initialize initrd subsystem
 *
 * INITRD-ALIAS-0: `addr` arrives as the initrd's PHYSICAL address (the boot
 * rescue copy at 16 MiB). The old code kept it verbatim and read the initrd
 * through the LOW IDENTITY map -- but user ELF images load at VA 0x800000
 * and a big image (browser2 grew past 15 MB of BSS) extends past VA 16 MiB,
 * so the loader's private zero-filled pages SHADOW the initrd identity
 * range in that process's page tables. Every kernel read of initrd-backed
 * data (vfs inode->data, initrd_get_file -> ELF loads) performed on that
 * process's CR3 then returns the process's own zeroes: exact byte counts,
 * all-zero content (the BROWSER2-IMG-0 zero-read bug).
 *
 * Fix: hold the DIRECT-MAP alias instead (PML4[256], supervisor-only,
 * shared BY REFERENCE into every CR3, never split, never user-shadowable).
 * Everything downstream -- tar parsing, zero-copy inode->data pointers,
 * initrd_get_file -- inherits the safe address. The PMM keeps reserving
 * the raw PHYSICAL range (kernel.c passes boot_info->initrd_addr there
 * unchanged), so frames are protected AND the mapping is unshadowable.
 * Single-core safe: pure VA selection at boot, no hardware touched.
 */
void initrd_init(uint64_t addr, uint64_t size) {
    if (addr && addr < DIRECT_MAP_SPAN) {
        initrd_addr = (uint64_t)PHYS_TO_DIRECT(addr);
    } else {
        initrd_addr = addr;   /* out of direct-map span: keep as-is (boots) */
    }
    initrd_size = size;
    initrd_mounted = 0;

    kprintf("[INITRD] Initrd phys 0x%016lx -> direct-map 0x%016lx, size %lu bytes\n",
            addr, initrd_addr, size);
}

/**
 * Mount initrd as root filesystem
 *
 * Extracts TAR archive and creates in-memory filesystem
 */
int initrd_mount(void) {
    if (!initrd_addr || !initrd_size) {
        kprintf("[INITRD] No initrd available\n");
        return -1;
    }

    if (initrd_mounted) {
        kprintf("[INITRD] Already mounted\n");
        return 0;
    }

    kprintf("[INITRD] Mounting initrd...\n");

    uint64_t offset = 0;
    int file_count = 0;
    int empty_blocks = 0;

    while (offset + 512 <= initrd_size) {   // the 512-byte header must fit
        const tar_header_t* header = (const tar_header_t*)(initrd_addr + offset);

        // Check for empty block (end of archive)
        if (is_empty_block(header)) {
            empty_blocks++;
            if (empty_blocks >= 2) {
                // End of archive
                break;
            }
            offset += 512;
            continue;
        }

        empty_blocks = 0;

        // Validate header
        if (!validate_tar_header(header)) {
            kprintf("[INITRD] Invalid TAR header at offset 0x%lx\n", offset);
            break;
        }

        // Determine full path (strip leading "./" if present)
        char fullpath[256];
        const char* raw_name = header->name;

        // Strip leading "./" or "." prefix from TAR filenames
        if (raw_name[0] == '.' && raw_name[1] == '/') {
            raw_name += 2;
        } else if (raw_name[0] == '.' && raw_name[1] == '\0') {
            // Skip lone "." entry
            uint64_t file_size_skip = octal_to_int(header->size, sizeof(header->size));
            uint64_t padding_skip = (512 - (file_size_skip % 512)) % 512;
            offset += 512 + file_size_skip + padding_skip;
            continue;
        }

        // Extract file information
        uint64_t file_size = octal_to_int(header->size, sizeof(header->size));
        char type = header->typeflag;

        kprintf("[INITRD] Extracting: %s (%lu bytes)\n", raw_name, file_size);

        // Build path with leading slash
        if (raw_name[0] == '/') {
            vfs_strcpy(fullpath, raw_name, 256);
        } else {
            fullpath[0] = '/';
            vfs_strcpy(fullpath + 1, raw_name, 255);
        }

        // Move to file data
        offset += 512;

        // Handle different file types
        if (type == '5') {
            // Directory - create recursively
            vfs_mkdir_recursive(fullpath, 0755);
        }
        else if (type == '0' || type == '\0') {
            // Regular file. Bounds: the file's data must lie fully within the
            // initrd image (offset already points past the 512-byte header).
            if (file_size > initrd_size || offset + file_size > initrd_size) {
                kprintf("[INITRD] '%s' (%lu B) extends past initrd end -- malformed, stopping\n",
                        raw_name, file_size);
                break;
            }
            const void* file_data = (const void*)(initrd_addr + offset);

            // Find parent directory and file name
            char parent_path[256];
            const char* file_name = fullpath;
            const char* last_slash = NULL;

            for (const char* p = fullpath; *p; p++) {
                if (*p == '/') last_slash = p;
            }

            if (last_slash && last_slash != fullpath) {
                // Extract parent path
                int parent_len = last_slash - fullpath;
                for (int i = 0; i < parent_len && i < 255; i++) {
                    parent_path[i] = fullpath[i];
                }
                parent_path[parent_len] = '\0';
                file_name = last_slash + 1;
            } else if (last_slash == fullpath) {
                // File in root
                parent_path[0] = '/';
                parent_path[1] = '\0';
                file_name = last_slash + 1;
            } else {
                // No slash, use root
                parent_path[0] = '/';
                parent_path[1] = '\0';
                file_name = fullpath;
            }

            // Ensure parent directory exists (create if missing)
            vfs_inode_t* parent = vfs_path_lookup(parent_path);
            if (!parent) {
                // Parent doesn't exist yet; create it recursively
                kprintf("[INITRD] Creating parent directory: %s\n", parent_path);
                vfs_mkdir_recursive(parent_path, 0755);
                parent = vfs_path_lookup(parent_path);
            }

            if (parent) {
                uint32_t mode = octal_to_int(header->mode, sizeof(header->mode));
                // Use zero-copy initrd-backed file creation
                vfs_ramfs_create_file_initrd(parent, file_name, file_data, file_size, mode);
                vfs_inode_put(parent);
                file_count++;
            } else {
                kprintf("[INITRD] WARNING: Parent directory not found for %s\n", fullpath);
            }
        }

        // Move to next header (512-byte aligned)
        uint64_t padding = (512 - (file_size % 512)) % 512;
        offset += file_size + padding;
    }

    initrd_mounted = 1;
    kprintf("[INITRD] Mounted successfully, %d files extracted\n", file_count);

    return 0;
}

/**
 * Get file from initrd
 *
 * @param path File path
 * @param size_out Output: file size
 * @return Pointer to file data, or NULL if not found
 */
void* initrd_get_file(const char* path, uint64_t* size_out) {
    if (!initrd_addr || !initrd_size) {
        return NULL;
    }

    uint64_t offset = 0;
    int empty_blocks = 0;

    while (offset + 512 <= initrd_size) {   // the 512-byte header must fit
        const tar_header_t* header = (const tar_header_t*)(initrd_addr + offset);

        // Check for empty block
        if (is_empty_block(header)) {
            empty_blocks++;
            if (empty_blocks >= 2) {
                break;
            }
            offset += 512;
            continue;
        }

        empty_blocks = 0;

        // Validate header
        if (!validate_tar_header(header)) {
            break;
        }

        // Check if this is the file we're looking for
        const char* filename = header->name;

        // Try exact match first
        int match = 1;
        for (int i = 0; path[i] || filename[i]; i++) {
            if (path[i] != filename[i]) {
                match = 0;
                break;
            }
        }

        // If no match and path starts with '/', try without leading slash
        if (!match && path[0] == '/') {
            match = 1;
            for (int i = 0; path[i+1] || filename[i]; i++) {
                if (path[i+1] != filename[i]) {
                    match = 0;
                    break;
                }
            }
        }

        // If no match and filename starts with '/', try without leading slash
        if (!match && filename[0] == '/') {
            match = 1;
            for (int i = 0; path[i] || filename[i+1]; i++) {
                if (path[i] != filename[i+1]) {
                    match = 0;
                    break;
                }
            }
        }

        // If no match and filename starts with './', try without './' prefix
        if (!match && filename[0] == '.' && filename[1] == '/') {
            match = 1;
            for (int i = 0; path[i] || filename[i+2]; i++) {
                if (path[i] != filename[i+2]) {
                    match = 0;
                    break;
                }
            }
        }

        if (match) {
            // Found it. Bounds: header + data must lie within the initrd image,
            // else the file is truncated/malformed -> do not hand back OOB memory.
            uint64_t file_size = octal_to_int(header->size, sizeof(header->size));
            if (offset + 512 + file_size > initrd_size) {
                kprintf("[INITRD] '%s' extends past initrd end -- refusing OOB read\n", path);
                return NULL;
            }
            void* file_data = (void*)(initrd_addr + offset + 512);

            if (size_out) {
                *size_out = file_size;
            }

            return file_data;
        }

        // Move to next entry
        uint64_t file_size = octal_to_int(header->size, sizeof(header->size));
        uint64_t padding = (512 - (file_size % 512)) % 512;
        offset += 512 + file_size + padding;
    }

    return NULL;  // Not found
}

/**
 * List files in initrd
 */
void initrd_list_files(void) {
    if (!initrd_addr || !initrd_size) {
        kprintf("[INITRD] No initrd available\n");
        return;
    }

    kprintf("[INITRD] File listing:\n");

    uint64_t offset = 0;
    int empty_blocks = 0;
    int file_count = 0;

    while (offset + 512 <= initrd_size) {   // the 512-byte header must fit
        const tar_header_t* header = (const tar_header_t*)(initrd_addr + offset);

        // Check for empty block
        if (is_empty_block(header)) {
            empty_blocks++;
            if (empty_blocks >= 2) {
                break;
            }
            offset += 512;
            continue;
        }

        empty_blocks = 0;

        // Validate header
        if (!validate_tar_header(header)) {
            break;
        }

        // Print file info
        uint64_t file_size = octal_to_int(header->size, sizeof(header->size));
        const char* filename = header->name;
        char type_char = header->typeflag;

        if (type_char == '0' || type_char == '\0') {
            type_char = 'f';  // Regular file
        } else if (type_char == '5') {
            type_char = 'd';  // Directory
        }

        kprintf("  %c %8lu  %s\n", type_char, file_size, filename);

        file_count++;

        // Move to next entry
        uint64_t padding = (512 - (file_size % 512)) % 512;
        offset += 512 + file_size + padding;
    }

    kprintf("[INITRD] Total: %d entries\n", file_count);
}

/**
 * Get initrd statistics
 */
void initrd_get_stats(uint64_t* total_files, uint64_t* total_size) {
    if (!initrd_addr || !initrd_size) {
        if (total_files) *total_files = 0;
        if (total_size) *total_size = 0;
        return;
    }

    uint64_t offset = 0;
    int empty_blocks = 0;
    int files = 0;
    uint64_t size = 0;

    while (offset + 512 <= initrd_size) {   // the 512-byte header must fit
        const tar_header_t* header = (const tar_header_t*)(initrd_addr + offset);

        if (is_empty_block(header)) {
            empty_blocks++;
            if (empty_blocks >= 2) break;
            offset += 512;
            continue;
        }

        empty_blocks = 0;

        if (!validate_tar_header(header)) break;

        uint64_t file_size = octal_to_int(header->size, sizeof(header->size));
        files++;
        size += file_size;

        uint64_t padding = (512 - (file_size % 512)) % 512;
        offset += 512 + file_size + padding;
    }

    if (total_files) *total_files = files;
    if (total_size) *total_size = size;
}
