/**
 * ext2 Filesystem Driver Implementation
 */

#include "../include/ext2.h"
#include "../include/fs_registry.h"
#include "../include/kernel.h"
#include "../include/mem.h"
#include "../include/string.h"
#include "../include/block.h"

// Forward declarations for VFS operations
static ssize_t ext2_vfs_read(vfs_file_t* file, void* buf, size_t count);
static ssize_t ext2_vfs_write(vfs_file_t* file, const void* buf, size_t count);
static int ext2_vfs_open(vfs_inode_t* inode, vfs_file_t* file);
static int ext2_vfs_close(vfs_file_t* file);
static off_t ext2_vfs_lseek(vfs_file_t* file, off_t offset, int whence);
static vfs_dentry_t* ext2_vfs_lookup(vfs_inode_t* dir, const char* name);

// VFS file operations for ext2
static vfs_file_ops_t ext2_file_ops = {
    .read = ext2_vfs_read,
    .write = ext2_vfs_write,
    .open = ext2_vfs_open,
    .close = ext2_vfs_close,
    .lseek = ext2_vfs_lseek,
};

// VFS inode operations for ext2
static vfs_inode_ops_t ext2_inode_ops = {
    .lookup = ext2_vfs_lookup,
    .create = NULL,  // Read-only for now
    .mkdir = NULL,
    .unlink = NULL,
    .rmdir = NULL,
};

/**
 * Read a block from the ext2 filesystem
 */
static int ext2_read_block(ext2_fs_data_t* fs_data, uint32_t block_num, void* buffer) {
    if (!fs_data || !buffer) {
        return -1;
    }

    block_device_t* dev = (block_device_t*)fs_data->block_device;
    if (!dev) {
        return -1;
    }

    // Calculate sector number (assuming 512-byte sectors)
    uint32_t sectors_per_block = fs_data->block_size / 512;
    uint64_t lba = block_num * sectors_per_block;

    return block_read(dev, lba, sectors_per_block, buffer) ? 0 : -1;
}

/**
 * Read an ext2 inode from disk
 */
static int ext2_read_inode(ext2_fs_data_t* fs_data, uint32_t inode_num, ext2_inode_t* inode) {
    if (!fs_data || !inode || inode_num == 0) {
        return -1;
    }

    ext2_superblock_t* sb = fs_data->superblock;

    // Untrusted on-disk superblock: s_inodes_per_group is the divisor below; a
    // corrupt/crafted image with 0 here would #DE (ring-0 divide-by-zero). Reject.
    if (sb->s_inodes_per_group == 0) {
        return -1;
    }

    // Calculate block group
    uint32_t group = (inode_num - 1) / sb->s_inodes_per_group;
    uint32_t index = (inode_num - 1) % sb->s_inodes_per_group;

    if (group >= fs_data->groups_count) {
        return -1;
    }

    // Get inode table block
    uint32_t inode_table_block = fs_data->group_desc[group].bg_inode_table;

    // Calculate inode position. inode_size is also untrusted on-disk: if it is 0
    // or exceeds the block size, inodes_per_block would be 0 and the divisions
    // below would #DE. Reject before dividing.
    uint32_t inode_size = sb->s_inode_size ? sb->s_inode_size : EXT2_GOOD_OLD_INODE_SIZE;
    if (inode_size == 0 || inode_size > fs_data->block_size) {
        return -1;
    }
    uint32_t inodes_per_block = fs_data->block_size / inode_size;
    if (inodes_per_block == 0) {
        return -1;
    }
    uint32_t block_offset = index / inodes_per_block;
    uint32_t inode_offset = index % inodes_per_block;
    // Ensure the inode fully fits inside the block buffer we memcpy from below.
    if ((uint64_t)inode_offset * inode_size + sizeof(ext2_inode_t) > fs_data->block_size) {
        return -1;
    }

    // Read block containing the inode
    void* block_buffer = kmalloc(fs_data->block_size);
    if (!block_buffer) {
        return -1;
    }

    if (ext2_read_block(fs_data, inode_table_block + block_offset, block_buffer) != 0) {
        kfree(block_buffer);
        return -1;
    }

    // Copy inode data
    memcpy(inode, (uint8_t*)block_buffer + (inode_offset * inode_size), sizeof(ext2_inode_t));

    kfree(block_buffer);
    return 0;
}

/**
 * Get the block number for a file at a given offset
 */
