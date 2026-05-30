/**
 * @file cow.c
 * @brief AutoFS Copy-on-Write Implementation
 *
 * Copy-on-write ensures that blocks are never modified in-place.
 * When writing to a block:
 * 1. Allocate a new block
 * 2. Write data to new block
 * 3. Update inode to point to new block
 * 4. Old block becomes free after refcount reaches zero
 *
 * This enables:
 * - Instant snapshots (just increment refcounts)
 * - Atomic updates
 * - Data integrity
 */

#include "../../include/autofs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/**
 * @brief Write block using copy-on-write
 *
 * @param fs Filesystem
 * @param ino Inode number
 * @param block_idx Block index within file (0, 1, 2, ...)
 * @param data Data to write (AUTOFS_BLOCK_SIZE bytes)
 * @return 0 on success, -1 on error
 */
int autofs_cow_write(autofs_fs_t *fs, uint64_t ino, uint64_t block_idx,
                    const void *data) {
    if (!fs || !data) {
        errno = EINVAL;
        return -1;
    }

    if (fs->read_only) {
        errno = EROFS;
        return -1;
    }

    /* Get inode */
    autofs_inode_t *inode = autofs_get_inode(fs, ino);
    if (!inode) {
        return -1;
    }

    /* Get old block number */
    uint64_t old_block = 0;
    if (block_idx < AUTOFS_DIRECT_BLOCKS) {
        old_block = inode->direct[block_idx];
    } else {
        /* Handle indirect blocks */
        /* Simplified for now */
        fprintf(stderr, "Indirect blocks not yet implemented\n");
        free(inode);
        errno = ENOSYS;
        return -1;
    }

    /* Allocate new block */
    uint64_t new_block = autofs_alloc_block(fs);
    if (new_block == 0) {
        free(inode);
        return -1;
    }

    /* Begin transaction */
    if (fs->sb->features & AUTOFS_FEATURE_JOURNAL) {
        autofs_journal_begin(fs);
    }

    /* Write data to new block */
    if (autofs_write_block(fs, new_block, data) < 0) {
        autofs_free_block(fs, new_block);
        free(inode);

        if (fs->sb->features & AUTOFS_FEATURE_JOURNAL) {
            autofs_journal_abort(fs);
        }

        return -1;
    }

    /* Log to journal */
    if (fs->sb->features & AUTOFS_FEATURE_JOURNAL) {
        autofs_journal_log(fs, JOURNAL_OP_WRITE, new_block, data);
    }

    /* Update inode to point to new block */
    inode->direct[block_idx] = new_block;
    inode->mtime = time(NULL);
    inode->ctime = time(NULL);

    if (autofs_put_inode(fs, inode) < 0) {
        autofs_free_block(fs, new_block);
        free(inode);

        if (fs->sb->features & AUTOFS_FEATURE_JOURNAL) {
            autofs_journal_abort(fs);
        }

        return -1;
    }

    /* Commit transaction */
    if (fs->sb->features & AUTOFS_FEATURE_JOURNAL) {
        autofs_journal_commit(fs);
    }

    /* Decrement refcount on old block */
    if (old_block != 0) {
        /* In a real implementation, we'd track refcounts and free when zero */
        /* For now, just free immediately if refcount == 1 */
        autofs_free_block(fs, old_block);
    }

    free(inode);
    return 0;
}

/**
 * @brief Increment reference count for CoW
 *
 * @param fs Filesystem
 * @param ino Inode number
 * @return 0 on success, -1 on error
 */
int autofs_cow_ref_inc(autofs_fs_t *fs, uint64_t ino) {
    if (!fs) {
        errno = EINVAL;
        return -1;
    }

    /* Get inode */
    autofs_inode_t *inode = autofs_get_inode(fs, ino);
    if (!inode) {
        return -1;
    }

    /* Increment refcount */
    inode->refcount++;

    /* Write back */
    int ret = autofs_put_inode(fs, inode);
    free(inode);

    return ret;
}

/**
 * @brief Decrement reference count for CoW
 *
 * @param fs Filesystem
 * @param ino Inode number
 * @return 0 on success, -1 on error
 */
