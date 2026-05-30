/**
 * @file autofs.c
 * @brief AutoFS Core Filesystem Implementation
 *
 * Implements the main filesystem operations including mount, unmount,
 * file I/O, and directory management.
 */

#include "../../include/autofs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

/* Internal helper functions */
static int autofs_read_superblock(autofs_fs_t *fs);
static int autofs_write_superblock(autofs_fs_t *fs);
static int autofs_load_bitmaps(autofs_fs_t *fs);
static int autofs_save_bitmaps(autofs_fs_t *fs);

/**
 * @brief Mount an AutoFS filesystem
 *
 * @param device Device path (e.g., "/dev/sda1")
 * @param read_only Mount as read-only
 * @return Mounted filesystem structure or NULL on error
 */
autofs_fs_t* autofs_mount(const char *device, bool read_only) {
    if (!device) {
        errno = EINVAL;
        return NULL;
    }

    autofs_fs_t *fs = calloc(1, sizeof(autofs_fs_t));
    if (!fs) {
        return NULL;
    }

    /* Open device */
    int flags = read_only ? O_RDONLY : O_RDWR;
    fs->fd = open(device, flags);
    if (fs->fd < 0) {
        free(fs);
        return NULL;
    }

    fs->read_only = read_only;
    fs->device_path = strdup(device);

    /* Allocate superblock */
    fs->sb = calloc(1, sizeof(autofs_superblock_t));
    if (!fs->sb) {
        close(fs->fd);
        free(fs->device_path);
        free(fs);
        return NULL;
    }

    /* Read superblock */
    if (autofs_read_superblock(fs) < 0) {
        fprintf(stderr, "Failed to read superblock\n");
        goto error;
    }

    /* Verify magic number */
    if (fs->sb->magic != AUTOFS_MAGIC) {
        fprintf(stderr, "Invalid AutoFS magic: 0x%lx (expected 0x%x)\n",
                fs->sb->magic, AUTOFS_MAGIC);
        errno = EINVAL;
        goto error;
    }

    /* Verify version */
    if (fs->sb->version != AUTOFS_VERSION) {
        fprintf(stderr, "Unsupported AutoFS version: %lu\n", fs->sb->version);
        errno = EINVAL;
        goto error;
    }

    /* Load bitmaps */
    if (autofs_load_bitmaps(fs) < 0) {
        fprintf(stderr, "Failed to load allocation bitmaps\n");
        goto error;
    }

    /* Initialize journal */
    if (fs->sb->features & AUTOFS_FEATURE_JOURNAL) {
        if (autofs_journal_init(fs) < 0) {
            fprintf(stderr, "Failed to initialize journal\n");
            goto error;
        }

        /* Recover from journal if needed */
        if (autofs_journal_recover(fs) < 0) {
            fprintf(stderr, "Failed to recover from journal\n");
            goto error;
        }
    }

    /* Initialize caches */
    if (autofs_cache_init(fs) < 0) {
        fprintf(stderr, "Failed to initialize caches\n");
        goto error;
    }

    /* Update mount info */
    if (!read_only) {
        fs->sb->mount_time = time(NULL);
        fs->sb->mount_count++;
        autofs_write_superblock(fs);
    }

    printf("AutoFS: Mounted %s (%s)\n", device,
           read_only ? "read-only" : "read-write");
    printf("  Version: %lu\n", fs->sb->version);
    printf("  Block size: %lu bytes\n", fs->sb->block_size);
    printf("  Blocks: %lu total, %lu free\n",
           fs->sb->block_count, fs->sb->free_blocks);
    printf("  Inodes: %lu total, %lu free\n",
           fs->sb->inode_count, fs->sb->free_inodes);
    printf("  Features: %s%s%s%s%s%s\n",
           (fs->sb->features & AUTOFS_FEATURE_JOURNAL) ? "journal " : "",
           (fs->sb->features & AUTOFS_FEATURE_COW) ? "cow " : "",
           (fs->sb->features & AUTOFS_FEATURE_COMPRESSION) ? "compression " : "",
           (fs->sb->features & AUTOFS_FEATURE_ENCRYPTION) ? "encryption " : "",
           (fs->sb->features & AUTOFS_FEATURE_SNAPSHOTS) ? "snapshots " : "",
           (fs->sb->features & AUTOFS_FEATURE_XATTR) ? "xattr" : "");

    return fs;

error:
    if (fs->block_bitmap) free(fs->block_bitmap);
    if (fs->inode_bitmap) free(fs->inode_bitmap);
    if (fs->sb) free(fs->sb);
    if (fs->device_path) free(fs->device_path);
    close(fs->fd);
    free(fs);
    return NULL;
}