static uint32_t ext2_get_block_num(ext2_fs_data_t* fs_data, ext2_inode_t* inode, uint32_t block_index) {
    if (!fs_data || !inode) {
        return 0;
    }

    uint32_t ptrs_per_block = fs_data->block_size / sizeof(uint32_t);

    // Direct blocks (0-11)
    if (block_index < 12) {
        return inode->i_block[block_index];
    }
    block_index -= 12;

    // Single indirect block (12)
    if (block_index < ptrs_per_block) {
        if (inode->i_block[12] == 0) {
            return 0;
        }

        void* indirect_block = kmalloc(fs_data->block_size);
        if (!indirect_block) {
            return 0;
        }

        if (ext2_read_block(fs_data, inode->i_block[12], indirect_block) != 0) {
            kfree(indirect_block);
            return 0;
        }

        uint32_t block_num = ((uint32_t*)indirect_block)[block_index];
        kfree(indirect_block);
        return block_num;
    }
    block_index -= ptrs_per_block;

    // Double indirect block (13)
    if (block_index < ptrs_per_block * ptrs_per_block) {
        if (inode->i_block[13] == 0) {
            return 0;
        }

        void* indirect_block = kmalloc(fs_data->block_size);
        if (!indirect_block) {
            return 0;
        }

        if (ext2_read_block(fs_data, inode->i_block[13], indirect_block) != 0) {
            kfree(indirect_block);
            return 0;
        }

        uint32_t indirect_index = block_index / ptrs_per_block;
        uint32_t block_offset = block_index % ptrs_per_block;
        uint32_t indirect_num = ((uint32_t*)indirect_block)[indirect_index];
        kfree(indirect_block);

        if (indirect_num == 0) {
            return 0;
        }

        void* second_indirect = kmalloc(fs_data->block_size);
        if (!second_indirect) {
            return 0;
        }

        if (ext2_read_block(fs_data, indirect_num, second_indirect) != 0) {
            kfree(second_indirect);
            return 0;
        }

        uint32_t block_num = ((uint32_t*)second_indirect)[block_offset];
        kfree(second_indirect);
        return block_num;
    }
    block_index -= ptrs_per_block * ptrs_per_block;

    // Triple indirect block (14)
    if (block_index < ptrs_per_block * ptrs_per_block * ptrs_per_block) {
        if (inode->i_block[14] == 0) {
            return 0;
        }

        void* indirect_block = kmalloc(fs_data->block_size);
        if (!indirect_block) {
            return 0;
        }

        if (ext2_read_block(fs_data, inode->i_block[14], indirect_block) != 0) {
            kfree(indirect_block);
            return 0;
        }

        uint32_t first_index = block_index / (ptrs_per_block * ptrs_per_block);
        uint32_t remaining = block_index % (ptrs_per_block * ptrs_per_block);
        uint32_t second_index = remaining / ptrs_per_block;
        uint32_t third_index = remaining % ptrs_per_block;

        uint32_t first_num = ((uint32_t*)indirect_block)[first_index];
        kfree(indirect_block);

        if (first_num == 0) {
            return 0;
        }

        void* second_indirect = kmalloc(fs_data->block_size);
        if (!second_indirect) {
            return 0;
        }

        if (ext2_read_block(fs_data, first_num, second_indirect) != 0) {
            kfree(second_indirect);
            return 0;
        }

        uint32_t second_num = ((uint32_t*)second_indirect)[second_index];
        kfree(second_indirect);

        if (second_num == 0) {
            return 0;
        }

        void* third_indirect = kmalloc(fs_data->block_size);
        if (!third_indirect) {
            return 0;
        }

        if (ext2_read_block(fs_data, second_num, third_indirect) != 0) {
            kfree(third_indirect);
            return 0;
        }

        uint32_t block_num = ((uint32_t*)third_indirect)[third_index];
        kfree(third_indirect);
        return block_num;
    }

    // Block index out of range
    return 0;
}

/**
 * VFS read operation for ext2
 */
