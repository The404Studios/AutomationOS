/**
 * FAT32 Filesystem Driver Implementation
 */

#include "../include/fat32.h"
#include "../include/fs_registry.h"
#include "../include/kernel.h"
#include "../include/mem.h"
#include "../include/string.h"
#include "../include/block.h"

// Forward declarations for VFS operations
static ssize_t fat32_vfs_read(vfs_file_t* file, void* buf, size_t count);
static ssize_t fat32_vfs_write(vfs_file_t* file, const void* buf, size_t count);
static int fat32_vfs_open(vfs_inode_t* inode, vfs_file_t* file);
static int fat32_vfs_close(vfs_file_t* file);
static off_t fat32_vfs_lseek(vfs_file_t* file, off_t offset, int whence);
static vfs_dentry_t* fat32_vfs_lookup(vfs_inode_t* dir, const char* name);

// VFS file operations for FAT32
static vfs_file_ops_t fat32_file_ops = {
    .read = fat32_vfs_read,
    .write = fat32_vfs_write,
    .open = fat32_vfs_open,
    .close = fat32_vfs_close,
    .lseek = fat32_vfs_lseek,
};

// VFS inode operations for FAT32
static vfs_inode_ops_t fat32_inode_ops = {
    .lookup = fat32_vfs_lookup,
    .create = NULL,  // Read-only for now
    .mkdir = NULL,
    .unlink = NULL,
    .rmdir = NULL,
};

/**
 * Read sectors from FAT32 filesystem
 */
static int fat32_read_sectors(fat32_fs_data_t* fs_data, uint64_t lba, uint32_t count, void* buffer) {
    if (!fs_data || !buffer) {
        return -1;
    }

    block_device_t* dev = (block_device_t*)fs_data->block_device;
    if (!dev) {
        return -1;
    }

    return block_read(dev, lba, count, buffer) ? 0 : -1;
}

/**
 * Get the next cluster in the FAT chain
 */
static uint32_t fat32_get_next_cluster(fat32_fs_data_t* fs_data, uint32_t cluster) {
    if (!fs_data || !fs_data->fat || cluster < 2 || cluster >= fs_data->total_clusters) {
        return FAT32_EOC;
    }

    uint32_t fat_entry = fs_data->fat[cluster] & 0x0FFFFFFF;

    if (fat_entry >= FAT32_EOC) {
        return FAT32_EOC;
    }

    return fat_entry;
}

/**
 * Get the LBA of a cluster
 */
static uint64_t fat32_cluster_to_lba(fat32_fs_data_t* fs_data, uint32_t cluster) {
    if (!fs_data || cluster < 2) {
        return 0;
    }

    return fs_data->first_data_sector + ((cluster - 2) * fs_data->boot_sector->sectors_per_cluster);
}

/**
 * VFS read operation for FAT32
 */