/**
 * @brief Unmount an AutoFS filesystem
 *
 * @param fs Mounted filesystem
 * @return 0 on success, -1 on error
 */
int autofs_unmount(autofs_fs_t *fs) {
    if (!fs) {
        errno = EINVAL;
        return -1;
    }

    /* Flush all caches */
    autofs_cache_flush(fs);

    /* Save bitmaps */
    if (!fs->read_only) {
        autofs_save_bitmaps(fs);

        /* Update write time */
        fs->sb->write_time = time(NULL);
        autofs_write_superblock(fs);
    }

    /* Cleanup */
    autofs_cache_destroy(fs);

    if (fs->journal_buffer) {
        free(fs->journal_buffer);
    }

    if (fs->block_bitmap) {
        free(fs->block_bitmap);
    }

    if (fs->inode_bitmap) {
        free(fs->inode_bitmap);
    }

    if (fs->sb) {
        free(fs->sb);
    }

    if (fs->device_path) {
        free(fs->device_path);
    }

    close(fs->fd);
    free(fs);

    printf("AutoFS: Filesystem unmounted\n");
    return 0;
}

/**
 * @brief Read superblock from disk
 */
static int autofs_read_superblock(autofs_fs_t *fs) {
    if (lseek(fs->fd, 0, SEEK_SET) < 0) {
        return -1;
    }

    ssize_t ret = read(fs->fd, fs->sb, sizeof(autofs_superblock_t));
    if (ret != sizeof(autofs_superblock_t)) {
        return -1;
    }

    return 0;
}

/**
 * @brief Write superblock to disk
 */
static int autofs_write_superblock(autofs_fs_t *fs) {
    if (fs->read_only) {
        errno = EROFS;
        return -1;
    }

    if (lseek(fs->fd, 0, SEEK_SET) < 0) {
        return -1;
    }

    ssize_t ret = write(fs->fd, fs->sb, sizeof(autofs_superblock_t));
    if (ret != sizeof(autofs_superblock_t)) {
        return -1;
    }

    return fsync(fs->fd);
}

/**
 * @brief Load allocation bitmaps from disk
 */
static int autofs_load_bitmaps(autofs_fs_t *fs) {
    /* Allocate block bitmap */
    size_t block_bitmap_size = fs->sb->block_bitmap_blocks * AUTOFS_BLOCK_SIZE;
    fs->block_bitmap = malloc(block_bitmap_size);
    if (!fs->block_bitmap) {
        return -1;
    }

    /* Read block bitmap */
    off_t offset = fs->sb->block_bitmap_start * AUTOFS_BLOCK_SIZE;
    if (lseek(fs->fd, offset, SEEK_SET) < 0) {
        free(fs->block_bitmap);
        return -1;
    }

    if (read(fs->fd, fs->block_bitmap, block_bitmap_size) != (ssize_t)block_bitmap_size) {
        free(fs->block_bitmap);
        return -1;
    }

    /* Allocate inode bitmap */
    size_t inode_bitmap_size = fs->sb->inode_bitmap_blocks * AUTOFS_BLOCK_SIZE;
    fs->inode_bitmap = malloc(inode_bitmap_size);
    if (!fs->inode_bitmap) {
        free(fs->block_bitmap);
        return -1;
    }

    /* Read inode bitmap */
    offset = fs->sb->inode_bitmap_start * AUTOFS_BLOCK_SIZE;
    if (lseek(fs->fd, offset, SEEK_SET) < 0) {
        free(fs->block_bitmap);
        free(fs->inode_bitmap);
        return -1;
    }

    if (read(fs->fd, fs->inode_bitmap, inode_bitmap_size) != (ssize_t)inode_bitmap_size) {
        free(fs->block_bitmap);
        free(fs->inode_bitmap);
        return -1;
    }

    return 0;
}

/**
 * @brief Save allocation bitmaps to disk
 */