static ssize_t ext2_vfs_read(vfs_file_t* file, void* buf, size_t count) {
    if (!file || !file->inode || !buf || count == 0) {
        return -1;
    }

    vfs_inode_t* vfs_inode = file->inode;
    ext2_fs_data_t* fs_data = (ext2_fs_data_t*)vfs_inode->sb->private_data;
    ext2_inode_t* ext2_inode = (ext2_inode_t*)vfs_inode->private_data;

    if (!fs_data || !ext2_inode) {
        return -1;
    }

    // Don't read past end of file
    if (file->offset >= vfs_inode->size) {
        return 0;
    }

    if (file->offset + count > vfs_inode->size) {
        count = vfs_inode->size - file->offset;
    }

    size_t bytes_read = 0;
    uint8_t* dest = (uint8_t*)buf;

    while (bytes_read < count) {
        uint32_t block_index = (file->offset + bytes_read) / fs_data->block_size;
        uint32_t block_offset = (file->offset + bytes_read) % fs_data->block_size;
        uint32_t block_num = ext2_get_block_num(fs_data, ext2_inode, block_index);

        if (block_num == 0) {
            // Sparse block (hole in file) - return zeros
            size_t to_read = fs_data->block_size - block_offset;
            if (to_read > count - bytes_read) {
                to_read = count - bytes_read;
            }
            memset(dest + bytes_read, 0, to_read);
            bytes_read += to_read;
            continue;
        }

        // Read block
        void* block_buffer = kmalloc(fs_data->block_size);
        if (!block_buffer) {
            break;
        }

        if (ext2_read_block(fs_data, block_num, block_buffer) != 0) {
            kfree(block_buffer);
            break;
        }

        // Copy data from block
        size_t to_read = fs_data->block_size - block_offset;
        if (to_read > count - bytes_read) {
            to_read = count - bytes_read;
        }

        memcpy(dest + bytes_read, (uint8_t*)block_buffer + block_offset, to_read);
        kfree(block_buffer);

        bytes_read += to_read;
    }

    file->offset += bytes_read;
    return bytes_read;
}

/**
 * VFS write operation for ext2 (read-only for now)
 */
static ssize_t ext2_vfs_write(vfs_file_t* file, const void* buf, size_t count) {
    // ext2 write support not implemented yet (read-only)
    return -1;
}

/**
 * VFS open operation for ext2
 */
static int ext2_vfs_open(vfs_inode_t* inode, vfs_file_t* file) {
    // Nothing special needed for ext2 open
    return 0;
}

/**
 * VFS close operation for ext2
 */
static int ext2_vfs_close(vfs_file_t* file) {
    // Nothing special needed for ext2 close
    return 0;
}

/**
 * VFS lseek operation for ext2
 */
static off_t ext2_vfs_lseek(vfs_file_t* file, off_t offset, int whence) {
    switch (whence) {
        case SEEK_SET:
            file->offset = offset;
            break;
        case SEEK_CUR:
            file->offset += offset;
            break;
        case SEEK_END:
            file->offset = file->inode->size + offset;
            break;
        default:
            return -1;
    }
    return file->offset;
}

/**
 * Convert ext2 inode to VFS inode
 */
static vfs_inode_t* ext2_inode_to_vfs(vfs_superblock_t* sb, uint32_t inode_num, ext2_inode_t* ext2_inode) {
    vfs_inode_t* vfs_inode = vfs_inode_alloc(sb);
    if (!vfs_inode) {
        return NULL;
    }

    vfs_inode->ino = inode_num;
    vfs_inode->mode = ext2_inode->i_mode;
    vfs_inode->uid = ext2_inode->i_uid;
    vfs_inode->gid = ext2_inode->i_gid;
    vfs_inode->size = ext2_inode->i_size;
    vfs_inode->atime = ext2_inode->i_atime;
    vfs_inode->mtime = ext2_inode->i_mtime;
    vfs_inode->ctime = ext2_inode->i_ctime;
    vfs_inode->nlink = ext2_inode->i_links_count;
    vfs_inode->blocks = ext2_inode->i_blocks;

    // Determine file type
    if ((ext2_inode->i_mode & 0xF000) == EXT2_S_IFDIR) {
        vfs_inode->type = VFS_TYPE_DIR;
    } else if ((ext2_inode->i_mode & 0xF000) == EXT2_S_IFREG) {
        vfs_inode->type = VFS_TYPE_FILE;
    } else if ((ext2_inode->i_mode & 0xF000) == EXT2_S_IFLNK) {
        vfs_inode->type = VFS_TYPE_SYMLINK;
    }

    // Store ext2 inode as private data
    ext2_inode_t* private_inode = kmalloc(sizeof(ext2_inode_t));
    if (private_inode) {
        memcpy(private_inode, ext2_inode, sizeof(ext2_inode_t));
        vfs_inode->private_data = private_inode;
    }

    vfs_inode->ops = &ext2_inode_ops;

    return vfs_inode;
}

