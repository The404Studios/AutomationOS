/**
 * @file xattr.c
 * @brief AutoFS Extended Attributes Implementation
 *
 * Extended attributes allow arbitrary metadata to be attached to files.
 * Common uses:
 * - security.capability
 * - user.comment
 * - encryption.key
 */

#include "../../include/autofs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/**
 * @brief Set extended attribute
 *
 * @param fs Filesystem
 * @param ino Inode number
 * @param name Attribute name
 * @param value Attribute value
 * @param size Value size
 * @return 0 on success, -1 on error
 */
int autofs_xattr_set(autofs_fs_t *fs, uint64_t ino, const char *name,
                    const void *value, size_t size) {
    if (!fs || !name || !value) {
        errno = EINVAL;
        return -1;
    }

    if (fs->read_only) {
        errno = EROFS;
        return -1;
    }

    if (!(fs->sb->features & AUTOFS_FEATURE_XATTR)) {
        errno = ENOTSUP;
        return -1;
    }

    /* Get inode */
    autofs_inode_t *inode = autofs_get_inode(fs, ino);
    if (!inode) {
        return -1;
    }

    /* Allocate xattr block if needed */
    if (inode->xattr_block == 0) {
        inode->xattr_block = autofs_alloc_block(fs);
        if (inode->xattr_block == 0) {
            free(inode);
            return -1;
        }

        /* Zero out xattr block */
        uint8_t zero_block[AUTOFS_BLOCK_SIZE] = {0};
        if (autofs_write_block(fs, inode->xattr_block, zero_block) < 0) {
            autofs_free_block(fs, inode->xattr_block);
            inode->xattr_block = 0;
            free(inode);
            return -1;
        }
    }

    /* Read xattr block */
    autofs_xattr_t xattr;
    if (autofs_read_block(fs, inode->xattr_block, &xattr) < 0) {
        free(inode);
        return -1;
    }

    /* Set attribute */
    strncpy(xattr.name, name, sizeof(xattr.name) - 1);
    xattr.value_len = size;

    if (size > sizeof(xattr.value)) {
        errno = ENOSPC;
        free(inode);
        return -1;
    }

    memcpy(xattr.value, value, size);

    /* Write xattr block */
    if (autofs_write_block(fs, inode->xattr_block, &xattr) < 0) {
        free(inode);
        return -1;
    }

    /* Update inode */
    int ret = autofs_put_inode(fs, inode);
    free(inode);

    return ret;
}

/**
 * @brief Get extended attribute
 *
 * @param fs Filesystem
 * @param ino Inode number
 * @param name Attribute name
 * @param value Buffer for value
 * @param size Buffer size
 * @return Value size or -1 on error
 */
ssize_t autofs_xattr_get(autofs_fs_t *fs, uint64_t ino, const char *name,
                        void *value, size_t size) {
    if (!fs || !name || !value) {
        errno = EINVAL;
        return -1;
    }

    /* Get inode */
    autofs_inode_t *inode = autofs_get_inode(fs, ino);
    if (!inode) {
        return -1;
    }

    if (inode->xattr_block == 0) {
        free(inode);
        errno = ENODATA;
        return -1;
    }

    /* Read xattr block */
    autofs_xattr_t xattr;
    if (autofs_read_block(fs, inode->xattr_block, &xattr) < 0) {
        free(inode);
        return -1;
    }

    free(inode);

    /* Check name matches */
    if (strncmp(xattr.name, name, sizeof(xattr.name)) != 0) {
        errno = ENODATA;
        return -1;
    }

    /* Copy value */
    if (size < xattr.value_len) {
        errno = ERANGE;
        return -1;
    }

    memcpy(value, xattr.value, xattr.value_len);

    return xattr.value_len;
}

/**
 * @brief List extended attributes
 *
 * @param fs Filesystem
 * @param ino Inode number
 * @param list Buffer for attribute names (null-separated)
 * @param size Buffer size
 * @return Size of list or -1 on error
 */
ssize_t autofs_xattr_list(autofs_fs_t *fs, uint64_t ino, char *list, size_t size) {
    if (!fs || !list) {
        errno = EINVAL;
        return -1;
    }

    /* Get inode */
    autofs_inode_t *inode = autofs_get_inode(fs, ino);
    if (!inode) {
        return -1;
    }

    if (inode->xattr_block == 0) {
        free(inode);
        return 0;  /* No xattrs */
    }

    /* Read xattr block */
    autofs_xattr_t xattr;
    if (autofs_read_block(fs, inode->xattr_block, &xattr) < 0) {
        free(inode);
        return -1;
    }

    free(inode);

    /* Copy name */
    size_t name_len = strlen(xattr.name) + 1;  /* Include null */
    if (size < name_len) {
        errno = ERANGE;
        return -1;
    }

    memcpy(list, xattr.name, name_len);

    return name_len;
}

/**
 * @brief Remove extended attribute
 *
 * @param fs Filesystem
 * @param ino Inode number
 * @param name Attribute name
 * @return 0 on success, -1 on error
 */
int autofs_xattr_remove(autofs_fs_t *fs, uint64_t ino, const char *name) {
    if (!fs || !name) {
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

    if (inode->xattr_block == 0) {
        free(inode);
        errno = ENODATA;
        return -1;
    }

    /* Free xattr block */
    autofs_free_block(fs, inode->xattr_block);
    inode->xattr_block = 0;

    /* Update inode */
    int ret = autofs_put_inode(fs, inode);
    free(inode);

    return ret;
}