int autofs_cow_ref_dec(autofs_fs_t *fs, uint64_t ino) {
    if (!fs) {
        errno = EINVAL;
        return -1;
    }

    /* Get inode */
    autofs_inode_t *inode = autofs_get_inode(fs, ino);
    if (!inode) {
        return -1;
    }

    if (inode->refcount == 0) {
        free(inode);
        errno = EINVAL;
        return -1;
    }

    /* Decrement refcount */
    inode->refcount--;

    /* If refcount reaches zero, free blocks */
    if (inode->refcount == 0) {
        /* Free all data blocks */
        for (int i = 0; i < AUTOFS_DIRECT_BLOCKS; i++) {
            if (inode->direct[i] != 0) {
                autofs_free_block(fs, inode->direct[i]);
            }
        }

        /* Free indirect blocks */
        if (inode->indirect != 0) {
            /* TODO: Implement indirect block freeing */
            autofs_free_block(fs, inode->indirect);
        }

        /* Free inode */
        autofs_free_inode(fs, ino);
    } else {
        /* Write back updated refcount */
        autofs_put_inode(fs, inode);
    }

    free(inode);
    return 0;
}

/**
 * @brief Clone inode for CoW (used by snapshots)
 *
 * @param fs Filesystem
 * @param ino Source inode number
 * @return New inode number or 0 on error
 */
uint64_t autofs_cow_clone_inode(autofs_fs_t *fs, uint64_t ino) {
    if (!fs) {
        errno = EINVAL;
        return 0;
    }

    /* Get source inode */
    autofs_inode_t *src_inode = autofs_get_inode(fs, ino);
    if (!src_inode) {
        return 0;
    }

    /* Allocate new inode */
    uint64_t new_ino = autofs_alloc_inode(fs);
    if (new_ino == 0) {
        free(src_inode);
        return 0;
    }

    /* Copy inode data */
    autofs_inode_t *new_inode = malloc(sizeof(autofs_inode_t));
    if (!new_inode) {
        autofs_free_inode(fs, new_ino);
        free(src_inode);
        return 0;
    }

    memcpy(new_inode, src_inode, sizeof(autofs_inode_t));
    new_inode->ino = new_ino;
    new_inode->refcount = 1;

    /* Increment refcounts on all blocks */
    for (int i = 0; i < AUTOFS_DIRECT_BLOCKS; i++) {
        if (src_inode->direct[i] != 0) {
            /* In real implementation, increment block refcount */
            /* For now, blocks are shared */
        }
    }

    /* Write new inode */
    if (autofs_put_inode(fs, new_inode) < 0) {
        autofs_free_inode(fs, new_ino);
        free(new_inode);
        free(src_inode);
        return 0;
    }

    free(new_inode);
    free(src_inode);

    return new_ino;
}

/**
 * @brief Read block with CoW semantics (no modification)
 *
 * @param fs Filesystem
 * @param ino Inode number
 * @param block_idx Block index within file
 * @param buf Buffer to read into (AUTOFS_BLOCK_SIZE bytes)
 * @return 0 on success, -1 on error
 */
int autofs_cow_read(autofs_fs_t *fs, uint64_t ino, uint64_t block_idx,
                   void *buf) {
    if (!fs || !buf) {
        errno = EINVAL;
        return -1;
    }

    /* Get inode */
    autofs_inode_t *inode = autofs_get_inode(fs, ino);
    if (!inode) {
        return -1;
    }

    /* Get block number */
    uint64_t block = 0;
    if (block_idx < AUTOFS_DIRECT_BLOCKS) {
        block = inode->direct[block_idx];
    } else {
        /* Handle indirect blocks */
        fprintf(stderr, "Indirect blocks not yet implemented\n");
        free(inode);
        errno = ENOSYS;
        return -1;
    }

    free(inode);

    if (block == 0) {
        /* Sparse file - return zeros */
        memset(buf, 0, AUTOFS_BLOCK_SIZE);
        return 0;
    }

    /* Read block */
    return autofs_read_block(fs, block, buf) < 0 ? -1 : 0;
}

/**
 * @brief Handle indirect block allocation for CoW
 *
 * @param fs Filesystem
 * @param inode Inode
 * @param block_idx Block index within file
 * @return Physical block number or 0 on error
 */
