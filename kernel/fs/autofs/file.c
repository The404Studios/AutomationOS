/**
 * @file file.c
 * @brief AutoFS File Operations
 *
 * Implements file I/O operations including open, read, write, close.
 */

#include "../../include/autofs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/**
 * @brief Open a file
 *
 * @param fs Filesystem
 * @param path File path
 * @param flags Open flags (O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, etc.)
 * @param ino Output: inode number
 * @return 0 on success, -1 on error
 */
int autofs_open(autofs_fs_t *fs, const char *path, int flags, uint64_t *ino) {
    if (!fs || !path || !ino) {
        errno = EINVAL;
        return -1;
    }

    /* Lookup path */
    if (autofs_path_lookup(fs, path, ino) < 0) {
        if ((flags & O_CREAT) && errno == ENOENT) {
            /* Create new file */
            *ino = autofs_alloc_inode(fs);
            if (*ino == 0) {
                return -1;
            }

            /* Initialize inode */
            autofs_inode_t *inode = calloc(1, sizeof(autofs_inode_t));
            if (!inode) {
                autofs_free_inode(fs, *ino);
                return -1;
            }

            inode->ino = *ino;
            inode->type = AUTOFS_TYPE_FILE;
            inode->mode = 0644;
            inode->uid = 0;
            inode->gid = 0;
            inode->size = 0;
            inode->blocks = 0;
            inode->links_count = 1;
            inode->refcount = 1;

            time_t now = time(NULL);
            inode->atime = now;
            inode->mtime = now;
            inode->ctime = now;
            inode->crtime = now;

            if (autofs_put_inode(fs, inode) < 0) {
                free(inode);
                autofs_free_inode(fs, *ino);
                return -1;
            }

            free(inode);

            printf("AutoFS: Created file '%s' (inode %lu)\n", path, *ino);
        } else {
            return -1;
        }
    }

    /* Check permissions */
    /* TODO: Implement permission checks */

    return 0;
}

/**
 * @brief Read from file
 *
 * @param file File handle
 * @param buf Buffer to read into
 * @param count Number of bytes to read
 * @return Number of bytes read or -1 on error
 */
ssize_t autofs_read(autofs_file_t *file, void *buf, size_t count) {
    if (!file || !buf) {
        errno = EINVAL;
        return -1;
    }

    autofs_fs_t *fs = file->fs;
    uint64_t ino = file->ino;

    /* Get inode */
    autofs_inode_t *inode = autofs_get_inode(fs, ino);
    if (!inode) {
        return -1;
    }

    /* Check if compressed */
    if (inode->flags & AUTOFS_INODE_COMPRESSED) {
        ssize_t ret = autofs_read_compressed(fs, ino, buf, count, file->offset);
        if (ret > 0) {
            file->offset += ret;
            inode->atime = time(NULL);
            autofs_put_inode(fs, inode);
        }
        free(inode);
        return ret;
    }

    /* Check if encrypted */
    if (inode->flags & AUTOFS_INODE_ENCRYPTED) {
        ssize_t ret = autofs_read_encrypted(fs, ino, buf, count, file->offset);
        if (ret > 0) {
            file->offset += ret;
            inode->atime = time(NULL);
            autofs_put_inode(fs, inode);
        }
        free(inode);
        return ret;
    }

    /* Regular read */
    uint64_t offset = file->offset;
    if (offset >= inode->size) {
        free(inode);
        return 0;  /* EOF */
    }

    /* Adjust count if reading past EOF */
    if (offset + count > inode->size) {
        count = inode->size - offset;
    }

    size_t bytes_read = 0;
    uint8_t *output = buf;

    while (bytes_read < count) {
        uint64_t block_idx = (offset + bytes_read) / AUTOFS_BLOCK_SIZE;
        uint64_t block_offset = (offset + bytes_read) % AUTOFS_BLOCK_SIZE;
        size_t to_read = AUTOFS_BLOCK_SIZE - block_offset;

        if (to_read > count - bytes_read) {
            to_read = count - bytes_read;
        }

        /* Read block */
        uint8_t block_buf[AUTOFS_BLOCK_SIZE];
        if (autofs_cow_read(fs, ino, block_idx, block_buf) < 0) {
            free(inode);
            return bytes_read > 0 ? bytes_read : -1;
        }

        memcpy(output + bytes_read, block_buf + block_offset, to_read);
        bytes_read += to_read;
    }

    file->offset += bytes_read;

    /* Update access time */
    if (!(inode->flags & AUTOFS_INODE_NO_ATIME)) {
        inode->atime = time(NULL);
        autofs_put_inode(fs, inode);
    }

    free(inode);
    return bytes_read;
}

/**
 * @brief Write to file
 *
 * @param file File handle
 * @param buf Buffer to write from
 * @param count Number of bytes to write
 * @return Number of bytes written or -1 on error
 */