/**
 * VFS lookup operation for ext2
 */
static vfs_dentry_t* ext2_vfs_lookup(vfs_inode_t* dir, const char* name) {
    if (!dir || !name || !(dir->type & VFS_TYPE_DIR)) {
        return NULL;
    }

    ext2_fs_data_t* fs_data = (ext2_fs_data_t*)dir->sb->private_data;
    ext2_inode_t* dir_inode = (ext2_inode_t*)dir->private_data;

    if (!fs_data || !dir_inode) {
        return NULL;
    }

    // Read directory blocks
    uint32_t blocks_to_read = (dir->size + fs_data->block_size - 1) / fs_data->block_size;

    for (uint32_t i = 0; i < blocks_to_read && i < 12; i++) {
        uint32_t block_num = ext2_get_block_num(fs_data, dir_inode, i);
        if (block_num == 0) {
            continue;
        }

        void* block_buffer = kmalloc(fs_data->block_size);
        if (!block_buffer) {
            continue;
        }

        if (ext2_read_block(fs_data, block_num, block_buffer) != 0) {
            kfree(block_buffer);
            continue;
        }

        // Parse directory entries. The loop bound requires the fixed 8-byte entry
        // header (inode+rec_len+name_len+file_type) to be fully inside the block
        // before we dereference it — otherwise a record near the block tail would
        // read entry->rec_len/name_len past the kmalloc(block_size) buffer.
        uint32_t offset = 0;
        while (offset + 8u <= fs_data->block_size) {
            ext2_dir_entry_t* entry = (ext2_dir_entry_t*)((uint8_t*)block_buffer + offset);

            if (entry->rec_len == 0 || entry->inode == 0) {
                break;
            }

            // Untrusted on-disk rec_len/name_len: the record must cover its header
            // plus the name, and must not run past the block. Otherwise the memcmp
            // below (reading name_len bytes at entry->name) overruns the buffer.
            if (entry->rec_len < 8u + (uint32_t)entry->name_len ||
                offset + entry->rec_len > fs_data->block_size) {
                break;
            }

            // Compare name
            if (entry->name_len == strlen(name) &&
                memcmp(entry->name, name, entry->name_len) == 0) {

                // Found the entry - read inode
                ext2_inode_t ext2_inode;
                if (ext2_read_inode(fs_data, entry->inode, &ext2_inode) == 0) {
                    vfs_inode_t* vfs_inode = ext2_inode_to_vfs(dir->sb, entry->inode, &ext2_inode);
                    if (vfs_inode) {
                        vfs_dentry_t* dentry = vfs_dentry_alloc(name);
                        if (dentry) {
                            dentry->inode = vfs_inode;
                            kfree(block_buffer);
                            return dentry;
                        }
                        vfs_inode_free(vfs_inode);
                    }
                }
            }

            offset += entry->rec_len;
        }

        kfree(block_buffer);
    }

    return NULL;
}

/**
 * Detect if a block device contains ext2
 */
int ext2_detect(const char* source) {
    if (!source) {
        return -1;
    }

    // Get block device
    block_device_t* dev = block_get_device(source);
    if (!dev) {
        return -1;
    }

    // Allocate buffer for superblock (1024 bytes at offset 1024)
    void* sb_buffer = kmalloc(1024);
    if (!sb_buffer) {
        return -1;
    }

    // Read superblock (sectors 2-3, assuming 512-byte sectors)
    if (!block_read(dev, 2, 2, sb_buffer)) {
        kfree(sb_buffer);
        return -1;
    }

    ext2_superblock_t* sb = (ext2_superblock_t*)sb_buffer;

    // Check magic number
    if (sb->s_magic != EXT2_SUPER_MAGIC) {
        kfree(sb_buffer);
        return -1;
    }

    kfree(sb_buffer);
    return 0;
}

/**
 * Mount an ext2 filesystem
 */