static int autofs_save_bitmaps(autofs_fs_t *fs) {
    if (fs->read_only) {
        errno = EROFS;
        return -1;
    }

    /* Write block bitmap */
    size_t block_bitmap_size = fs->sb->block_bitmap_blocks * AUTOFS_BLOCK_SIZE;
    off_t offset = fs->sb->block_bitmap_start * AUTOFS_BLOCK_SIZE;

    if (lseek(fs->fd, offset, SEEK_SET) < 0) {
        return -1;
    }

    if (write(fs->fd, fs->block_bitmap, block_bitmap_size) != (ssize_t)block_bitmap_size) {
        return -1;
    }

    /* Write inode bitmap */
    size_t inode_bitmap_size = fs->sb->inode_bitmap_blocks * AUTOFS_BLOCK_SIZE;
    offset = fs->sb->inode_bitmap_start * AUTOFS_BLOCK_SIZE;

    if (lseek(fs->fd, offset, SEEK_SET) < 0) {
        return -1;
    }

    if (write(fs->fd, fs->inode_bitmap, inode_bitmap_size) != (ssize_t)inode_bitmap_size) {
        return -1;
    }

    return fsync(fs->fd);
}

/**
 * @brief Allocate a new block
 *
 * @param fs Filesystem
 * @return Block number or 0 on error
 */
uint64_t autofs_alloc_block(autofs_fs_t *fs) {
    if (!fs || fs->read_only) {
        return 0;
    }

    if (fs->sb->free_blocks == 0) {
        errno = ENOSPC;
        return 0;
    }

    /* Find free block in bitmap */
    size_t bitmap_bytes = fs->sb->block_bitmap_blocks * AUTOFS_BLOCK_SIZE;
    for (size_t i = 0; i < bitmap_bytes; i++) {
        if (fs->block_bitmap[i] != 0xFF) {
            /* Found byte with free bit */
            for (int bit = 0; bit < 8; bit++) {
                if (!(fs->block_bitmap[i] & (1 << bit))) {
                    /* Mark as allocated */
                    fs->block_bitmap[i] |= (1 << bit);
                    fs->sb->free_blocks--;

                    uint64_t block_num = (i * 8 + bit) + fs->sb->data_start;
                    return block_num;
                }
            }
        }
    }

    errno = ENOSPC;
    return 0;
}

/**
 * @brief Free a block
 *
 * @param fs Filesystem
 * @param block Block number
 * @return 0 on success, -1 on error
 */
int autofs_free_block(autofs_fs_t *fs, uint64_t block) {
    if (!fs || fs->read_only) {
        errno = EROFS;
        return -1;
    }

    if (block < fs->sb->data_start || block >= fs->sb->block_count) {
        errno = EINVAL;
        return -1;
    }

    /* Calculate bitmap position */
    uint64_t rel_block = block - fs->sb->data_start;
    size_t byte = rel_block / 8;
    int bit = rel_block % 8;

    /* Mark as free */
    fs->block_bitmap[byte] &= ~(1 << bit);
    fs->sb->free_blocks++;

    return 0;
}

/**
 * @brief Read a block from disk
 *
 * @param fs Filesystem
 * @param block Block number
 * @param buf Buffer to read into (must be AUTOFS_BLOCK_SIZE bytes)
 * @return Number of bytes read or -1 on error
 */
ssize_t autofs_read_block(autofs_fs_t *fs, uint64_t block, void *buf) {
    if (!fs || !buf) {
        errno = EINVAL;
        return -1;
    }

    if (block >= fs->sb->block_count) {
        errno = EINVAL;
        return -1;
    }

    off_t offset = block * AUTOFS_BLOCK_SIZE;
    if (lseek(fs->fd, offset, SEEK_SET) < 0) {
        return -1;
    }

    ssize_t ret = read(fs->fd, buf, AUTOFS_BLOCK_SIZE);
    if (ret > 0) {
        fs->stats.reads++;
    }

    return ret;
}

/**
 * @brief Write a block to disk
 *
 * @param fs Filesystem
 * @param block Block number
 * @param buf Buffer to write from (must be AUTOFS_BLOCK_SIZE bytes)
 * @return Number of bytes written or -1 on error
 */