ssize_t autofs_write(autofs_file_t *file, const void *buf, size_t count) {
    if (!file || !buf) {
        errno = EINVAL;
        return -1;
    }

    autofs_fs_t *fs = file->fs;
    uint64_t ino = file->ino;

    if (fs->read_only) {
        errno = EROFS;
        return -1;
    }

    /* Get inode */
    autofs_inode_t *inode = autofs_get_inode(fs, ino);
    if (!inode) {
        return -1;
    }

    /* Check if immutable */
    if (inode->flags & AUTOFS_INODE_IMMUTABLE) {
        free(inode);
        errno = EPERM;
        return -1;
    }

    /* Check if append-only */
    if (inode->flags & AUTOFS_INODE_APPEND_ONLY) {
        file->offset = inode->size;
    }

    uint64_t offset = file->offset;
    size_t bytes_written = 0;
    const uint8_t *input = buf;

    /* Begin transaction */
    if (fs->sb->features & AUTOFS_FEATURE_JOURNAL) {
        autofs_journal_begin(fs);
    }

    while (bytes_written < count) {
        uint64_t block_idx = (offset + bytes_written) / AUTOFS_BLOCK_SIZE;
        uint64_t block_offset = (offset + bytes_written) % AUTOFS_BLOCK_SIZE;
        size_t to_write = AUTOFS_BLOCK_SIZE - block_offset;

        if (to_write > count - bytes_written) {
            to_write = count - bytes_written;
        }

        /* Read-modify-write for partial blocks */
        uint8_t block_buf[AUTOFS_BLOCK_SIZE] = {0};

        if (block_offset != 0 || to_write != AUTOFS_BLOCK_SIZE) {
            /* Partial block - read first */
            autofs_cow_read(fs, ino, block_idx, block_buf);
        }

        memcpy(block_buf + block_offset, input + bytes_written, to_write);

        /* Write block using CoW */
        if (autofs_cow_write(fs, ino, block_idx, block_buf) < 0) {
            if (fs->sb->features & AUTOFS_FEATURE_JOURNAL) {
                autofs_journal_abort(fs);
            }
            free(inode);
            return bytes_written > 0 ? bytes_written : -1;
        }

        bytes_written += to_write;
    }

    /* Commit transaction */
    if (fs->sb->features & AUTOFS_FEATURE_JOURNAL) {
        autofs_journal_commit(fs);
    }

    /* Update inode */
    file->offset += bytes_written;

    if (file->offset > inode->size) {
        inode->size = file->offset;
    }

    inode->mtime = time(NULL);
    inode->ctime = time(NULL);

    autofs_put_inode(fs, inode);
    free(inode);

    return bytes_written;
}

/**
 * @brief Close file
 *
 * @param file File handle
 * @return 0 on success, -1 on error
 */
int autofs_close(autofs_file_t *file) {
    if (!file) {
        errno = EINVAL;
        return -1;
    }

    /* Flush any pending writes */
    /* Nothing to do for now */

    free(file);
    return 0;
}

/**
 * @brief Truncate file
 *
 * @param fs Filesystem
 * @param ino Inode number
 * @param size New size
 * @return 0 on success, -1 on error
 */
int autofs_truncate(autofs_fs_t *fs, uint64_t ino, uint64_t size) {
    if (!fs) {
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

    if (size < inode->size) {
        /* Shrink file - free blocks beyond new size */
        uint64_t blocks_needed = (size + AUTOFS_BLOCK_SIZE - 1) / AUTOFS_BLOCK_SIZE;
        uint64_t old_blocks = (inode->size + AUTOFS_BLOCK_SIZE - 1) / AUTOFS_BLOCK_SIZE;

        for (uint64_t i = blocks_needed; i < old_blocks; i++) {
            if (i < AUTOFS_DIRECT_BLOCKS && inode->direct[i] != 0) {
                autofs_free_block(fs, inode->direct[i]);
                inode->direct[i] = 0;
            }
        }
    }

    inode->size = size;
    inode->mtime = time(NULL);
    inode->ctime = time(NULL);

    int ret = autofs_put_inode(fs, inode);
    free(inode);

    return ret;
}

/**
 * @brief Unlink (delete) file
 *
 * @param fs Filesystem
 * @param path File path
 * @return 0 on success, -1 on error
 */
int autofs_unlink(autofs_fs_t *fs, const char *path) {
    if (!fs || !path) {
        errno = EINVAL;
        return -1;
    }

    if (fs->read_only) {
        errno = EROFS;
        return -1;
    }

    /* Lookup file */
    uint64_t ino;
    if (autofs_path_lookup(fs, path, &ino) < 0) {
        return -1;
    }

    /* Get inode */
    autofs_inode_t *inode = autofs_get_inode(fs, ino);
    if (!inode) {
        return -1;
    }

    /* Decrement link count */
    if (inode->links_count > 0) {
        inode->links_count--;
    }

    if (inode->links_count == 0) {
        /* Free all blocks */
        for (int i = 0; i < AUTOFS_DIRECT_BLOCKS; i++) {
            if (inode->direct[i] != 0) {
                autofs_free_block(fs, inode->direct[i]);
            }
        }

        /* Free inode */
        autofs_free_inode(fs, ino);
    } else {
        autofs_put_inode(fs, inode);
    }

    free(inode);

    printf("AutoFS: Unlinked '%s' (inode %lu)\n", path, ino);

    return 0;
}

/**
 * @brief Lookup path and return inode number
 *
 * @param fs Filesystem
 * @param path Path to lookup
 * @param ino Output: inode number
 * @return 0 on success, -1 on error
 */
int autofs_path_lookup(autofs_fs_t *fs, const char *path, uint64_t *ino) {
    if (!fs || !path || !ino) {
        errno = EINVAL;
        return -1;
    }

    /* Simplified path lookup - just use root inode for now */
    /* Real implementation would parse path and traverse directories */

    if (strcmp(path, "/") == 0) {
        *ino = fs->sb->root_inode;
        return 0;
    }

    /* For now, return error for non-root paths */
    errno = ENOENT;
    return -1;
}