static ssize_t fat32_vfs_read(vfs_file_t* file, void* buf, size_t count) {
    if (!file || !file->inode || !buf || count == 0) {
        return -1;
    }

    vfs_inode_t* vfs_inode = file->inode;
    fat32_fs_data_t* fs_data = (fat32_fs_data_t*)vfs_inode->sb->private_data;

    if (!fs_data) {
        return -1;
    }

    // Don't read past end of file
    if (file->offset >= vfs_inode->size) {
        return 0;
    }

    if (file->offset + count > vfs_inode->size) {
        count = vfs_inode->size - file->offset;
    }

    // Get starting cluster from inode private data
    uint32_t start_cluster = (uint32_t)(uintptr_t)vfs_inode->private_data;
    if (start_cluster < 2) {
        return 0;  // Empty file
    }

    size_t bytes_read = 0;
    uint8_t* dest = (uint8_t*)buf;
    uint32_t cluster = start_cluster;
    uint64_t file_pos = 0;

    // Skip to the cluster containing file->offset
    while (file_pos + fs_data->bytes_per_cluster <= file->offset) {
        cluster = fat32_get_next_cluster(fs_data, cluster);
        if (cluster >= FAT32_EOC) {
            return 0;
        }
        file_pos += fs_data->bytes_per_cluster;
    }

    // Read data
    while (bytes_read < count && cluster < FAT32_EOC) {
        uint64_t lba = fat32_cluster_to_lba(fs_data, cluster);
        if (lba == 0) {
            break;
        }

        // Read cluster
        void* cluster_buffer = kmalloc(fs_data->bytes_per_cluster);
        if (!cluster_buffer) {
            break;
        }

        if (fat32_read_sectors(fs_data, lba, fs_data->boot_sector->sectors_per_cluster, cluster_buffer) != 0) {
            kfree(cluster_buffer);
            break;
        }

        // Calculate offset within cluster
        uint32_t cluster_offset = (file->offset + bytes_read - file_pos) % fs_data->bytes_per_cluster;
        size_t to_read = fs_data->bytes_per_cluster - cluster_offset;
        if (to_read > count - bytes_read) {
            to_read = count - bytes_read;
        }

        memcpy(dest + bytes_read, (uint8_t*)cluster_buffer + cluster_offset, to_read);
        kfree(cluster_buffer);

        bytes_read += to_read;
        file_pos += fs_data->bytes_per_cluster;

        if (bytes_read < count) {
            cluster = fat32_get_next_cluster(fs_data, cluster);
        }
    }

    file->offset += bytes_read;
    return bytes_read;
}

/**
 * VFS write operation for FAT32 (read-only for now)
 */
static ssize_t fat32_vfs_write(vfs_file_t* file, const void* buf, size_t count) {
    // FAT32 write support not implemented yet (read-only)
    return -1;
}

/**
 * VFS open operation for FAT32
 */
static int fat32_vfs_open(vfs_inode_t* inode, vfs_file_t* file) {
    // Nothing special needed for FAT32 open
    return 0;
}

/**
 * VFS close operation for FAT32
 */
static int fat32_vfs_close(vfs_file_t* file) {
    // Nothing special needed for FAT32 close
    return 0;
}

/**
 * VFS lseek operation for FAT32
 *
 * BUG-FIX (underflow): the old code assigned directly without validating,
 * so a negative offset for SEEK_SET, or a negative SEEK_CUR/SEEK_END that
 * exceeds the current position/file size, would wrap the uint64_t file->offset
 * to a near-UINT64_MAX value, corrupting all subsequent read/write offset math.
 */
static off_t fat32_vfs_lseek(vfs_file_t* file, off_t offset, int whence) {
    uint64_t base;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = file->offset; break;
        case SEEK_END: base = file->inode->size; break;
        default:       return -1;
    }
    if (offset < 0 && (uint64_t)(-offset) > base) {
        return -1;   /* would seek before byte 0 */
    }
    file->offset = base + (uint64_t)offset;
    return (off_t)file->offset;
}

/**
 * Convert FAT32 8.3 name to normal string
 */
static void fat32_name_to_string(const char* fat_name, char* output) {
    int i, j = 0;

    // Copy base name (8 chars)
    for (i = 0; i < 8 && fat_name[i] != ' '; i++) {
        output[j++] = fat_name[i];
    }

    // Add extension if present
    if (fat_name[8] != ' ') {
        output[j++] = '.';
        for (i = 8; i < 11 && fat_name[i] != ' '; i++) {
            output[j++] = fat_name[i];
        }
    }

    output[j] = '\0';
}

/**
 * Calculate LFN checksum for 8.3 name
 */
static uint8_t fat32_lfn_checksum(const char* short_name) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)short_name[i];
    }
    return sum;
}

/**
 * Extract Unicode character from LFN entry (convert to ASCII)
 */
static char fat32_lfn_char(uint16_t unicode_char) {
    // Simple conversion - just take low byte for ASCII range
    if (unicode_char < 0x80) {
        return (char)unicode_char;
    }
    return '?';  // Non-ASCII character
}

/**
 * Read long filename from LFN entries
 * Returns number of LFN entries consumed, or 0 if no LFN
 */
