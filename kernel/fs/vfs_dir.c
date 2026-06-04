/**
 * VFS Directory Operations
 *
 * Implements directory-specific VFS operations including readdir, stat, unlink, rename
 */

#include "../include/vfs.h"
#include "../include/kernel.h"
#include "../include/mem.h"
#include "../include/string.h"
#include "../include/syscall.h"

// cleanup helper: auto-kfree a heap path buffer on scope exit -- keeps the
// 4096-byte path buffers in vfs_unlink/vfs_rename OFF the 8KB kernel stack
// (vfs_rename had TWO = 8KB, overflowing on its own even after sys_rename was
// heap-allocated). kfree(NULL) is safe.
static inline void free_path_buf(char** p) { if (*p) kfree(*p); }

// Directory handle structure
typedef struct {
    vfs_inode_t* inode;
    uint64_t position;  // Current entry index
    uint32_t ref_count;
} vfs_dir_handle_t;

// Global directory handle table (per-process would be better, but keep it simple)
#define MAX_DIR_HANDLES 64
static vfs_dir_handle_t* dir_handles[MAX_DIR_HANDLES];
static uint32_t dir_handles_initialized = 0;

/**
 * Initialize directory handle table
 */
static void vfs_dir_init(void) {
    if (!dir_handles_initialized) {
        for (int i = 0; i < MAX_DIR_HANDLES; i++) {
            dir_handles[i] = NULL;
        }
        dir_handles_initialized = 1;
    }
}

/**
 * Allocate directory handle
 */
static int vfs_dir_alloc_handle(vfs_inode_t* inode) {
    vfs_dir_init();

    // Find free slot
    for (int i = 0; i < MAX_DIR_HANDLES; i++) {
        if (!dir_handles[i]) {
            dir_handles[i] = (vfs_dir_handle_t*)kmalloc(sizeof(vfs_dir_handle_t));
            if (!dir_handles[i]) {
                return -1;
            }

            dir_handles[i]->inode = inode;
            dir_handles[i]->position = 0;
            dir_handles[i]->ref_count = 1;
            vfs_inode_get(inode);

            return i;
        }
    }

    return -1;  // No free handles
}

/**
 * Free directory handle
 */
static void vfs_dir_free_handle(int handle) {
    if (handle >= 0 && handle < MAX_DIR_HANDLES && dir_handles[handle]) {
        vfs_dir_handle_t* dh = dir_handles[handle];

        if (dh->inode) {
            vfs_inode_put(dh->inode);
        }

        kfree(dh);
        dir_handles[handle] = NULL;
    }
}

/**
 * Get directory handle
 */
static vfs_dir_handle_t* vfs_dir_get_handle(int handle) {
    if (handle >= 0 && handle < MAX_DIR_HANDLES) {
        return dir_handles[handle];
    }
    return NULL;
}

/**
 * Open directory - allocate handle for directory iteration
 * Returns: directory handle (>= 0) on success, negative error code on failure
 */
int vfs_opendir(const char* path) {
    if (!path) {
        return VFS_ERR_INVAL;
    }

    // Lookup directory inode
    vfs_inode_t* inode = vfs_path_lookup(path);
    if (!inode) {
        return VFS_ERR_NOENT;
    }

    // Verify it's a directory
    if (!(inode->type & VFS_TYPE_DIR)) {
        vfs_inode_put(inode);
        return VFS_ERR_NOTDIR;
    }

    // Allocate directory handle. alloc_handle takes its OWN reference on the
    // inode (vfs_inode_get), so we must drop the reference vfs_path_lookup gave
    // us — otherwise every opendir/closedir cycle leaks one inode reference and
    // the inode is never freed.
    int handle = vfs_dir_alloc_handle(inode);
    if (handle < 0) {
        vfs_inode_put(inode);
        return VFS_ERR_NOMEM;
    }

    vfs_inode_put(inode);  // release the lookup ref; the handle holds its own
    return handle;
}

/**
 * Read directory entry
 * Returns: 0 on success, negative error code on failure
 *
 * Note: dirent structure must be provided by caller
 */