ssize_t autofs_write_block(autofs_fs_t *fs, uint64_t block, const void *buf) {
    if (!fs || !buf) {
        errno = EINVAL;
        return -1;
    }

    if (fs->read_only) {
        errno = EROFS;
        return -1;
    }

    if (block >= fs->sb->block_count) {
        errno = EINVAL;
        return -1;
    }

    off_t offset = block * AUTOFS_BLOCK_SIZE;
    if (lseek(fs->fd, offset, SEEK_SET) < 0) {
        return -1;
    }

    ssize_t ret = write(fs->fd, buf, AUTOFS_BLOCK_SIZE);
    if (ret > 0) {
        fs->stats.writes++;
    }

    return ret;
}

/**
 * @brief Allocate a new inode
 *
 * @param fs Filesystem
 * @return Inode number or 0 on error
 */
uint64_t autofs_alloc_inode(autofs_fs_t *fs) {
    if (!fs || fs->read_only) {
        errno = EROFS;
        return 0;
    }

    if (fs->sb->free_inodes == 0) {
        errno = ENOSPC;
        return 0;
    }

    /* Find free inode in bitmap */
    size_t bitmap_bytes = fs->sb->inode_bitmap_blocks * AUTOFS_BLOCK_SIZE;
    for (size_t i = 0; i < bitmap_bytes; i++) {
        if (fs->inode_bitmap[i] != 0xFF) {
            for (int bit = 0; bit < 8; bit++) {
                if (!(fs->inode_bitmap[i] & (1 << bit))) {
                    /* Mark as allocated */
                    fs->inode_bitmap[i] |= (1 << bit);
                    fs->sb->free_inodes--;

                    uint64_t ino = i * 8 + bit + 1;  /* Inode 0 is reserved */
                    return ino;
                }
            }
        }
    }

    errno = ENOSPC;
    return 0;
}

/**
 * @brief Free an inode
 *
 * @param fs Filesystem
 * @param ino Inode number
 * @return 0 on success, -1 on error
 */
int autofs_free_inode(autofs_fs_t *fs, uint64_t ino) {
    if (!fs || fs->read_only) {
        errno = EROFS;
        return -1;
    }

    if (ino == 0 || ino > fs->sb->inode_count) {
        errno = EINVAL;
        return -1;
    }

    /* Calculate bitmap position */
    uint64_t rel_ino = ino - 1;
    size_t byte = rel_ino / 8;
    int bit = rel_ino % 8;

    /* Mark as free */
    fs->inode_bitmap[byte] &= ~(1 << bit);
    fs->sb->free_inodes++;

    /* Invalidate cache */
    autofs_cache_invalidate(fs, ino);

    return 0;
}

/**
 * @brief Get an inode from disk
 *
 * @param fs Filesystem
 * @param ino Inode number
 * @return Inode structure or NULL on error
 */
autofs_inode_t* autofs_get_inode(autofs_fs_t *fs, uint64_t ino) {
    if (!fs || ino == 0 || ino > fs->sb->inode_count) {
        errno = EINVAL;
        return NULL;
    }

    /* Allocate inode structure */
    autofs_inode_t *inode = calloc(1, sizeof(autofs_inode_t));
    if (!inode) {
        return NULL;
    }

    /* Calculate inode location */
    uint64_t inode_block = fs->sb->inode_table_start +
                           ((ino - 1) * sizeof(autofs_inode_t)) / AUTOFS_BLOCK_SIZE;
    uint64_t inode_offset = ((ino - 1) * sizeof(autofs_inode_t)) % AUTOFS_BLOCK_SIZE;

    /* Read inode block */
    uint8_t block_buf[AUTOFS_BLOCK_SIZE];
    if (autofs_read_block(fs, inode_block, block_buf) < 0) {
        free(inode);
        return NULL;
    }

    /* Copy inode data */
    memcpy(inode, block_buf + inode_offset, sizeof(autofs_inode_t));

    return inode;
}

/**
 * @brief Write an inode to disk
 *
 * @param fs Filesystem
 * @param inode Inode to write
 * @return 0 on success, -1 on error
 */