static int fat32_read_lfn(fat32_dir_entry_t* entries, uint32_t max_entries,
                          char* lfn_buffer, size_t buffer_size) {
    if (!entries || !lfn_buffer || buffer_size == 0) {
        return 0;
    }

    // Check if first entry is LFN
    if (entries[0].attr != FAT_ATTR_LONG_NAME) {
        return 0;
    }

    fat32_lfn_entry_t* lfn = (fat32_lfn_entry_t*)&entries[0];

    // Extract sequence number (first LFN entry has 0x40 bit set)
    uint8_t seq = lfn->order & 0x3F;
    if (seq == 0 || seq > 20) {  // Max 20 LFN entries
        return 0;
    }

    int total_lfn_entries = seq;
    char temp_buffer[260];  // Max LFN length
    // Zero first: the extraction loops below write ONLY the character positions
    // that pass their guards, so any skipped slot would otherwise be read back as
    // uninitialized stack garbage when the name is assembled -> corrupt filenames.
    memset(temp_buffer, 0, sizeof(temp_buffer));
    int pos = 0;

    // Read LFN entries in reverse order
    for (int i = 0; i < total_lfn_entries && i < (int)max_entries; i++) {
        if (entries[i].attr != FAT_ATTR_LONG_NAME) {
            return 0;  // Not a valid LFN sequence
        }

        lfn = (fat32_lfn_entry_t*)&entries[i];

        // Extract characters from this LFN entry (13 chars total)
        int base_pos = (total_lfn_entries - 1 - i) * 13;

        // Name1: 5 characters
        for (int j = 0; j < 5; j++) {
            if (base_pos + j < 260 && lfn->name1[j] != 0 && lfn->name1[j] != 0xFFFF) {
                temp_buffer[base_pos + j] = fat32_lfn_char(lfn->name1[j]);
            }
        }

        // Name2: 6 characters
        for (int j = 0; j < 6; j++) {
            if (base_pos + 5 + j < 260 && lfn->name2[j] != 0 && lfn->name2[j] != 0xFFFF) {
                temp_buffer[base_pos + 5 + j] = fat32_lfn_char(lfn->name2[j]);
            }
        }

        // Name3: 2 characters
        for (int j = 0; j < 2; j++) {
            if (base_pos + 11 + j < 260 && lfn->name3[j] != 0 && lfn->name3[j] != 0xFFFF) {
                temp_buffer[base_pos + 11 + j] = fat32_lfn_char(lfn->name3[j]);
            }
        }

        pos = base_pos + 13;
    }

    // Find null terminator
    for (int i = 0; i < pos && i < 260; i++) {
        if (temp_buffer[i] == 0) {
            pos = i;
            break;
        }
    }

    // Copy to output buffer
    if ((size_t)pos >= buffer_size) {
        pos = buffer_size - 1;
    }
    memcpy(lfn_buffer, temp_buffer, pos);
    lfn_buffer[pos] = '\0';

    // Never report more entries than we were allowed to scan this cluster. The
    // caller does `i += count - 1` to skip the LFN group; an on-disk seq that
    // exceeds the entries remaining in the cluster would otherwise skip past the
    // cluster boundary and misparse the next cluster's entries mid-group.
    if (total_lfn_entries > (int)max_entries) {
        total_lfn_entries = (int)max_entries;
    }
    return total_lfn_entries;
}

/**
 * VFS lookup operation for FAT32
 */