int vfs_readdir(int dirfd, struct dirent* entry) {
    if (!entry) {
        return VFS_ERR_INVAL;
    }

    vfs_dir_handle_t* dh = vfs_dir_get_handle(dirfd);
    if (!dh || !dh->inode) {
        return VFS_ERR_BADF;
    }

    vfs_inode_t* dir = dh->inode;

    // Verify it's a directory
    if (!(dir->type & VFS_TYPE_DIR)) {
        return VFS_ERR_NOTDIR;
    }

    /* BUG-FIX (sparse-array / invisible entries after unlink):
     * After vfs_unlink() removes an entry, it sets the slot to NULL and
     * decrements dir->size.  Using dir->size as the loop bound means we stop
     * scanning before we reach live entries whose indices are now >= dir->size.
     * Fix: use dir->data_capacity as the bound (scan the full array) and skip
     * NULL slots.  dh->position persists across calls so this is safe. */

    // Check if we have entries
    if (!dir->private_data || dh->position >= dir->data_capacity) {
        return VFS_ERR_NOENT;  // End of directory
    }

    // Get directory entries array
    vfs_dentry_t** entries = (vfs_dentry_t**)dir->private_data;

    // Find next valid entry
    while (dh->position < dir->data_capacity) {
        vfs_dentry_t* dentry = entries[dh->position];

        if (dentry && dentry->inode) {
            // Fill in dirent structure
            memset(entry, 0, sizeof(struct dirent));

            entry->d_ino = dentry->inode->ino;
            entry->d_off = dh->position;
            entry->d_reclen = sizeof(struct dirent);

            // Set file type
            if (dentry->inode->type & VFS_TYPE_DIR) {
                entry->d_type = DT_DIR;
            } else if (dentry->inode->type & VFS_TYPE_FILE) {
                entry->d_type = DT_REG;
            } else if (dentry->inode->type & VFS_TYPE_SYMLINK) {
                entry->d_type = DT_LNK;
            } else if (dentry->inode->type & VFS_TYPE_DEVICE) {
                entry->d_type = DT_CHR;
            } else {
                entry->d_type = DT_UNKNOWN;
            }

            // Copy name (safely)
            size_t name_len = strlen(dentry->name);
            if (name_len >= NAME_MAX) {
                name_len = NAME_MAX - 1;
            }
            memcpy(entry->d_name, dentry->name, name_len);
            entry->d_name[name_len] = '\0';

            // Advance position
            dh->position++;

            return 0;  // Success
        }

        dh->position++;
    }

    // End of directory (exhausted all capacity slots)
    return VFS_ERR_NOENT;
}

/**
 * Close directory handle
 */
int vfs_closedir(int dirfd) {
    vfs_dir_handle_t* dh = vfs_dir_get_handle(dirfd);
    if (!dh) {
        return VFS_ERR_BADF;
    }

    vfs_dir_free_handle(dirfd);
    return 0;
}

/**
 * Get file/directory status (stat)
 * Note: vfs_stat is already implemented in vfs.c, this is just a wrapper
 */
int vfs_stat_wrapper(const char* path, vfs_stat_t* buf) {
    return vfs_stat(path, buf);
}

/**
 * Delete file (unlink)
 */
