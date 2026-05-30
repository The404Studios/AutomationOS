/**
 * @file dir.c
 * @brief AutoFS Directory Operations
 *
 * Implements directory-related operations.
 */

#include "../../include/autofs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/**
 * @brief Create a directory
 *
 * @param fs Filesystem
 * @param path Directory path
 * @param mode Permissions mode
 * @return 0 on success, -1 on error
 */
int autofs_mkdir(autofs_fs_t *fs, const char *path, uint32_t mode) {
    if (!fs || !path) {
        errno = EINVAL;
        return -1;
    }

    if (fs->read_only) {
        errno = EROFS;
        return -1;
    }

    /* Allocate inode */
    uint64_t ino = autofs_alloc_inode(fs);
    if (ino == 0) {
        return -1;
    }

    /* Create directory inode */
    autofs_inode_t *inode = calloc(1, sizeof(autofs_inode_t));
    if (!inode) {
        autofs_free_inode(fs, ino);
        return -1;
    }

    inode->ino = ino;
    inode->type = AUTOFS_TYPE_DIR;
    inode->mode = mode | 0040000;  /* S_IFDIR */
    inode->uid = 0;  /* TODO: Get current user */
    inode->gid = 0;  /* TODO: Get current group */
    inode->size = 0;
    inode->blocks = 0;
    inode->links_count = 2;  /* . and .. */
    inode->refcount = 1;

    time_t now = time(NULL);
    inode->atime = now;
    inode->mtime = now;
    inode->ctime = now;
    inode->crtime = now;

    /* Write inode */
    if (autofs_put_inode(fs, inode) < 0) {
        free(inode);
        autofs_free_inode(fs, ino);
        return -1;
    }

    free(inode);

    printf("AutoFS: Created directory '%s' (inode %lu)\n", path, ino);

    return 0;
}

/**
 * @brief Remove a directory
 *
 * @param fs Filesystem
 * @param path Directory path
 * @return 0 on success, -1 on error
 */
int autofs_rmdir(autofs_fs_t *fs, const char *path) {
    if (!fs || !path) {
        errno = EINVAL;
        return -1;
    }

    if (fs->read_only) {
        errno = EROFS;
        return -1;
    }

    /* Lookup directory */
    uint64_t ino;
    if (autofs_path_lookup(fs, path, &ino) < 0) {
        return -1;
    }

    /* Get inode */
    autofs_inode_t *inode = autofs_get_inode(fs, ino);
    if (!inode) {
        return -1;
    }

    /* Check if directory */
    if (inode->type != AUTOFS_TYPE_DIR) {
        free(inode);
        errno = ENOTDIR;
        return -1;
    }

    /* Check if empty */
    if (inode->size > 0) {
        free(inode);
        errno = ENOTEMPTY;
        return -1;
    }

    /* Free inode */
    autofs_free_inode(fs, ino);
    free(inode);

    printf("AutoFS: Removed directory '%s'\n", path);

    return 0;
}

/**
 * @brief Open directory for reading
 *
 * @param fs Filesystem
 * @param path Directory path
 * @return Directory handle or NULL on error
 */
autofs_dir_t* autofs_opendir(autofs_fs_t *fs, const char *path) {
    if (!fs || !path) {
        errno = EINVAL;
        return NULL;
    }

    /* Lookup directory */
    uint64_t ino;
    if (autofs_path_lookup(fs, path, &ino) < 0) {
        return NULL;
    }

    /* Get inode */
    autofs_inode_t *inode = autofs_get_inode(fs, ino);
    if (!inode) {
        return NULL;
    }

    /* Check if directory */
    if (inode->type != AUTOFS_TYPE_DIR) {
        free(inode);
        errno = ENOTDIR;
        return NULL;
    }

    /* Allocate directory handle */
    autofs_dir_t *dir = malloc(sizeof(autofs_dir_t));
    if (!dir) {
        free(inode);
        return NULL;
    }

    dir->fs = fs;
    dir->ino = ino;
    dir->inode = inode;
    dir->offset = 0;

    return dir;
}

/**
 * @brief Read next directory entry
 *
 * @param dir Directory handle
 * @return Directory entry or NULL if no more entries
 */
autofs_dirent_t* autofs_readdir(autofs_dir_t *dir) {
    if (!dir) {
        errno = EINVAL;
        return NULL;
    }

    /* Check if at end */
    if (dir->offset >= dir->inode->size) {
        return NULL;
    }

    /* Read directory entry */
    /* This is simplified - real implementation would read from data blocks */

    /* For now, return NULL (empty directory) */
    return NULL;
}

/**
 * @brief Close directory
 *
 * @param dir Directory handle
 * @return 0 on success, -1 on error
 */
int autofs_closedir(autofs_dir_t *dir) {
    if (!dir) {
        errno = EINVAL;
        return -1;
    }

    if (dir->inode) {
        free(dir->inode);
    }

    free(dir);

    return 0;
}