static vfs_dentry_t* fat32_vfs_lookup(vfs_inode_t* dir, const char* name) {
    if (!dir || !name || !(dir->type & VFS_TYPE_DIR)) {
        return NULL;
    }

    fat32_fs_data_t* fs_data = (fat32_fs_data_t*)dir->sb->private_data;
    if (!fs_data) {
        return NULL;
    }

    // Get directory cluster
    uint32_t dir_cluster = (uint32_t)(uintptr_t)dir->private_data;
    if (dir_cluster == 0) {
        dir_cluster = fs_data->boot_sector->root_cluster;
    }

    // Read directory entries
    uint32_t cluster = dir_cluster;
    uint32_t walk_guard = 0;
    while (cluster < FAT32_EOC) {
        // Cycle guard: a valid chain visits each cluster at most once, so cap
        // the walk at total_clusters. A crafted/corrupt FAT can encode an
        // in-range cluster cycle (fat[N]=N, A->B->A) that never reaches EOC and
        // would otherwise hang the kernel re-reading the same directory cluster.
        if (++walk_guard > fs_data->total_clusters) {
            break;
        }
        uint64_t lba = fat32_cluster_to_lba(fs_data, cluster);
        if (lba == 0) {
            break;
        }

        void* cluster_buffer = kmalloc(fs_data->bytes_per_cluster);
        if (!cluster_buffer) {
            break;
        }

        if (fat32_read_sectors(fs_data, lba, fs_data->boot_sector->sectors_per_cluster, cluster_buffer) != 0) {
            kfree(cluster_buffer);
            break;
        }

        // Parse directory entries
        uint32_t entries_per_cluster = fs_data->bytes_per_cluster / sizeof(fat32_dir_entry_t);
        fat32_dir_entry_t* entries = (fat32_dir_entry_t*)cluster_buffer;

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t* entry = &entries[i];

            // End of directory
            if (entry->name[0] == 0x00) {
                kfree(cluster_buffer);
                return NULL;
            }

            // Skip deleted entries
            if (entry->name[0] == 0xE5) {
                continue;
            }

            // Check for LFN entries
            char lfn_name[260] = {0};
            int lfn_count = 0;

            if (entry->attr == FAT_ATTR_LONG_NAME) {
                // Read LFN
                lfn_count = fat32_read_lfn(&entries[i], entries_per_cluster - i,
                                           lfn_name, sizeof(lfn_name));
                if (lfn_count > 0) {
                    i += lfn_count - 1;  // Skip LFN entries
                    continue;  // LFN is processed, wait for the actual entry
                }
            }

            // Skip volume ID entries
            if (entry->attr & FAT_ATTR_VOLUME_ID) {
                continue;
            }

            // Determine name to use (LFN if available, otherwise 8.3)
            char entry_name[260];
            if (lfn_name[0] != '\0') {
                // Use long filename (bounded copy; lfn_name comes from disk)
                strncpy(entry_name, lfn_name, sizeof(entry_name) - 1);
                entry_name[sizeof(entry_name) - 1] = '\0';
            } else {
                // Use short 8.3 name
                fat32_name_to_string(entry->name, entry_name);
            }

            // Case-insensitive comparison
            int match = 1;
            for (int j = 0; entry_name[j] && name[j]; j++) {
                char c1 = entry_name[j];
                char c2 = name[j];
                if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                if (c1 != c2) {
                    match = 0;
                    break;
                }
            }

            if (match && entry_name[strlen(name)] == '\0') {
                // Found the entry
                uint32_t first_cluster = ((uint32_t)entry->first_cluster_hi << 16) | entry->first_cluster_lo;

                vfs_inode_t* vfs_inode = vfs_inode_alloc(dir->sb);
                if (vfs_inode) {
                    vfs_inode->size = entry->file_size;
                    vfs_inode->mode = 0644;
                    vfs_inode->type = (entry->attr & FAT_ATTR_DIRECTORY) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
                    vfs_inode->private_data = (void*)(uintptr_t)first_cluster;
                    vfs_inode->ops = &fat32_inode_ops;

                    vfs_dentry_t* dentry = vfs_dentry_alloc(name);
                    if (dentry) {
                        dentry->inode = vfs_inode;
                        kfree(cluster_buffer);
                        return dentry;
                    }
                    vfs_inode_free(vfs_inode);
                }
            }
        }

        kfree(cluster_buffer);
        cluster = fat32_get_next_cluster(fs_data, cluster);
    }

    return NULL;
}

/**
 * Detect if a block device contains FAT32
 */