vfs_superblock_t* ext2_mount(const char* source, uint32_t flags) {
    kprintf("[EXT2] Mounting %s\n", source);

    // Get block device
    block_device_t* dev = block_get_device(source);
    if (!dev) {
        kprintf("[EXT2] Block device not found: %s\n", source);
        return NULL;
    }

    // Allocate filesystem data structure
    ext2_fs_data_t* fs_data = kmalloc(sizeof(ext2_fs_data_t));
    if (!fs_data) {
        kprintf("[EXT2] Failed to allocate filesystem data\n");
        return NULL;
    }
    memset(fs_data, 0, sizeof(ext2_fs_data_t));
    fs_data->block_device = dev;

    // Read superblock (1024 bytes at offset 1024)
    fs_data->superblock = kmalloc(sizeof(ext2_superblock_t));
    if (!fs_data->superblock) {
        kfree(fs_data);
        return NULL;
    }

    // Read superblock (sectors 2-3, assuming 512-byte sectors)
    if (!block_read(dev, 2, 2, fs_data->superblock)) {
        kprintf("[EXT2] Failed to read superblock\n");
        kfree(fs_data->superblock);
        kfree(fs_data);
        return NULL;
    }

    // Validate magic number
    if (fs_data->superblock->s_magic != EXT2_SUPER_MAGIC) {
        kprintf("[EXT2] Invalid ext2 magic number: 0x%x\n", fs_data->superblock->s_magic);
        kfree(fs_data->superblock);
        kfree(fs_data);
        return NULL;
    }

    // Validate untrusted on-disk superblock numerics BEFORE using them as shift
    // amounts / divisors. A corrupt or crafted image must not be able to trigger a
    // ring-0 #DE (divide-by-zero) or a multi-GB kmalloc:
    //   - s_log_block_size > 2 makes (1024 << n) overflow toward 0 (div-by-zero in
    //     read_inode) or a huge block_size (huge per-block kmalloc). ext2 only
    //     defines block sizes 1024/2048/4096, i.e. n in {0,1,2}.
    //   - s_blocks_per_group == 0 is the divisor at groups_count below (#DE).
    //   - s_inodes_per_group == 0 is the divisor in read_inode (#DE).
    if (fs_data->superblock->s_log_block_size > 2 ||
        fs_data->superblock->s_blocks_per_group == 0 ||
        fs_data->superblock->s_inodes_per_group == 0) {
        kprintf("[EXT2] Rejecting image: invalid superblock geometry "
                "(log_block_size=%u blocks_per_group=%u inodes_per_group=%u)\n",
                fs_data->superblock->s_log_block_size,
                fs_data->superblock->s_blocks_per_group,
                fs_data->superblock->s_inodes_per_group);
        kfree(fs_data->superblock);
        kfree(fs_data);
        return NULL;
    }

    // Calculate block size
    fs_data->block_size = 1024 << fs_data->superblock->s_log_block_size;
    kprintf("[EXT2] Block size: %u bytes\n", fs_data->block_size);
    kprintf("[EXT2] Inodes: %u, Blocks: %u\n",
            fs_data->superblock->s_inodes_count,
            fs_data->superblock->s_blocks_count);

    // Calculate number of block groups
    fs_data->groups_count = (fs_data->superblock->s_blocks_count +
                             fs_data->superblock->s_blocks_per_group - 1) /
                            fs_data->superblock->s_blocks_per_group;

    kprintf("[EXT2] Block groups: %u\n", fs_data->groups_count);

    // Bound the derived group count BEFORE allocating. groups_count comes from
    // fully-untrusted superblock fields (s_blocks_count / s_blocks_per_group);
    // an out-of-range value would otherwise (a) overflow the 32-bit gdt_size
    // below -> a truncated allocation that ext2_read_inode's "group >=
    // groups_count" check then indexes far past (heap OOB read), and (b) demand
    // a multi-MB kmalloc that trips the heap's size assert (panic). Validate in
    // 64-bit and reject implausible images. 4 MiB of GDT = 131072 groups, far
    // more than any real device needs.
    {
        uint64_t gdt_size64 = (uint64_t)fs_data->groups_count *
                              sizeof(ext2_block_group_desc_t);
        if (fs_data->groups_count == 0 || gdt_size64 > (4u * 1024 * 1024)) {
            kprintf("[EXT2] Rejecting image: implausible group count %u\n",
                    fs_data->groups_count);
            kfree(fs_data->superblock);
            kfree(fs_data);
            return NULL;
        }
    }

    // Read block group descriptor table
    uint32_t gdt_size = fs_data->groups_count * sizeof(ext2_block_group_desc_t);
    fs_data->group_desc = kmalloc(gdt_size);
    if (!fs_data->group_desc) {
        kprintf("[EXT2] Failed to allocate group descriptor table\n");
        kfree(fs_data->superblock);
        kfree(fs_data);
        return NULL;
    }

    // GDT starts in block after superblock
    uint32_t gdt_block = (fs_data->block_size == 1024) ? 2 : 1;
    uint32_t gdt_blocks = (gdt_size + fs_data->block_size - 1) / fs_data->block_size;

    void* gdt_buffer = kmalloc(gdt_blocks * fs_data->block_size);
    if (!gdt_buffer) {
        kprintf("[EXT2] Failed to allocate GDT buffer\n");
        kfree(fs_data->group_desc);
        kfree(fs_data->superblock);
        kfree(fs_data);
        return NULL;
    }

    // Read GDT blocks
    for (uint32_t i = 0; i < gdt_blocks; i++) {
        uint32_t block_num = gdt_block + i;
        uint32_t sectors_per_block = fs_data->block_size / 512;
        uint64_t lba = block_num * sectors_per_block;

        if (!block_read(dev, lba, sectors_per_block,
                       (uint8_t*)gdt_buffer + (i * fs_data->block_size))) {
            kprintf("[EXT2] Failed to read block group descriptor table\n");
            kfree(gdt_buffer);
            kfree(fs_data->group_desc);
            kfree(fs_data->superblock);
            kfree(fs_data);
            return NULL;
        }
    }

    memcpy(fs_data->group_desc, gdt_buffer, gdt_size);
    kfree(gdt_buffer);

    // Create VFS superblock
    vfs_superblock_t* vfs_sb = kmalloc(sizeof(vfs_superblock_t));
    if (!vfs_sb) {
        kprintf("[EXT2] Failed to allocate VFS superblock\n");
        kfree(fs_data->group_desc);
        kfree(fs_data->superblock);
        kfree(fs_data);
        return NULL;
    }
    memset(vfs_sb, 0, sizeof(vfs_superblock_t));

    vfs_sb->magic = EXT2_SUPER_MAGIC;
    vfs_sb->blocksize = fs_data->block_size;
    vfs_sb->maxbytes = 0x7FFFFFFF;  // 2GB limit for now
    vfs_sb->type = "ext2";
    vfs_sb->private_data = fs_data;

    // Read root inode (inode 2)
    ext2_inode_t root_inode;
    if (ext2_read_inode(fs_data, EXT2_ROOT_INO, &root_inode) != 0) {
        kprintf("[EXT2] Failed to read root inode\n");
        kfree(vfs_sb);
        kfree(fs_data->group_desc);
        kfree(fs_data->superblock);
        kfree(fs_data);
        return NULL;
    }

    vfs_sb->root = ext2_inode_to_vfs(vfs_sb, EXT2_ROOT_INO, &root_inode);
    if (!vfs_sb->root) {
        kprintf("[EXT2] Failed to create root inode\n");
        kfree(vfs_sb);
        kfree(fs_data->group_desc);
        kfree(fs_data->superblock);
        kfree(fs_data);
        return NULL;
    }

    kprintf("[EXT2] Successfully mounted %s\n", source);
    return vfs_sb;
}

/**
 * Unmount an ext2 filesystem
 */
void ext2_unmount(vfs_superblock_t* sb) {
    if (!sb) {
        return;
    }

    // Free the root inode if present
    if (sb->root) {
        vfs_inode_put(sb->root);
        sb->root = NULL;
    }

    // Free ext2-specific data
    if (sb->private_data) {
        ext2_fs_data_t* fs_data = (ext2_fs_data_t*)sb->private_data;

        if (fs_data->superblock) {
            kfree(fs_data->superblock);
        }
        if (fs_data->group_desc) {
            kfree(fs_data->group_desc);
        }

        kfree(fs_data);
        sb->private_data = NULL;
    }

    kfree(sb);
}

/**
 * Filesystem type operations for ext2
 */
static fs_type_ops_t ext2_fs_ops = {
    .mount = ext2_mount,
    .unmount = ext2_unmount,
    .detect = ext2_detect,
};

/**
 * Initialize ext2 filesystem driver
 */
void ext2_init(void) {
    kprintf("[EXT2] Initializing ext2 filesystem driver\n");
    fs_register_type("ext2", &ext2_fs_ops);
    kprintf("[EXT2] ext2 filesystem driver registered\n");
}