static uint64_t cow_get_indirect_block(autofs_fs_t *fs, autofs_inode_t *inode,
                                      uint64_t block_idx) {
    if (block_idx < AUTOFS_DIRECT_BLOCKS) {
        return inode->direct[block_idx];
    }

    /* Single indirect */
    block_idx -= AUTOFS_DIRECT_BLOCKS;
    uint64_t entries_per_block = AUTOFS_BLOCK_SIZE / sizeof(uint64_t);

    if (block_idx < entries_per_block) {
        /* Allocate indirect block if needed */
        if (inode->indirect == 0) {
            inode->indirect = autofs_alloc_block(fs);
            if (inode->indirect == 0) {
                return 0;
            }

            /* Zero out indirect block */
            uint8_t zero_block[AUTOFS_BLOCK_SIZE] = {0};
            autofs_write_block(fs, inode->indirect, zero_block);
        }

        /* Read indirect block */
        uint64_t indirect_data[entries_per_block];
        if (autofs_read_block(fs, inode->indirect, indirect_data) < 0) {
            return 0;
        }

        return indirect_data[block_idx];
    }

    /* Double indirect */
    block_idx -= entries_per_block;
    if (block_idx < entries_per_block * entries_per_block) {
        /* TODO: Implement double indirect */
        errno = ENOSYS;
        return 0;
    }

    /* Triple indirect */
    /* TODO: Implement triple indirect */
    errno = ENOSYS;
    return 0;
}

/**
 * @brief Set indirect block pointer for CoW
 *
 * @param fs Filesystem
 * @param inode Inode
 * @param block_idx Block index within file
 * @param block Physical block number
 * @return 0 on success, -1 on error
 */
static int cow_set_indirect_block(autofs_fs_t *fs, autofs_inode_t *inode,
                                 uint64_t block_idx, uint64_t block) {
    if (block_idx < AUTOFS_DIRECT_BLOCKS) {
        inode->direct[block_idx] = block;
        return 0;
    }

    /* Single indirect */
    block_idx -= AUTOFS_DIRECT_BLOCKS;
    uint64_t entries_per_block = AUTOFS_BLOCK_SIZE / sizeof(uint64_t);

    if (block_idx < entries_per_block) {
        /* Allocate indirect block if needed */
        if (inode->indirect == 0) {
            inode->indirect = autofs_alloc_block(fs);
            if (inode->indirect == 0) {
                return -1;
            }

            /* Zero out indirect block */
            uint8_t zero_block[AUTOFS_BLOCK_SIZE] = {0};
            autofs_write_block(fs, inode->indirect, zero_block);
        }

        /* Read indirect block */
        uint64_t indirect_data[entries_per_block];
        if (autofs_read_block(fs, inode->indirect, indirect_data) < 0) {
            return -1;
        }

        /* Update entry */
        indirect_data[block_idx] = block;

        /* Write back indirect block using CoW */
        uint64_t new_indirect = autofs_alloc_block(fs);
        if (new_indirect == 0) {
            return -1;
        }

        if (autofs_write_block(fs, new_indirect, indirect_data) < 0) {
            autofs_free_block(fs, new_indirect);
            return -1;
        }

        /* Free old indirect block */
        uint64_t old_indirect = inode->indirect;
        inode->indirect = new_indirect;
        autofs_free_block(fs, old_indirect);

        return 0;
    }

    /* Double/triple indirect - not implemented */
    errno = ENOSYS;
    return -1;
}

/**
 * @brief Calculate maximum file size with current block pointers
 *
 * @return Maximum file size in bytes
 */
uint64_t autofs_cow_max_file_size(void) {
    uint64_t entries_per_block = AUTOFS_BLOCK_SIZE / sizeof(uint64_t);

    uint64_t max_size = 0;

    /* Direct blocks */
    max_size += AUTOFS_DIRECT_BLOCKS * AUTOFS_BLOCK_SIZE;

    /* Single indirect */
    max_size += entries_per_block * AUTOFS_BLOCK_SIZE;

    /* Double indirect */
    max_size += entries_per_block * entries_per_block * AUTOFS_BLOCK_SIZE;

    /* Triple indirect */
    max_size += entries_per_block * entries_per_block * entries_per_block * AUTOFS_BLOCK_SIZE;

    return max_size;
}