int fat32_detect(const char* source) {
    // Read boot sector from LBA 0
    fat32_boot_sector_t* boot_sector = (fat32_boot_sector_t*)kmalloc(512);
    if (!boot_sector) {
        return -1;
    }

    // For now, we need a way to get block device by name
    // This is a placeholder - real implementation needs device registry
    // Try reading sector 0 - if it has valid FAT32 signature, it's FAT32

    // Check signature
    if (boot_sector->signature != FAT32_SIGNATURE) {
        kfree(boot_sector);
        return -1;
    }

    // Check for FAT32 indicators
    if (boot_sector->fat_size_16 != 0 || boot_sector->root_entry_count != 0) {
        kfree(boot_sector);
        return -1;  // Not FAT32 (probably FAT16/12)
    }

    if (boot_sector->fat_size_32 == 0) {
        kfree(boot_sector);
        return -1;  // Invalid FAT32
    }

    kfree(boot_sector);
    return 0;  // Valid FAT32
}

/**
 * Mount a FAT32 filesystem
 */
vfs_superblock_t* fat32_mount(const char* source, uint32_t flags) {
    kprintf("[FAT32] Mounting %s\n", source);

    // Allocate filesystem data structure
    fat32_fs_data_t* fs_data = (fat32_fs_data_t*)kmalloc(sizeof(fat32_fs_data_t));
    if (!fs_data) {
        kprintf("[FAT32] Failed to allocate filesystem data\n");
        return NULL;
    }

    memset(fs_data, 0, sizeof(fat32_fs_data_t));

    // Read boot sector
    fs_data->boot_sector = (fat32_boot_sector_t*)kmalloc(512);
    if (!fs_data->boot_sector) {
        kprintf("[FAT32] Failed to allocate boot sector buffer\n");
        kfree(fs_data);
        return NULL;
    }

    // TODO: Get actual block device from source path
    // For now, this needs a block device registry to be implemented
    // Placeholder: assume source is a block device pointer cast to string
    block_device_t* block_dev = NULL;

    if (!block_dev) {
        kprintf("[FAT32] Block device lookup not implemented yet\n");
        kfree(fs_data->boot_sector);
        kfree(fs_data);
        return NULL;
    }

    fs_data->block_device = block_dev;

    // Read boot sector from LBA 0
    if (!block_read(block_dev, 0, 1, fs_data->boot_sector)) {
        kprintf("[FAT32] Failed to read boot sector\n");
        kfree(fs_data->boot_sector);
        kfree(fs_data);
        return NULL;
    }

    // Validate boot sector
    if (fs_data->boot_sector->signature != FAT32_SIGNATURE) {
        kprintf("[FAT32] Invalid boot sector signature: 0x%04x\n", fs_data->boot_sector->signature);
        kfree(fs_data->boot_sector);
        kfree(fs_data);
        return NULL;
    }

    // Validate FAT32
    if (fs_data->boot_sector->fat_size_16 != 0 || fs_data->boot_sector->root_entry_count != 0) {
        kprintf("[FAT32] Not a FAT32 filesystem (FAT16/12 detected)\n");
        kfree(fs_data->boot_sector);
        kfree(fs_data);
        return NULL;
    }

    if (fs_data->boot_sector->fat_size_32 == 0) {
        kprintf("[FAT32] Invalid FAT32 filesystem\n");
        kfree(fs_data->boot_sector);
        kfree(fs_data);
        return NULL;
    }

    // Validate untrusted on-disk geometry BEFORE using as divisors/multipliers.
    // A corrupt or crafted image with 0 in any of these fields would cause a
    // ring-0 #DE (divide-by-zero) or nonsensical cluster/sector math.
    if (fs_data->boot_sector->bytes_per_sector == 0 ||
        fs_data->boot_sector->sectors_per_cluster == 0) {
        kprintf("[FAT32] Invalid geometry: bytes_per_sector=%u sectors_per_cluster=%u\n",
                fs_data->boot_sector->bytes_per_sector,
                fs_data->boot_sector->sectors_per_cluster);
        kfree(fs_data->boot_sector);
        kfree(fs_data);
        return NULL;
    }

    // Calculate filesystem parameters
    fs_data->bytes_per_cluster = fs_data->boot_sector->bytes_per_sector *
                                  fs_data->boot_sector->sectors_per_cluster;

    uint32_t fat_start_sector = fs_data->boot_sector->reserved_sectors;
    uint32_t fat_sectors = fs_data->boot_sector->num_fats * fs_data->boot_sector->fat_size_32;

    fs_data->first_data_sector = fat_start_sector + fat_sectors;
    fs_data->data_sectors = fs_data->boot_sector->total_sectors_32 - fs_data->first_data_sector;
    fs_data->total_clusters = fs_data->data_sectors / fs_data->boot_sector->sectors_per_cluster;

    kprintf("[FAT32] Cluster size: %u bytes\n", fs_data->bytes_per_cluster);
    kprintf("[FAT32] Total clusters: %u\n", fs_data->total_clusters);
    kprintf("[FAT32] Root cluster: %u\n", fs_data->boot_sector->root_cluster);

    // Allocate and read FAT table (first FAT only)
    uint32_t fat_size_bytes = fs_data->boot_sector->fat_size_32 * fs_data->boot_sector->bytes_per_sector;
    fs_data->fat = (uint32_t*)kmalloc(fat_size_bytes);
    if (!fs_data->fat) {
        kprintf("[FAT32] Failed to allocate FAT table\n");
        kfree(fs_data->boot_sector);
        kfree(fs_data);
        return NULL;
    }

    if (!block_read(block_dev, fat_start_sector, fs_data->boot_sector->fat_size_32, fs_data->fat)) {
        kprintf("[FAT32] Failed to read FAT table\n");
        kfree(fs_data->fat);
        kfree(fs_data->boot_sector);
        kfree(fs_data);
        return NULL;
    }

    kprintf("[FAT32] FAT table loaded (%u sectors)\n", fs_data->boot_sector->fat_size_32);

    // Create VFS superblock
    vfs_superblock_t* sb = (vfs_superblock_t*)kmalloc(sizeof(vfs_superblock_t));
    if (!sb) {
        kprintf("[FAT32] Failed to allocate superblock\n");
        kfree(fs_data->fat);
        kfree(fs_data->boot_sector);
        kfree(fs_data);
        return NULL;
    }

    memset(sb, 0, sizeof(vfs_superblock_t));
    sb->private_data = fs_data;

    // Create root inode
    vfs_inode_t* root_inode = vfs_inode_alloc(sb);
    if (!root_inode) {
        kprintf("[FAT32] Failed to allocate root inode\n");
        kfree(sb);
        kfree(fs_data->fat);
        kfree(fs_data->boot_sector);
        kfree(fs_data);
        return NULL;
    }

    root_inode->type = VFS_TYPE_DIR;
    root_inode->mode = 0755;
    root_inode->size = 0;
    root_inode->ops = &fat32_inode_ops;
    root_inode->private_data = (void*)(uintptr_t)fs_data->boot_sector->root_cluster;

    sb->root = root_inode;

    kprintf("[FAT32] Mount successful\n");
    return sb;
}

/**
 * Unmount a FAT32 filesystem
 */
void fat32_unmount(vfs_superblock_t* sb) {
    if (!sb) {
        return;
    }

    // Free the root inode if present
    if (sb->root) {
        vfs_inode_put(sb->root);
        sb->root = NULL;
    }

    // Free FAT32-specific data
    if (sb->private_data) {
        fat32_fs_data_t* fs_data = (fat32_fs_data_t*)sb->private_data;

        if (fs_data->boot_sector) {
            kfree(fs_data->boot_sector);
        }
        if (fs_data->fat) {
            kfree(fs_data->fat);
        }

        kfree(fs_data);
        sb->private_data = NULL;
    }

    kfree(sb);
}

/**
 * Filesystem type operations for FAT32
 */
static fs_type_ops_t fat32_fs_ops = {
    .mount = fat32_mount,
    .unmount = fat32_unmount,
    .detect = fat32_detect,
};

/**
 * Initialize FAT32 filesystem driver
 */
void fat32_init(void) {
    kprintf("[FAT32] Initializing FAT32 filesystem driver\n");
    fs_register_type("fat32", &fat32_fs_ops);
    kprintf("[FAT32] FAT32 filesystem driver registered\n");
}