int autofs_put_inode(autofs_fs_t *fs, autofs_inode_t *inode) {
    if (!fs || !inode || fs->read_only) {
        errno = EINVAL;
        return -1;
    }

    uint64_t ino = inode->ino;
    if (ino == 0 || ino > fs->sb->inode_count) {
        errno = EINVAL;
        return -1;
    }

    /* Calculate inode location */
    uint64_t inode_block = fs->sb->inode_table_start +
                           ((ino - 1) * sizeof(autofs_inode_t)) / AUTOFS_BLOCK_SIZE;
    uint64_t inode_offset = ((ino - 1) * sizeof(autofs_inode_t)) % AUTOFS_BLOCK_SIZE;

    /* Read block, modify, write back */
    uint8_t block_buf[AUTOFS_BLOCK_SIZE];
    if (autofs_read_block(fs, inode_block, block_buf) < 0) {
        return -1;
    }

    memcpy(block_buf + inode_offset, inode, sizeof(autofs_inode_t));

    if (autofs_write_block(fs, inode_block, block_buf) < 0) {
        return -1;
    }

    /* Invalidate cache */
    autofs_cache_invalidate(fs, ino);

    return 0;
}

/**
 * @brief Calculate checksum for data
 *
 * @param data Data to checksum
 * @param len Length of data
 * @return 64-bit checksum
 */
uint64_t autofs_checksum(const void *data, size_t len) {
    const uint8_t *bytes = data;
    uint64_t sum = 0;

    for (size_t i = 0; i < len; i++) {
        sum = ((sum << 5) + sum) + bytes[i];  /* sum = sum * 33 + byte */
    }

    return sum;
}

/**
 * @brief Print filesystem statistics
 *
 * @param fs Filesystem
 */
void autofs_print_stats(autofs_fs_t *fs) {
    if (!fs) {
        return;
    }

    printf("\n=== AutoFS Statistics ===\n");
    printf("Device: %s\n", fs->device_path);
    printf("Label: %s\n", fs->sb->label);
    printf("\nCapacity:\n");
    {
        uint64_t blk_used_pct = (fs->sb->block_count > 0) ?
            (100 * (fs->sb->block_count - fs->sb->free_blocks) / fs->sb->block_count) : 0;
        uint64_t blk_used_frac = (fs->sb->block_count > 0) ?
            ((1000 * (fs->sb->block_count - fs->sb->free_blocks) / fs->sb->block_count) % 10) : 0;
        printf("  Blocks: %lu total, %lu free (%lu.%lu%% used)\n",
               fs->sb->block_count, fs->sb->free_blocks, blk_used_pct, blk_used_frac);
    }
    {
        uint64_t ino_used_pct = (fs->sb->inode_count > 0) ?
            (100 * (fs->sb->inode_count - fs->sb->free_inodes) / fs->sb->inode_count) : 0;
        uint64_t ino_used_frac = (fs->sb->inode_count > 0) ?
            ((1000 * (fs->sb->inode_count - fs->sb->free_inodes) / fs->sb->inode_count) % 10) : 0;
        printf("  Inodes: %lu total, %lu free (%lu.%lu%% used)\n",
               fs->sb->inode_count, fs->sb->free_inodes, ino_used_pct, ino_used_frac);
    }
    printf("\nI/O Statistics:\n");
    printf("  Reads: %lu\n", fs->stats.reads);
    printf("  Writes: %lu\n", fs->stats.writes);
    printf("  Cache hits: %lu\n", fs->stats.cache_hits);
    printf("  Cache misses: %lu\n", fs->stats.cache_misses);
    if (fs->stats.cache_hits + fs->stats.cache_misses > 0) {
        {
            uint64_t total_ops = fs->stats.cache_hits + fs->stats.cache_misses;
            uint64_t hit_pct = 100 * fs->stats.cache_hits / total_ops;
            uint64_t hit_frac = (1000 * fs->stats.cache_hits / total_ops) % 10;
            printf("  Cache hit rate: %lu.%lu%%\n", hit_pct, hit_frac);
        }
    }
    printf("\nMount Info:\n");
    printf("  Mount count: %u\n", fs->sb->mount_count);
    printf("  Last mount: %s", ctime((time_t*)&fs->sb->mount_time));
    printf("  Last write: %s", ctime((time_t*)&fs->sb->write_time));
    printf("\nSnapshots: %lu\n", fs->sb->snapshot_count);
    printf("========================\n\n");
}