int vfs_unlink(const char* path) {
    if (!path) {
        return VFS_ERR_INVAL;
    }

    // Find parent directory and filename (heap buffer, auto-freed)
    char* parent_path __attribute__((cleanup(free_path_buf))) = (char*)kmalloc(VFS_MAX_PATH);
    if (!parent_path) {
        return VFS_ERR_INVAL;
    }
    const char* filename;

    // Find last slash
    const char* last_slash = NULL;
    for (const char* p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    if (!last_slash) {
        return VFS_ERR_INVAL;
    }

    // Copy parent path
    size_t parent_len = last_slash - path;
    if (parent_len == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        if (parent_len >= VFS_MAX_PATH) {
            parent_len = VFS_MAX_PATH - 1;
        }
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
    }

    filename = last_slash + 1;
    if (*filename == '\0') {
        return VFS_ERR_INVAL;  // Can't unlink directory with trailing slash
    }

    // Lookup parent directory
    vfs_inode_t* parent = vfs_path_lookup(parent_path);
    if (!parent) {
        return VFS_ERR_NOENT;
    }

    if (!(parent->type & VFS_TYPE_DIR)) {
        vfs_inode_put(parent);
        return VFS_ERR_NOTDIR;
    }

    // Find the entry in parent directory
    if (!parent->private_data) {
        vfs_inode_put(parent);
        return VFS_ERR_NOENT;
    }

    vfs_dentry_t** entries = (vfs_dentry_t**)parent->private_data;
    int found = -1;

    for (uint64_t i = 0; i < parent->data_capacity; i++) {
        if (entries[i] && strcmp(entries[i]->name, filename) == 0) {
            found = i;
            break;
        }
    }

    if (found < 0) {
        vfs_inode_put(parent);
        return VFS_ERR_NOENT;
    }

    vfs_dentry_t* dentry = entries[found];

    // Can't unlink directories with this call (use rmdir instead)
    if (dentry->inode && (dentry->inode->type & VFS_TYPE_DIR)) {
        vfs_inode_put(parent);
        return VFS_ERR_ISDIR;
    }

    // Remove from parent directory
    entries[found] = NULL;
    parent->size--;

    // Free the dentry; vfs_dentry_free() drops the inode's reference (and frees
    // the inode if this was the last link), so we must NOT put it again here.
    vfs_dentry_free(dentry);

    vfs_inode_put(parent);
    return 0;
}

/**
 * Rename/move file or directory
 */
int vfs_rename(const char* oldpath, const char* newpath) {
    if (!oldpath || !newpath) {
        return VFS_ERR_INVAL;
    }

    // Parse old path (heap buffer, auto-freed)
    char* old_parent_path __attribute__((cleanup(free_path_buf))) = (char*)kmalloc(VFS_MAX_PATH);
    if (!old_parent_path) {
        return VFS_ERR_INVAL;
    }
    const char* old_filename;
    const char* old_last_slash = NULL;

    for (const char* p = oldpath; *p; p++) {
        if (*p == '/') old_last_slash = p;
    }

    if (!old_last_slash) {
        return VFS_ERR_INVAL;
    }

    size_t old_parent_len = old_last_slash - oldpath;
    if (old_parent_len == 0) {
        old_parent_path[0] = '/';
        old_parent_path[1] = '\0';
    } else {
        if (old_parent_len >= VFS_MAX_PATH) {
            old_parent_len = VFS_MAX_PATH - 1;
        }
        memcpy(old_parent_path, oldpath, old_parent_len);
        old_parent_path[old_parent_len] = '\0';
    }

    old_filename = old_last_slash + 1;
    if (*old_filename == '\0') {
        return VFS_ERR_INVAL;
    }

    // Parse new path (heap buffer, auto-freed)
    char* new_parent_path __attribute__((cleanup(free_path_buf))) = (char*)kmalloc(VFS_MAX_PATH);
    if (!new_parent_path) {
        return VFS_ERR_INVAL;
    }
    const char* new_filename;
    const char* new_last_slash = NULL;

    for (const char* p = newpath; *p; p++) {
        if (*p == '/') new_last_slash = p;
    }

    if (!new_last_slash) {
        return VFS_ERR_INVAL;
    }

    size_t new_parent_len = new_last_slash - newpath;
    if (new_parent_len == 0) {
        new_parent_path[0] = '/';
        new_parent_path[1] = '\0';
    } else {
        if (new_parent_len >= VFS_MAX_PATH) {
            new_parent_len = VFS_MAX_PATH - 1;
        }
        memcpy(new_parent_path, newpath, new_parent_len);
        new_parent_path[new_parent_len] = '\0';
    }

    new_filename = new_last_slash + 1;
    if (*new_filename == '\0') {
        return VFS_ERR_INVAL;
    }

    // Lookup old parent
    vfs_inode_t* old_parent = vfs_path_lookup(old_parent_path);
    if (!old_parent) {
        return VFS_ERR_NOENT;
    }

    if (!(old_parent->type & VFS_TYPE_DIR)) {
        vfs_inode_put(old_parent);
        return VFS_ERR_NOTDIR;
    }

    // Lookup new parent
    vfs_inode_t* new_parent = vfs_path_lookup(new_parent_path);
    if (!new_parent) {
        vfs_inode_put(old_parent);
        return VFS_ERR_NOENT;
    }

    if (!(new_parent->type & VFS_TYPE_DIR)) {
        vfs_inode_put(old_parent);
        vfs_inode_put(new_parent);
        return VFS_ERR_NOTDIR;
    }

    // Find entry in old parent
    if (!old_parent->private_data) {
        vfs_inode_put(old_parent);
        vfs_inode_put(new_parent);
        return VFS_ERR_NOENT;
    }

    vfs_dentry_t** old_entries = (vfs_dentry_t**)old_parent->private_data;
    int old_index = -1;

    for (uint64_t i = 0; i < old_parent->data_capacity; i++) {
        if (old_entries[i] && strcmp(old_entries[i]->name, old_filename) == 0) {
            old_index = i;
            break;
        }
    }

    if (old_index < 0) {
        vfs_inode_put(old_parent);
        vfs_inode_put(new_parent);
        return VFS_ERR_NOENT;
    }

    vfs_dentry_t* dentry = old_entries[old_index];

    // Self-rename (same directory, same name) is a no-op. Without this guard the
    // destination-removal loop below matches THIS very dentry (same parent + same
    // name), vfs_dentry_free()s it, and the name memcpy / re-insert further down
    // is then a use-after-free + directory corruption.
    if (old_parent == new_parent && strcmp(old_filename, new_filename) == 0) {
        vfs_inode_put(old_parent);
        vfs_inode_put(new_parent);
        return 0;
    }

    // Check if destination exists and remove it
    if (new_parent->private_data) {
        vfs_dentry_t** new_entries = (vfs_dentry_t**)new_parent->private_data;
        for (uint64_t i = 0; i < new_parent->data_capacity; i++) {
            if (new_entries[i] && strcmp(new_entries[i]->name, new_filename) == 0) {
                // Never free the source dentry itself (defensive: e.g. case-folded
                // collisions). The self-rename short-circuit above is the primary
                // guard; this prevents any residual aliasing from freeing `dentry`.
                if (new_entries[i] == dentry) {
                    continue;
                }
                // Destination exists - remove it. vfs_dentry_free() drops the
                // inode reference, so don't put it separately (double-free).
                vfs_dentry_t* old_dentry = new_entries[i];
                new_entries[i] = NULL;
                new_parent->size--;

                vfs_dentry_free(old_dentry);
                break;
            }
        }
    }

    // Remove from old parent
    old_entries[old_index] = NULL;
    old_parent->size--;

    // Update dentry name
    size_t name_len = strlen(new_filename);
    if (name_len >= VFS_MAX_NAME) {
        name_len = VFS_MAX_NAME - 1;
    }
    memcpy(dentry->name, new_filename, name_len);
    dentry->name[name_len] = '\0';

    /* BUG-FIX (rename to full directory): the original code open-coded an
     * inline 16-slot-only insertion into the new parent, so a rename into a
     * directory with >= 16 entries would always fail with VFS_ERR_NOMEM rather
     * than growing the array.  Replicate the same doubling-growth logic used
     * by ramfs_dir_add() in vfs.c (that function is static and not visible
     * here). */

    // Ensure new parent has an allocated entry array.
    if (!new_parent->private_data) {
        new_parent->private_data = kmalloc(sizeof(vfs_dentry_t*) * 16);
        if (!new_parent->private_data) {
            old_entries[old_index] = dentry;
            old_parent->size++;
            vfs_inode_put(old_parent);
            vfs_inode_put(new_parent);
            return VFS_ERR_NOMEM;
        }
        memset(new_parent->private_data, 0, sizeof(vfs_dentry_t*) * 16);
        new_parent->data_capacity = 16;
    }

    {
        vfs_dentry_t** new_entries = (vfs_dentry_t**)new_parent->private_data;
        int added = 0;

        // Try an empty slot first.
        for (uint64_t i = 0; i < new_parent->data_capacity; i++) {
            if (!new_entries[i]) {
                new_entries[i] = dentry;
                new_parent->size++;
                added = 1;
                break;
            }
        }

        if (!added) {
            // Array is full — grow it (double capacity), then insert.
            uint64_t new_cap = new_parent->data_capacity * 2;
            vfs_dentry_t** grown = (vfs_dentry_t**)kmalloc(
                sizeof(vfs_dentry_t*) * new_cap);
            if (!grown) {
                // Out of memory - restore dentry to old parent.
                old_entries[old_index] = dentry;
                old_parent->size++;
                vfs_inode_put(old_parent);
                vfs_inode_put(new_parent);
                return VFS_ERR_NOMEM;
            }
            memset(grown, 0, sizeof(vfs_dentry_t*) * new_cap);
            memcpy(grown, new_entries,
                   sizeof(vfs_dentry_t*) * new_parent->data_capacity);
            kfree(new_entries);
            new_parent->private_data = grown;

            // First slot beyond old capacity is guaranteed empty.
            grown[new_parent->data_capacity] = dentry;
            new_parent->data_capacity = new_cap;
            new_parent->size++;
        }
    }

    vfs_inode_put(old_parent);
    vfs_inode_put(new_parent);
    return 0;
}
