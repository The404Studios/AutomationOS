/**
 * Virtual File System (VFS) Implementation
 *
 * Minimal working VFS to enable kernel-to-userspace transition
 */

#include "../include/vfs.h"
#include "../include/kernel.h"
#include "../include/mem.h"
#include "../include/string.h"
#include "../include/input.h"  /* vfs_devnode_t / VFS_DEVNODE_MAGIC for device-node dispatch */
#include "../include/sched.h"  /* process_get_current() for per-process fd tables */
#include "../include/page_cache.h"  /* Unified page cache */
#include "../include/fs_registry.h"  /* Filesystem type registry */

// Mount point structure
typedef struct vfs_mount {
    char target[VFS_MAX_PATH];
    vfs_superblock_t* sb;
    uint32_t flags;
    struct vfs_mount* next;
} vfs_mount_t;

// Global VFS state
static struct {
    vfs_mount_t* mounts;
    vfs_file_t* fd_table[VFS_MAX_FDS];
    vfs_inode_t* inode_cache[VFS_INODE_CACHE_SIZE];
    vfs_dentry_t* dentry_cache[VFS_DENTRY_CACHE_SIZE];
    uint64_t next_inode;
    uint32_t initialized;
} vfs_state;

// Forward declarations for ramfs operations
static ssize_t ramfs_read(vfs_file_t* file, void* buf, size_t count);
static ssize_t ramfs_write(vfs_file_t* file, const void* buf, size_t count);
static int ramfs_open(vfs_inode_t* inode, vfs_file_t* file);
static int ramfs_close(vfs_file_t* file);
static off_t ramfs_lseek(vfs_file_t* file, off_t offset, int whence);
static vfs_dentry_t* ramfs_lookup(vfs_inode_t* dir, const char* name);
static int ramfs_create(vfs_inode_t* dir, vfs_dentry_t* dentry, uint32_t mode);
static int ramfs_mkdir(vfs_inode_t* dir, vfs_dentry_t* dentry, uint32_t mode);
static int ramfs_unlink(vfs_inode_t* dir, vfs_dentry_t* dentry);
static int ramfs_rmdir(vfs_inode_t* dir, vfs_dentry_t* dentry);
static vfs_inode_t* vfs_create_file_inode(const char* path, uint32_t mode);

static vfs_file_ops_t ramfs_file_ops = {
    .read = ramfs_read,
    .write = ramfs_write,
    .open = ramfs_open,
    .close = ramfs_close,
    .lseek = ramfs_lseek,
};

static vfs_inode_ops_t ramfs_inode_ops = {
    .lookup = ramfs_lookup,
    .create = ramfs_create,
    .mkdir = ramfs_mkdir,
    .unlink = ramfs_unlink,
    .rmdir = ramfs_rmdir,
};

/* Initial number of dentry slots in a directory's entry array */
#define RAMFS_DIR_INIT_SLOTS 16

/**
 * Ensure a directory inode has an allocated entry array.
 * Returns 0 on success, -1 on failure.
 */
static int ramfs_dir_ensure(vfs_inode_t* dir) {
    if (dir->private_data) {
        return 0;
    }
    dir->private_data = kmalloc(sizeof(vfs_dentry_t*) * RAMFS_DIR_INIT_SLOTS);
    if (!dir->private_data) {
        return -1;
    }
    memset(dir->private_data, 0, sizeof(vfs_dentry_t*) * RAMFS_DIR_INIT_SLOTS);
    dir->data_capacity = RAMFS_DIR_INIT_SLOTS;
    return 0;
}

/**
 * Insert a dentry into a directory inode's entry array, growing it if full.
 * On success the dentry is owned by the directory and dir->size is bumped.
 * Returns 0 on success, -1 on failure (caller still owns the dentry).
 */
static int ramfs_dir_add(vfs_inode_t* dir, vfs_dentry_t* dentry) {
    if (ramfs_dir_ensure(dir) != 0) {
        return -1;
    }

    vfs_dentry_t** entries = (vfs_dentry_t**)dir->private_data;

    // Try to find an empty slot first
    for (uint64_t i = 0; i < dir->data_capacity; i++) {
        if (!entries[i]) {
            entries[i] = dentry;
            dir->size++;
            return 0;
        }
    }

    // No free slot - grow the array (double capacity)
    uint64_t new_cap = dir->data_capacity * 2;
    vfs_dentry_t** new_entries = (vfs_dentry_t**)kmalloc(sizeof(vfs_dentry_t*) * new_cap);
    if (!new_entries) {
        return -1;
    }
    memset(new_entries, 0, sizeof(vfs_dentry_t*) * new_cap);
    memcpy(new_entries, entries, sizeof(vfs_dentry_t*) * dir->data_capacity);

    uint64_t slot = dir->data_capacity;  // first new slot
    new_entries[slot] = dentry;

    kfree(entries);
    dir->private_data = new_entries;
    dir->data_capacity = new_cap;
    dir->size++;
    return 0;
}

/**
 * String utilities
 */
static inline size_t vfs_strlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static inline void vfs_strcpy(char* dst, const char* src, size_t max) {
    size_t i;
    for (i = 0; i < max - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static inline int vfs_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a - *b;
}

static inline int vfs_strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!a[i] || !b[i] || a[i] != b[i]) {
            return a[i] - b[i];
        }
    }
    return 0;
}

// Forward declaration for ramfs registration
static vfs_superblock_t* ramfs_mount(const char* source, uint32_t flags);
static void ramfs_unmount(vfs_superblock_t* sb);

// Filesystem type operations for ramfs
static fs_type_ops_t ramfs_fs_ops = {
    .mount = ramfs_mount,
    .unmount = ramfs_unmount,
    .detect = NULL,  // ramfs doesn't need detection
};

/**
 * Initialize VFS subsystem
 */
void vfs_init(void) {
    kprintf("[VFS] Initializing Virtual File System...\n");

    // Clear all state
    memset(&vfs_state, 0, sizeof(vfs_state));

    // Initialize file descriptor table (0, 1, 2 reserved for stdio)
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        vfs_state.fd_table[i] = NULL;
    }

    // Initialize caches
    for (int i = 0; i < VFS_INODE_CACHE_SIZE; i++) {
        vfs_state.inode_cache[i] = NULL;
    }
    for (int i = 0; i < VFS_DENTRY_CACHE_SIZE; i++) {
        vfs_state.dentry_cache[i] = NULL;
    }

    vfs_state.next_inode = 1;
    vfs_state.mounts = NULL;
    vfs_state.initialized = 1;

    // Initialize unified page cache
    page_cache_init();

    // Initialize filesystem registry
    fs_registry_init();

    // Register built-in ramfs
    fs_register_type("ramfs", &ramfs_fs_ops);

    kprintf("[VFS] Virtual File System initialized\n");
}

/**
 * Allocate inode
 */
vfs_inode_t* vfs_inode_alloc(vfs_superblock_t* sb) {
    vfs_inode_t* inode = (vfs_inode_t*)kmalloc(sizeof(vfs_inode_t));
    if (!inode) {
        return NULL;
    }

    memset(inode, 0, sizeof(vfs_inode_t));
    inode->ino = vfs_state.next_inode++;
    inode->sb = sb;
    inode->ref_count = 1;
    inode->nlink = 1;

    return inode;
}

/**
 * Free inode
 */
void vfs_inode_free(vfs_inode_t* inode) {
    if (!inode) return;

    // Evict all cached pages for this inode
    page_cache_evict_inode(inode);

    // Free private_data for directory inodes
    if ((inode->type & VFS_TYPE_DIR) && inode->private_data) {
        kfree(inode->private_data);
        inode->private_data = NULL;
    }

    // Only free data if it was heap-allocated
    if (inode->data && (inode->flags & VFS_DATA_OWNED)) {
        kfree(inode->data);
        inode->data = NULL;
    }
    // If VFS_DATA_INITRD_BACKED, don't free - it points into initrd

    kfree(inode);
}

/**
 * Increment inode reference count
 */
void vfs_inode_get(vfs_inode_t* inode) {
    if (inode) {
        inode->ref_count++;
    }
}

/**
 * Decrement inode reference count and free if zero
 */
void vfs_inode_put(vfs_inode_t* inode) {
    if (!inode) return;

    if (--inode->ref_count == 0) {
        vfs_inode_free(inode);
    }
}

/**
 * Allocate dentry
 */
vfs_dentry_t* vfs_dentry_alloc(const char* name) {
    vfs_dentry_t* dentry = (vfs_dentry_t*)kmalloc(sizeof(vfs_dentry_t));
    if (!dentry) {
        return NULL;
    }

    memset(dentry, 0, sizeof(vfs_dentry_t));
    vfs_strcpy(dentry->name, name, VFS_MAX_NAME);
    dentry->ref_count = 1;

    return dentry;
}

/**
 * Free dentry
 */
void vfs_dentry_free(vfs_dentry_t* dentry) {
    if (!dentry) return;

    if (dentry->inode) {
        vfs_inode_put(dentry->inode);
    }
    kfree(dentry);
}

/**
 * Allocate file descriptor
 *
 * DESIGN NOTE (per-process fd tables): The current implementation uses a
 * single global fd_table shared across all processes.  This means:
 *   - An fd returned by vfs_open() is visible to every process.
 *   - There is no per-process cleanup on process exit: when process_destroy()
 *     / process_unref() frees a process_t it does NOT close its open fds.
 *     Those entries remain allocated, leaking inodes and their data.
 *
 * Safe fix (without changing process.c or handlers.c): add a function
 *   void vfs_close_all_fds(void)
 * that walks fd_table[3..VFS_MAX_FDS-1] calling vfs_fd_free() on each live
 * entry, and call it from sys_exit() just before schedule() returns.
 * Until then, fd table entries from short-lived processes accumulate and
 * eventually exhaust VFS_MAX_FDS (1024) on a busy system.
 */
// Per-process fd table router: use the CURRENT process's table when one is set;
// fall back to the global table for early-boot / in-kernel opens that happen
// before any process is current. process_t.fd_table is sized == VFS_MAX_FDS.
static vfs_file_t** cur_fdt(void) {
    process_t* p = process_get_current();
    return p ? (vfs_file_t**)p->fd_table : vfs_state.fd_table;
}

int vfs_fd_alloc(void) {
    // Start from 3 (0,1,2 reserved for stdin/stdout/stderr)
    vfs_file_t** t = cur_fdt();
    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (!t[i]) {
            return i;
        }
    }
    return -1;  // No free descriptors
}

/**
 * Free file descriptor
 *
 * BUG-FIX (double-inode-put): vfs_dentry_free() calls vfs_inode_put() on
 * dentry->inode.  If file->inode and dentry->inode point at the same object
 * (which they always do when the dentry is populated via vfs_open), then
 * calling vfs_inode_put(file->inode) BEFORE vfs_dentry_free() drops the
 * refcount twice — the second drop may free the inode while we still hold a
 * pointer to it inside the dentry.  Fix: clear file->inode before freeing the
 * dentry so only the dentry path does the drop when both point to the same
 * inode.  When there is no dentry (the common case), we still put the inode.
 */
void vfs_fd_free(int fd) {
    if (fd >= 0 && fd < VFS_MAX_FDS) {
        vfs_file_t** t = cur_fdt();
        if (t[fd]) {
            vfs_file_t* file = t[fd];

            if (file->dentry) {
                /* dentry's inode == file->inode in the normal case.
                 * Clear file->inode first so we don't double-drop it. */
                file->inode = NULL;
                vfs_dentry_free(file->dentry);
                file->dentry = NULL;
            } else if (file->inode) {
                vfs_inode_put(file->inode);
                file->inode = NULL;
            }

            kfree(file);
            t[fd] = NULL;
        }
    }
}

/**
 * Close every open fd a process still holds (called at process teardown so
 * descriptors don't leak inodes/data). Operates on the GIVEN process's table
 * (not the current one), mirroring vfs_fd_free's inode/dentry drop. fds 0/1/2
 * are stdio handled out-of-band and never live in the table, so start at 3.
 */
void vfs_close_all_fds(struct process* proc) {
    if (!proc) return;
    vfs_file_t** t = (vfs_file_t**)proc->fd_table;
    for (int fd = 3; fd < VFS_MAX_FDS; fd++) {
        vfs_file_t* file = t[fd];
        if (!file) continue;
        if (file->ops && file->ops->close) {
            file->ops->close(file);
        }
        if (file->dentry) {
            file->inode = NULL;            /* avoid double inode_put via dentry */
            vfs_dentry_free(file->dentry);
            file->dentry = NULL;
        } else if (file->inode) {
            vfs_inode_put(file->inode);
            file->inode = NULL;
        }
        kfree(file);
        t[fd] = NULL;
    }
}

/**
 * Get file structure from descriptor
 */
vfs_file_t* vfs_fd_get(int fd) {
    if (fd >= 0 && fd < VFS_MAX_FDS) {
        return cur_fdt()[fd];
    }
    return NULL;
}

/**
 * Find mount point for path
 */
static vfs_mount_t* vfs_find_mount(const char* path) {
    vfs_mount_t* mount = vfs_state.mounts;
    vfs_mount_t* best_match = NULL;
    size_t best_len = 0;

    while (mount) {
        size_t len = vfs_strlen(mount->target);
        if (vfs_strncmp(path, mount->target, len) == 0) {
            /* Require a path boundary after the prefix so that a mount at
             * "/foo" does not wrongly match the path "/foobar".  The char
             * immediately after the matched prefix must be a separator or the
             * end of the path.  A mount target ending in '/' (e.g. "/") is
             * always a valid boundary by itself. */
            char after = path[len];
            int boundary = (after == '\0' || after == '/' ||
                            (len > 0 && mount->target[len - 1] == '/'));
            if (boundary && len > best_len) {
                best_match = mount;
                best_len = len;
            }
        }
        mount = mount->next;
    }

    return best_match;
}

/**
 * Path component lookup
 */
static vfs_inode_t* vfs_lookup_component(vfs_inode_t* dir, const char* name, size_t len) {
    if (!dir || !name || len == 0) {
        return NULL;
    }

    // "." and ".." are normally resolved by vfs_path_lookup before reaching
    // here (so parent tracking stays correct).  Keep a defensive "." fast-path
    // for any other caller: it resolves to the directory itself.
    if (len == 1 && name[0] == '.') {
        vfs_inode_get(dir);
        return dir;
    }

    // Use inode operations if available
    if (dir->ops && dir->ops->lookup) {
        char component[VFS_MAX_NAME];
        if (len >= VFS_MAX_NAME) {
            len = VFS_MAX_NAME - 1;
        }
        memcpy(component, name, len);
        component[len] = '\0';

        vfs_dentry_t* dentry = dir->ops->lookup(dir, component);
        if (dentry && dentry->inode) {
            vfs_inode_get(dentry->inode);
            return dentry->inode;
        }
    }

    return NULL;
}

/**
 * Path lookup - returns inode for path
 *
 * Follows symlinks up to a maximum depth to prevent infinite loops.
 *
 * ".." NAVIGATION BEHAVIOR:
 * This function implements single-level ".." tracking: 'parent' points to the
 * directory one level up from 'current', but grandparent and higher ancestors
 * are not tracked. This means:
 *   - "cd /a/b" followed by "cd .." works (returns to /a)
 *   - "cd /a/b/c" followed by "cd ../.." works (returns to /a)
 *   - But paths like "/a/b/../../.." can only go up as many levels as the
 *     path descended, because each ".." discards the grandparent pointer
 *
 * The underlying issue is that dentry->parent is NULL for all dentries created
 * via ramfs_create/ramfs_mkdir (they receive a parent inode, not a parent
 * dentry, so there's no dentry tree to link into). Until we implement a full
 * dentry cache with inode-to-dentry back-pointers, this manual tracking is the
 * best we can do.
 *
 * For userspace shell navigation, this is sufficient because shells normalize
 * paths client-side (e.g., terminal_m3.c's resolve_path() collapses ".."
 * before calling the kernel), so the kernel never sees deeply nested ".."
 * sequences that would exceed the single-level tracking.
 */
#define MAX_SYMLINK_DEPTH 8

vfs_inode_t* vfs_path_lookup(const char* path) {
    if (!path || path[0] != '/') {
        return NULL;
    }

    // Find mount point
    vfs_mount_t* mount = vfs_find_mount(path);
    if (!mount || !mount->sb || !mount->sb->root) {
        return NULL;
    }

    // Start from root.  'root' is the inode we treat as the top of this mount;
    // ".." at or above it stays put.  'parent' tracks the directory one level
    // up from 'current' so that ".." can navigate upward.
    vfs_inode_t* root = mount->sb->root;
    vfs_inode_t* current = root;
    vfs_inode_get(current);
    vfs_inode_t* parent = NULL;  // no parent above root

    // Skip leading slash
    path++;

    // If just "/", return root
    if (*path == '\0') {
        return current;
    }

    // Symlink resolution depth counter
    int symlink_depth = 0;

    // Parse path components
    while (*path) {
        // Skip slashes
        while (*path == '/') path++;
        if (*path == '\0') break;

        // Find end of component
        const char* end = path;
        while (*end && *end != '/') end++;
        size_t comp_len = (size_t)(end - path);

        // "." refers to the current directory: no movement, parent unchanged.
        if (comp_len == 1 && path[0] == '.') {
            path = end;
            continue;
        }

        // ".." navigates to the parent directory.  At the mount root there is
        // no parent (parent == NULL), so ".." stays at root.
        if (comp_len == 2 && path[0] == '.' && path[1] == '.') {
            if (parent) {
                // Move up: current becomes parent.  We lose the old parent
                // pointer (we don't keep a grandparent), so a subsequent ".."
                // re-derives the parent below as NULL until we descend again.
                vfs_inode_put(current);
                current = parent;     // already holds a reference
                parent = NULL;        // grandparent not tracked
            }
            // else: at root, ".." is a no-op (stays root).
            path = end;
            continue;
        }

        // Ordinary component: descend into it.
        vfs_inode_t* next = vfs_lookup_component(current, path, comp_len);
        if (!next) {
            vfs_inode_put(current);
            if (parent) {
                vfs_inode_put(parent);
            }
            return NULL;
        }

        // Check if the resolved component is a symlink
        if (next->type & VFS_TYPE_SYMLINK) {
            // Prevent infinite symlink loops
            if (++symlink_depth > MAX_SYMLINK_DEPTH) {
                vfs_inode_put(next);
                vfs_inode_put(current);
                if (parent) {
                    vfs_inode_put(parent);
                }
                return NULL;  // Too many symlinks
            }

            // Read symlink target from inode data
            if (!next->data || next->size == 0) {
                // Symlink has no target
                vfs_inode_put(next);
                vfs_inode_put(current);
                if (parent) {
                    vfs_inode_put(parent);
                }
                return NULL;
            }

            // Copy symlink target (ensure null-termination)
            char target[VFS_MAX_PATH];
            size_t target_len = next->size;
            if (target_len >= VFS_MAX_PATH) {
                target_len = VFS_MAX_PATH - 1;
            }
            memcpy(target, next->data, target_len);
            target[target_len] = '\0';

            // Done with the symlink inode
            vfs_inode_put(next);

            // Resolve the symlink target
            vfs_inode_t* symlink_target = NULL;
            if (target[0] == '/') {
                // Absolute symlink: resolve from root
                symlink_target = vfs_path_lookup(target);
            } else {
                // Relative symlink: resolve from current directory
                // Build full path: current directory + '/' + target
                // For simplicity, we'll just fail on relative symlinks for now
                // (proper implementation would reconstruct the full path)
                vfs_inode_put(current);
                if (parent) {
                    vfs_inode_put(parent);
                }
                return NULL;  // Relative symlinks not yet supported
            }

            if (!symlink_target) {
                vfs_inode_put(current);
                if (parent) {
                    vfs_inode_put(parent);
                }
                return NULL;
            }

            // Replace next with the symlink target
            next = symlink_target;
        }

        // 'current' becomes the new parent for the next level.  Release any
        // previous parent reference first.
        if (parent) {
            vfs_inode_put(parent);
        }
        parent = current;
        current = next;
        path = end;
    }

    if (parent) {
        vfs_inode_put(parent);
    }
    return current;
}

/**
 * Open file
 */
int vfs_open(const char* path, int flags, int mode) {
    if (!vfs_state.initialized) {
        return VFS_ERR_INVAL;
    }

    if (!path) {
        return VFS_ERR_INVAL;
    }

    // Lookup inode
    vfs_inode_t* inode = vfs_path_lookup(path);
    if (!inode) {
        // File not found - check if O_CREAT
        if (flags & O_CREAT) {
            inode = vfs_create_file_inode(path, mode);
            if (!inode) {
                return VFS_ERR_NOENT;  // could not create (no parent dir / nomem)
            }
            // vfs_create_file_inode returns a held reference
        } else {
            return VFS_ERR_NOENT;
        }
    } else {
        // File exists - O_EXCL|O_CREAT means fail
        if ((flags & O_CREAT) && (flags & O_EXCL)) {
            vfs_inode_put(inode);
            return VFS_ERR_INVAL;
        }
    }

    // Check if inode is a directory (can't open directories for read/write)
    if (inode->type & VFS_TYPE_DIR) {
        vfs_inode_put(inode);
        return VFS_ERR_ISDIR;
    }

    // Handle O_TRUNC: discard existing contents on a writable open.
    if ((flags & O_TRUNC) &&
        ((flags & O_WRONLY) || (flags & O_RDWR))) {
        if (inode->data && (inode->flags & VFS_DATA_OWNED)) {
            kfree(inode->data);
        }
        // Initrd-backed data is simply detached (not freed) on truncation.
        inode->data = NULL;
        inode->data_capacity = 0;
        inode->size = 0;
        inode->flags &= ~(uint32_t)(VFS_DATA_OWNED | VFS_DATA_INITRD_BACKED);
    }

    // Allocate file descriptor
    int fd = vfs_fd_alloc();
    if (fd < 0) {
        vfs_inode_put(inode);
        return VFS_ERR_NFILE;
    }

    // Allocate file structure
    vfs_file_t* file = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
    if (!file) {
        vfs_inode_put(inode);
        return VFS_ERR_NOMEM;
    }

    memset(file, 0, sizeof(vfs_file_t));
    file->inode = inode;
    file->flags = flags;
    file->mode = mode;
    // O_APPEND: start at end-of-file so writes append.
    file->offset = (flags & O_APPEND) ? inode->size : 0;
    // Device nodes (e.g. /dev/input/eventN) dispatch to their driver's fops;
    // everything else uses the default ramfs file ops.
    if ((inode->type & VFS_TYPE_DEVICE) && inode->private_data &&
        *(uint32_t*)inode->private_data == VFS_DEVNODE_MAGIC) {
        file->ops = ((vfs_devnode_t*)inode->private_data)->fops;
    } else {
        file->ops = &ramfs_file_ops;
    }
    file->ref_count = 1;
    // Initialize read-ahead tracking
    file->ra_last_offset = 0;
    file->ra_window = VFS_READAHEAD_PAGES;
    file->ra_sequential = 0;

    // Call open operation if available
    if (file->ops && file->ops->open) {
        if (file->ops->open(inode, file) < 0) {
            kfree(file);
            vfs_inode_put(inode);
            return VFS_ERR_ACCES;
        }
    }

    cur_fdt()[fd] = file;
    return fd;
}

/**
 * Read from file
 */
ssize_t vfs_read(int fd, void* buf, size_t count) {
    if (!buf && count > 0) {
        return VFS_ERR_INVAL;
    }

    /* BUG-FIX: validate fd range before indexing fd_table. */
    if (fd < 0 || fd >= VFS_MAX_FDS) {
        return VFS_ERR_BADF;
    }

    vfs_file_t* file = vfs_fd_get(fd);
    if (!file) {
        return VFS_ERR_BADF;
    }
    if (!file->ops || !file->ops->read) {
        return VFS_ERR_NOSYS;
    }

    return file->ops->read(file, buf, count);
}

/**
 * Write to file
 */
ssize_t vfs_write(int fd, const void* buf, size_t count) {
    if (!buf && count > 0) {
        return VFS_ERR_INVAL;
    }

    /* BUG-FIX: validate fd range before indexing fd_table. */
    if (fd < 0 || fd >= VFS_MAX_FDS) {
        return VFS_ERR_BADF;
    }

    vfs_file_t* file = vfs_fd_get(fd);
    if (!file) {
        return VFS_ERR_BADF;
    }
    if (!file->ops || !file->ops->write) {
        return VFS_ERR_NOSYS;
    }

    return file->ops->write(file, buf, count);
}

/**
 * Close file
 */
int vfs_close(int fd) {
    /* BUG-FIX: range-check fd BEFORE indexing fd_table.  The old code called
     * vfs_fd_get() first (which does bounds-check internally and returns NULL
     * for out-of-range), then re-checked — but a negative fd passed as int
     * would have already been sign-extended and the second check was dead code
     * placed AFTER the first dereference.  Do the cheap check up front. */
    if (fd < 0 || fd >= VFS_MAX_FDS) {
        return VFS_ERR_BADF;
    }

    vfs_file_t* file = vfs_fd_get(fd);
    if (!file) {
        return VFS_ERR_BADF;
    }

    // Flush dirty pages for this inode
    if (file->inode) {
        page_cache_flush_inode(file->inode);
    }

    // Call close operation if available
    if (file->ops && file->ops->close) {
        file->ops->close(file);
    }

    vfs_fd_free(fd);
    return 0;
}

/**
 * Seek in file
 */
off_t vfs_lseek(int fd, off_t offset, int whence) {
    /* BUG-FIX: validate fd range before indexing fd_table (same class of bug
     * as vfs_close). */
    if (fd < 0 || fd >= VFS_MAX_FDS) {
        return -1;
    }

    vfs_file_t* file = vfs_fd_get(fd);
    if (!file) {
        return -1;
    }

    if (file->ops && file->ops->lseek) {
        return file->ops->lseek(file, offset, whence);
    }

    // Default implementation
    uint64_t new_offset;
    switch (whence) {
        case SEEK_SET:
            if (offset < 0) return -1;  /* negative absolute position */
            new_offset = (uint64_t)offset;
            break;
        case SEEK_CUR:
            /* Guard against underflow (seeking before start). */
            if (offset < 0 && (uint64_t)(-offset) > file->offset) return -1;
            new_offset = file->offset + (uint64_t)offset;
            break;
        case SEEK_END:
            /* Guard against seeking before file start. */
            if (offset < 0 && (uint64_t)(-offset) > file->inode->size) return -1;
            new_offset = file->inode->size + (uint64_t)offset;
            break;
        default:
            return -1;
    }

    file->offset = new_offset;
    return (off_t)file->offset;
}

/**
 * Get file status
 */
int vfs_stat(const char* path, vfs_stat_t* buf) {
    if (!buf) {
        return -1;
    }

    vfs_inode_t* inode = vfs_path_lookup(path);
    if (!inode) {
        return -1;
    }

    memset(buf, 0, sizeof(vfs_stat_t));
    buf->st_ino = inode->ino;
    buf->st_mode = inode->mode;
    buf->st_nlink = inode->nlink;
    buf->st_uid = inode->uid;
    buf->st_gid = inode->gid;
    buf->st_size = inode->size;
    buf->st_blocks = inode->blocks;
    buf->st_atime = inode->atime;
    buf->st_mtime = inode->mtime;
    buf->st_ctime = inode->ctime;

    vfs_inode_put(inode);
    return 0;
}

/**
 * Get file status by descriptor
 */
int vfs_fstat(int fd, vfs_stat_t* buf) {
    if (!buf) {
        return -1;
    }

    vfs_file_t* file = vfs_fd_get(fd);
    if (!file || !file->inode) {
        return -1;
    }

    vfs_inode_t* inode = file->inode;

    memset(buf, 0, sizeof(vfs_stat_t));
    buf->st_ino = inode->ino;
    buf->st_mode = inode->mode;
    buf->st_nlink = inode->nlink;
    buf->st_uid = inode->uid;
    buf->st_gid = inode->gid;
    buf->st_size = inode->size;
    buf->st_blocks = inode->blocks;
    buf->st_atime = inode->atime;
    buf->st_mtime = inode->mtime;
    buf->st_ctime = inode->ctime;

    return 0;
}

/**
 * Truncate file to specified length
 */
int vfs_truncate(const char* path, off_t length) {
    if (!path || length < 0) {
        return VFS_ERR_INVAL;
    }

    vfs_inode_t* inode = vfs_path_lookup(path);
    if (!inode) {
        return VFS_ERR_NOENT;
    }

    // Can't truncate directories
    if (inode->type & VFS_TYPE_DIR) {
        vfs_inode_put(inode);
        return VFS_ERR_ISDIR;
    }

    // Can't truncate symlinks directly
    if (inode->type & VFS_TYPE_SYMLINK) {
        vfs_inode_put(inode);
        return VFS_ERR_INVAL;
    }

    // Flush dirty pages first
    page_cache_flush_inode(inode);

    // Truncate to new length
    if ((uint64_t)length < inode->size) {
        // Shrinking: free excess data
        if (inode->data && (inode->flags & VFS_DATA_OWNED)) {
            // Could realloc to shrink, but for simplicity just truncate in place
            inode->size = (uint64_t)length;
        } else {
            // Initrd-backed or no data: just update size
            inode->size = (uint64_t)length;
        }
    } else if ((uint64_t)length > inode->size) {
        // Growing: extend data
        if (inode->flags & VFS_DATA_OWNED) {
            // Need to grow allocated buffer
            if ((uint64_t)length > inode->data_capacity) {
                void* new_data = kmalloc((size_t)length);
                if (!new_data) {
                    vfs_inode_put(inode);
                    return VFS_ERR_NOMEM;
                }
                // Copy existing data
                if (inode->data && inode->size > 0) {
                    memcpy(new_data, inode->data, inode->size);
                }
                // Zero-fill the extension
                memset((char*)new_data + inode->size, 0, (size_t)length - inode->size);
                // Free old buffer
                if (inode->data) {
                    kfree(inode->data);
                }
                inode->data = new_data;
                inode->data_capacity = (uint64_t)length;
            } else {
                // Fits in existing capacity: zero-fill extension
                memset((char*)inode->data + inode->size, 0, (size_t)length - inode->size);
            }
            inode->size = (uint64_t)length;
        } else {
            // Initrd-backed or no data: allocate new buffer
            void* new_data = kmalloc((size_t)length);
            if (!new_data) {
                vfs_inode_put(inode);
                return VFS_ERR_NOMEM;
            }
            // Copy existing data
            if (inode->data && inode->size > 0) {
                memcpy(new_data, inode->data, inode->size);
            }
            // Zero-fill the extension
            memset((char*)new_data + inode->size, 0, (size_t)length - inode->size);
            // Don't free initrd-backed data
            inode->data = new_data;
            inode->data_capacity = (uint64_t)length;
            inode->size = (uint64_t)length;
            inode->flags |= VFS_DATA_OWNED;
            inode->flags &= ~(uint32_t)VFS_DATA_INITRD_BACKED;
        }
    }

    // Evict now-invalid cached pages beyond new size
    page_cache_evict_inode(inode);

    vfs_inode_put(inode);
    return 0;
}

/**
 * Truncate file via file descriptor
 */
int vfs_ftruncate(int fd, off_t length) {
    if (length < 0) {
        return VFS_ERR_INVAL;
    }

    if (fd < 0 || fd >= VFS_MAX_FDS) {
        return VFS_ERR_BADF;
    }

    vfs_file_t* file = vfs_fd_get(fd);
    if (!file || !file->inode) {
        return VFS_ERR_BADF;
    }

    vfs_inode_t* inode = file->inode;

    // Can't truncate directories
    if (inode->type & VFS_TYPE_DIR) {
        return VFS_ERR_ISDIR;
    }

    // Can't truncate symlinks
    if (inode->type & VFS_TYPE_SYMLINK) {
        return VFS_ERR_INVAL;
    }

    // Check if file is open for writing
    if (!(file->flags & (O_WRONLY | O_RDWR))) {
        return VFS_ERR_INVAL;
    }

    // Flush dirty pages first
    page_cache_flush_inode(inode);

    // Truncate to new length (same logic as vfs_truncate)
    if ((uint64_t)length < inode->size) {
        // Shrinking
        inode->size = (uint64_t)length;
    } else if ((uint64_t)length > inode->size) {
        // Growing
        if (inode->flags & VFS_DATA_OWNED) {
            if ((uint64_t)length > inode->data_capacity) {
                void* new_data = kmalloc((size_t)length);
                if (!new_data) {
                    return VFS_ERR_NOMEM;
                }
                if (inode->data && inode->size > 0) {
                    memcpy(new_data, inode->data, inode->size);
                }
                memset((char*)new_data + inode->size, 0, (size_t)length - inode->size);
                if (inode->data) {
                    kfree(inode->data);
                }
                inode->data = new_data;
                inode->data_capacity = (uint64_t)length;
            } else {
                memset((char*)inode->data + inode->size, 0, (size_t)length - inode->size);
            }
            inode->size = (uint64_t)length;
        } else {
            void* new_data = kmalloc((size_t)length);
            if (!new_data) {
                return VFS_ERR_NOMEM;
            }
            if (inode->data && inode->size > 0) {
                memcpy(new_data, inode->data, inode->size);
            }
            memset((char*)new_data + inode->size, 0, (size_t)length - inode->size);
            inode->data = new_data;
            inode->data_capacity = (uint64_t)length;
            inode->size = (uint64_t)length;
            inode->flags |= VFS_DATA_OWNED;
            inode->flags &= ~(uint32_t)VFS_DATA_INITRD_BACKED;
        }
    }

    // Evict cached pages beyond new size
    page_cache_evict_inode(inode);

    return 0;
}

/**
 * Sync file descriptor - flush dirty pages to storage
 */
int vfs_fsync(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS) {
        return VFS_ERR_BADF;
    }

    vfs_file_t* file = vfs_fd_get(fd);
    if (!file || !file->inode) {
        return VFS_ERR_BADF;
    }

    // Flush dirty pages for this inode
    return page_cache_flush_inode(file->inode);
}

/**
 * Sync all filesystems - flush all dirty pages
 */
int vfs_sync(void) {
    // Flush all dirty pages in the page cache
    int result = page_cache_flush_all();

    // Could also call sync_fs on all mounted filesystems if they support it
    vfs_mount_t* mount = vfs_state.mounts;
    while (mount) {
        if (mount->sb && mount->sb->sync_fs) {
            mount->sb->sync_fs(mount->sb);
        }
        mount = mount->next;
    }

    return result;
}

/**
 * Mount filesystem
 */
int vfs_mount(const char* source, const char* target, const char* fstype) {
    kprintf("[VFS] Mounting %s on %s (type: %s)\n", source, target, fstype);

    vfs_superblock_t* sb = NULL;

    // Look up filesystem type in registry
    fs_type_t* fs_type = fs_lookup_type(fstype);

    if (fs_type && fs_type->ops && fs_type->ops->mount) {
        // Use registered filesystem driver
        sb = fs_type->ops->mount(source, 0);
    } else if (vfs_strcmp(fstype, "ramfs") == 0) {
        // Fallback to built-in ramfs if not registered
        sb = vfs_create_ramfs();
    } else {
        kprintf("[VFS] Unsupported filesystem type: %s\n", fstype);
        return -1;
    }

    if (!sb) {
        kprintf("[VFS] Failed to create superblock for %s\n", fstype);
        return -1;
    }

    // Create mount point
    vfs_mount_t* mount = (vfs_mount_t*)kmalloc(sizeof(vfs_mount_t));
    if (!mount) {
        kfree(sb);
        return -1;
    }

    vfs_strcpy(mount->target, target, VFS_MAX_PATH);
    mount->sb = sb;
    mount->flags = 0;
    mount->next = vfs_state.mounts;
    vfs_state.mounts = mount;

    kprintf("[VFS] Mounted %s successfully\n", target);
    return 0;
}

/**
 * Create directory (single level)
 */
int vfs_mkdir(const char* path, uint32_t mode) {
    // Find parent directory
    char parent_path[VFS_MAX_PATH];
    const char* name;

    // Find last slash
    const char* last_slash = NULL;
    for (const char* p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    if (!last_slash) {
        return -1;
    }

    // Copy parent path
    size_t parent_len = last_slash - path;
    if (parent_len == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
    }

    name = last_slash + 1;

    // Lookup parent
    vfs_inode_t* parent = vfs_path_lookup(parent_path);
    if (!parent) {
        return -1;
    }

    // Create directory
    int result = -1;
    if (parent->ops && parent->ops->mkdir) {
        vfs_dentry_t* dentry = vfs_dentry_alloc(name);
        if (dentry) {
            result = parent->ops->mkdir(parent, dentry, mode);
            if (result < 0) {
                vfs_dentry_free(dentry);
            }
        }
    }

    vfs_inode_put(parent);
    return result;
}

/**
 * Create directory recursively
 */
int vfs_mkdir_recursive(const char* path, uint32_t mode) {
    char tmp[VFS_MAX_PATH];
    char* p = NULL;
    size_t len;

    vfs_strcpy(tmp, path, VFS_MAX_PATH);
    len = vfs_strlen(tmp);

    if (len == 0) {
        return -1;   // empty path -> avoid tmp[-1] underread below
    }
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    // Create each component
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            vfs_mkdir(tmp, mode);  // Ignore errors for existing dirs
            *p = '/';
        }
    }

    return vfs_mkdir(tmp, mode);
}

//
// RAMFS Operations
//

/**
 * RAMFS: Read file (with page cache acceleration)
 */
static ssize_t ramfs_read(vfs_file_t* file, void* buf, size_t count) {
    if (!file || !file->inode || !buf) {
        return -1;
    }

    /* NOTE: the page cache (page_cache_read) was found to corrupt file data
     * (write-then-read content mismatch) and slow the boot; until it is fixed,
     * regular files use the direct, proven-correct memcpy path below. */
    vfs_inode_t* inode = file->inode;

    // Check bounds
    if (file->offset >= inode->size) {
        return 0;  // EOF
    }

    /* BUG-FIX (integer overflow in read): the old code computed
     *   file->offset + bytes_to_read > inode->size
     * without guarding against overflow.  If file->offset is near UINT64_MAX
     * the addition wraps and we'd copy 'count' bytes past EOF.  Compute the
     * available byte count first using subtraction (safe because we already
     * checked file->offset < inode->size), then clamp count to that. */
    uint64_t available = inode->size - file->offset;
    size_t bytes_to_read = count;
    if ((uint64_t)bytes_to_read > available) {
        bytes_to_read = (size_t)available;
    }

    // Copy data
    if (inode->data) {
        memcpy(buf, (uint8_t*)inode->data + file->offset, bytes_to_read);
    }

    file->offset += bytes_to_read;
    return (ssize_t)bytes_to_read;
}

/**
 * RAMFS: Write file (with page cache acceleration)
 *
 * Grows the inode's data buffer as needed. If the file's backing store is
 * initrd memory (read-only, zero-copy) the data is first copied into a fresh
 * heap buffer (copy-on-write) so we never scribble over the initrd image.
 */
static ssize_t ramfs_write(vfs_file_t* file, const void* buf, size_t count) {
    if (!file || !file->inode || !buf) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }

    /* NOTE: page_cache_write corrupts data + slows boot (see ramfs_read); use
     * the direct, proven-correct path below until the page cache is fixed. */
    vfs_inode_t* inode = file->inode;

    // Append mode always writes at end-of-file.
    if (file->flags & O_APPEND) {
        file->offset = inode->size;
    }

    /* BUG-FIX (integer overflow in write): the original cast
     *   size_t required = (size_t)file->offset + count
     * can overflow when file->offset is near SIZE_MAX.  If required wraps to
     * a small value, new_capacity = required * 2 is also tiny, kmalloc
     * succeeds, and the memcpy at the end writes 'count' bytes off the end of
     * the allocation — heap corruption.  Guard with an explicit overflow check
     * and a hard cap so userspace cannot allocate more than 256 MiB per file. */
#define RAMFS_MAX_FILE_SIZE ((uint64_t)(256ULL * 1024 * 1024))
    if (file->offset > RAMFS_MAX_FILE_SIZE ||
        count > RAMFS_MAX_FILE_SIZE ||
        file->offset + (uint64_t)count > RAMFS_MAX_FILE_SIZE) {
        return -1;  /* would overflow or exceed per-file cap */
    }

    // Calculate required size (offset + count).  Safe: overflow checked above.
    uint64_t required64 = file->offset + (uint64_t)count;
    size_t required = (size_t)required64;

    // Copy-on-write for initrd-backed (read-only) inodes, or grow the heap
    // buffer if the current capacity is insufficient.
    int initrd_backed = (inode->flags & VFS_DATA_INITRD_BACKED) ? 1 : 0;

    if (initrd_backed || required > inode->data_capacity) {
        size_t new_capacity = required * 2;  // grow with headroom
        if (new_capacity < 16) {
            new_capacity = 16;
        }
        uint8_t* new_data = (uint8_t*)kmalloc(new_capacity);
        if (!new_data) {
            return -1;
        }
        memset(new_data, 0, new_capacity);

        // Preserve any existing contents (handles sparse gaps too).
        if (inode->data && inode->size > 0) {
            size_t copy = inode->size;
            if (copy > new_capacity) {
                copy = new_capacity;
            }
            memcpy(new_data, inode->data, copy);
        }

        // Free the old heap buffer; leave initrd-backed memory untouched.
        if (inode->data && (inode->flags & VFS_DATA_OWNED)) {
            kfree(inode->data);
        }

        inode->data = new_data;
        inode->data_capacity = new_capacity;
        inode->flags |= VFS_DATA_OWNED;
        inode->flags &= ~(uint32_t)VFS_DATA_INITRD_BACKED;
    }

    // Write data.
    memcpy((uint8_t*)inode->data + file->offset, buf, count);
    file->offset += count;

    // Update size if the write extended the file.
    if (file->offset > inode->size) {
        inode->size = file->offset;
    }

    return (ssize_t)count;
}

/**
 * RAMFS: Open file
 */
static int ramfs_open(vfs_inode_t* inode, vfs_file_t* file) {
    // Nothing special to do
    return 0;
}

/**
 * RAMFS: Close file
 */
static int ramfs_close(vfs_file_t* file) {
    // Nothing special to do
    return 0;
}

/**
 * RAMFS: Seek in file
 */
static off_t ramfs_lseek(vfs_file_t* file, off_t offset, int whence) {
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
 * RAMFS: Lookup entry in directory
 *
 * BUG-FIX (sparse-array / invisible entries after unlink):
 * The directory entry array is a sparse array: vfs_unlink() sets a slot to
 * NULL and decrements dir->size, but does NOT compact the array.  Using
 * dir->size as the loop bound means that after any unlink the scan stops
 * before reaching live entries whose indices are now beyond dir->size.
 * Fix: scan the full allocated capacity (dir->data_capacity) and skip NULL
 * slots.  This is O(capacity) in the worst case, which is bounded and fine for
 * an in-memory FS.
 */
static vfs_dentry_t* ramfs_lookup(vfs_inode_t* dir, const char* name) {
    if (!dir || !name || !(dir->type & VFS_TYPE_DIR)) {
        return NULL;
    }

    // Directory data is an array of dentries
    if (!dir->private_data) {
        return NULL;
    }

    vfs_dentry_t** entries = (vfs_dentry_t**)dir->private_data;
    for (uint64_t i = 0; i < dir->data_capacity; i++) {
        if (entries[i] && vfs_strcmp(entries[i]->name, name) == 0) {
            return entries[i];
        }
    }

    return NULL;
}

/**
 * RAMFS: Create file in directory
 *
 * Creates a new empty regular-file inode, attaches it to the supplied
 * dentry, and links the dentry into the directory's entry array.
 */
static int ramfs_create(vfs_inode_t* dir, vfs_dentry_t* dentry, uint32_t mode) {
    if (!dir || !dentry || !(dir->type & VFS_TYPE_DIR)) {
        return -1;
    }

    // Reject duplicate names so we don't shadow an existing entry.
    if (ramfs_lookup(dir, dentry->name)) {
        return -1;
    }

    // Create inode
    vfs_inode_t* inode = vfs_inode_alloc(dir->sb);
    if (!inode) {
        return -1;
    }

    inode->mode = mode;
    inode->type = VFS_TYPE_FILE;
    inode->ops = &ramfs_inode_ops;
    inode->size = 0;
    inode->data = NULL;
    inode->data_capacity = 0;

    dentry->inode = inode;

    // Link the dentry into the parent directory.
    if (ramfs_dir_add(dir, dentry) != 0) {
        dentry->inode = NULL;
        vfs_inode_free(inode);
        return -1;
    }

    /* Set up parent tracking: we don't have the parent dentry pointer here
     * (ramfs_create is called with a parent inode, not a parent dentry), so
     * we cannot establish a proper dentry tree link at this level. The parent
     * pointer remains NULL, which means ".." navigation will be limited to
     * the vfs_path_lookup() manual parent tracking (one level only).
     *
     * A complete fix would require:
     *   1. Maintaining a dentry for every directory inode (including root)
     *   2. Passing parent_dentry instead of parent_inode to create/mkdir ops
     *   3. Storing a back-pointer from inode to its primary dentry
     * This is deferred until the dentry cache is fully implemented. */
    dentry->parent = NULL;

    return 0;
}

/**
 * Create a regular file at an absolute path and return a *held* reference
 * to its inode. Used by vfs_open(O_CREAT). Returns NULL on failure
 * (e.g. parent directory missing, out of memory, or name already exists).
 */
static vfs_inode_t* vfs_create_file_inode(const char* path, uint32_t mode) {
    if (!path || path[0] != '/') {
        return NULL;
    }

    // Split into parent path + final component.
    const char* last_slash = NULL;
    for (const char* p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }
    if (!last_slash) {
        return NULL;
    }

    const char* name = last_slash + 1;
    if (*name == '\0') {
        return NULL;  // trailing slash - not a file
    }

    char parent_path[VFS_MAX_PATH];
    size_t parent_len = (size_t)(last_slash - path);
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

    vfs_inode_t* parent = vfs_path_lookup(parent_path);
    if (!parent) {
        return NULL;
    }
    if (!(parent->type & VFS_TYPE_DIR) || !parent->ops || !parent->ops->create) {
        vfs_inode_put(parent);
        return NULL;
    }

    vfs_dentry_t* dentry = vfs_dentry_alloc(name);
    if (!dentry) {
        vfs_inode_put(parent);
        return NULL;
    }

    if (parent->ops->create(parent, dentry, mode) != 0) {
        vfs_dentry_free(dentry);
        vfs_inode_put(parent);
        return NULL;
    }

    // The directory now owns the dentry/inode; hand back a held reference.
    vfs_inode_t* inode = dentry->inode;
    vfs_inode_get(inode);

    vfs_inode_put(parent);
    return inode;
}

/**
 * RAMFS: Create directory
 */
static int ramfs_mkdir(vfs_inode_t* dir, vfs_dentry_t* dentry, uint32_t mode) {
    if (!dir || !dentry || !(dir->type & VFS_TYPE_DIR)) {
        return -1;
    }

    // Reject duplicate names (e.g. mkdir of an already-existing directory).
    if (ramfs_lookup(dir, dentry->name)) {
        return -1;
    }

    // Create inode
    vfs_inode_t* inode = vfs_inode_alloc(dir->sb);
    if (!inode) {
        return -1;
    }

    inode->mode = mode;
    inode->type = VFS_TYPE_DIR;
    inode->ops = &ramfs_inode_ops;

    // Allocate directory entries array for the new directory.
    if (ramfs_dir_ensure(inode) != 0) {
        vfs_inode_free(inode);
        return -1;
    }

    dentry->inode = inode;

    // Link into parent directory (grows the parent array if needed).
    if (ramfs_dir_add(dir, dentry) != 0) {
        dentry->inode = NULL;
        vfs_inode_free(inode);
        return -1;
    }

    /* Set up parent tracking: we don't have the parent dentry pointer here
     * (ramfs_mkdir is called with a parent inode, not a parent dentry), so
     * we cannot establish a proper dentry tree link at this level. The parent
     * pointer remains NULL, which means ".." navigation will be limited to
     * the vfs_path_lookup() manual parent tracking (one level only).
     *
     * A complete fix would require:
     *   1. Maintaining a dentry for every directory inode (including root)
     *   2. Passing parent_dentry instead of parent_inode to create/mkdir ops
     *   3. Storing a back-pointer from inode to its primary dentry
     * This is deferred until the dentry cache is fully implemented. */
    dentry->parent = NULL;

    return 0;
}

/**
 * RAMFS: Unlink file from directory
 *
 * Removes a file (not directory) from the parent directory by setting the
 * dentry slot to NULL and decrementing the directory size. The dentry is
 * NOT freed here - the caller owns it and must free it.
 */
static int ramfs_unlink(vfs_inode_t* dir, vfs_dentry_t* dentry) {
    if (!dir || !dentry || !(dir->type & VFS_TYPE_DIR)) {
        return -1;
    }

    // Can't unlink directories with unlink (use rmdir instead)
    if (dentry->inode && (dentry->inode->type & VFS_TYPE_DIR)) {
        return -1;
    }

    // Find and remove the dentry from the directory's entry array
    if (!dir->private_data) {
        return -1;
    }

    vfs_dentry_t** entries = (vfs_dentry_t**)dir->private_data;
    for (uint64_t i = 0; i < dir->data_capacity; i++) {
        if (entries[i] == dentry) {
            entries[i] = NULL;
            dir->size--;
            return 0;
        }
    }

    // Dentry not found in this directory
    return -1;
}

/**
 * RAMFS: Remove directory
 *
 * Removes an empty directory from the parent directory. The directory must
 * be empty (size == 0) to be removed. The dentry is NOT freed here - the
 * caller owns it and must free it.
 */
static int ramfs_rmdir(vfs_inode_t* dir, vfs_dentry_t* dentry) {
    if (!dir || !dentry || !(dir->type & VFS_TYPE_DIR)) {
        return -1;
    }

    // Can only rmdir directories
    if (!dentry->inode || !(dentry->inode->type & VFS_TYPE_DIR)) {
        return -1;
    }

    // Directory must be empty (size == 0)
    if (dentry->inode->size != 0) {
        return -1;  // Directory not empty
    }

    // Find and remove the dentry from the directory's entry array
    if (!dir->private_data) {
        return -1;
    }

    vfs_dentry_t** entries = (vfs_dentry_t**)dir->private_data;
    for (uint64_t i = 0; i < dir->data_capacity; i++) {
        if (entries[i] == dentry) {
            entries[i] = NULL;
            dir->size--;
            return 0;
        }
    }

    // Dentry not found in this directory
    return -1;
}

/**
 * Create RAMFS superblock
 */
vfs_superblock_t* vfs_create_ramfs(void) {
    vfs_superblock_t* sb = (vfs_superblock_t*)kmalloc(sizeof(vfs_superblock_t));
    if (!sb) {
        return NULL;
    }

    memset(sb, 0, sizeof(vfs_superblock_t));
    sb->magic = 0x524146534d454d;  // "RAMFS" in hex
    sb->blocksize = 4096;
    sb->type = "ramfs";

    // Create root inode
    vfs_inode_t* root = vfs_inode_alloc(sb);
    if (!root) {
        kfree(sb);
        return NULL;
    }

    root->mode = 0755;
    root->type = VFS_TYPE_DIR;
    root->ops = &ramfs_inode_ops;

    // Allocate directory entries array
    root->private_data = kmalloc(sizeof(vfs_dentry_t*) * 16);
    if (!root->private_data) {
        vfs_inode_free(root);
        kfree(sb);
        return NULL;
    }
    memset(root->private_data, 0, sizeof(vfs_dentry_t*) * 16);
    root->data_capacity = 16;

    sb->root = root;

    return sb;
}

/**
 * ramfs mount wrapper for filesystem registry
 */
static vfs_superblock_t* ramfs_mount(const char* source, uint32_t flags) {
    return vfs_create_ramfs();
}

/**
 * ramfs unmount wrapper for filesystem registry
 */
static void ramfs_unmount(vfs_superblock_t* sb) {
    if (sb) {
        if (sb->root) {
            vfs_inode_free(sb->root);
        }
        kfree(sb);
    }
}

/**
 * RAMFS: Create file with data
 */
int vfs_ramfs_create_file(vfs_inode_t* dir, const char* name,
                          const void* data, size_t size, uint32_t mode) {
    if (!dir || !name || !(dir->type & VFS_TYPE_DIR)) {
        return -1;
    }

    // Create dentry
    vfs_dentry_t* dentry = vfs_dentry_alloc(name);
    if (!dentry) {
        return -1;
    }

    // Create inode
    vfs_inode_t* inode = vfs_inode_alloc(dir->sb);
    if (!inode) {
        vfs_dentry_free(dentry);
        return -1;
    }

    inode->mode = mode;
    inode->type = VFS_TYPE_FILE;
    inode->ops = &ramfs_inode_ops;
    inode->size = size;

    // Copy data
    if (data && size > 0) {
        inode->data = kmalloc(size);
        if (!inode->data) {
            vfs_inode_free(inode);
            vfs_dentry_free(dentry);
            return -1;
        }
        memcpy(inode->data, data, size);
        inode->data_capacity = size;
        inode->flags |= VFS_DATA_OWNED;  // Mark data as heap-allocated
    }

    dentry->inode = inode;

    // Link into directory (grows the entry array if full).
    if (ramfs_dir_add(dir, dentry) != 0) {
        dentry->inode = NULL;
        vfs_inode_free(inode);
        vfs_dentry_free(dentry);
        return -1;
    }

    return 0;
}

/**
 * RAMFS: Create file with initrd-backed data (zero-copy)
 *
 * Creates a file that points directly into initrd memory without copying.
 * The data pointer must remain valid for the lifetime of the file.
 */
int vfs_ramfs_create_file_initrd(vfs_inode_t* dir, const char* name,
                                  const void* data, size_t size, uint32_t mode) {
    if (!dir || !name || !(dir->type & VFS_TYPE_DIR)) {
        return -1;
    }

    // Create dentry
    vfs_dentry_t* dentry = vfs_dentry_alloc(name);
    if (!dentry) {
        return -1;
    }

    // Create inode
    vfs_inode_t* inode = vfs_inode_alloc(dir->sb);
    if (!inode) {
        vfs_dentry_free(dentry);
        return -1;
    }

    inode->mode = mode;
    inode->type = VFS_TYPE_FILE;
    inode->ops = &ramfs_inode_ops;
    inode->size = size;

    // Point directly to initrd data (zero-copy)
    if (data && size > 0) {
        inode->data = (void*)data;  // Direct pointer into initrd
        inode->data_capacity = size;
        inode->flags |= VFS_DATA_INITRD_BACKED;  // Mark as initrd-backed, don't free
    }

    dentry->inode = inode;

    // Link into directory (grows the entry array if full).
    if (ramfs_dir_add(dir, dentry) != 0) {
        dentry->inode = NULL;
        vfs_inode_free(inode);
        vfs_dentry_free(dentry);
        return -1;
    }

    return 0;
}

/**
 * RAMFS: Create directory
 */
int vfs_ramfs_create_dir(vfs_inode_t* dir, const char* name, uint32_t mode) {
    vfs_dentry_t* dentry = vfs_dentry_alloc(name);
    if (!dentry) {
        return -1;
    }

    return ramfs_mkdir(dir, dentry, mode);
}

/**
 * Filesystem post-mount initialization.
 *
 * Creates standard writable directories in the (already-mounted) root ramfs.
 * Safe to call multiple times: vfs_mkdir on an existing directory is ignored.
 *
 * The integrator should call this once after vfs_mount("/", ...) and
 * initrd_mount() have run (e.g. right after the /dev/pts setup in kernel.c).
 * Without it, userspace can still create files anywhere a parent directory
 * already exists, but /tmp won't be present unless the initrd shipped it.
 */
void vfs_fs_init(void) {
    // Writable scratch directories. The root ramfs is fully writable, so these
    // behave like a tmpfs mounted at each path.
    vfs_mkdir("/tmp", 0777);
    vfs_mkdir("/var", 0755);
    vfs_mkdir("/var/tmp", 0777);
    vfs_mkdir("/run", 0755);
    kprintf("[VFS] Writable directories ready: /tmp /var/tmp /run\n");
}

/**
 * Unmount filesystem
 *
 * Removes the mount from vfs_state.mounts, calls the filesystem-specific
 * unmount handler (which frees the superblock, root inode, and any fs-specific
 * data like ext2's group_desc and superblock buffer), and prevents leaks.
 *
 * Returns 0 on success, -1 on failure (target not mounted, or VFS not initialized).
 */
int vfs_unmount(const char* target) {
    if (!vfs_state.initialized || !target) {
        return -1;
    }

    kprintf("[VFS] Unmounting %s\n", target);

    // Find the mount point in the linked list
    vfs_mount_t** prev_ptr = &vfs_state.mounts;
    vfs_mount_t* mount = vfs_state.mounts;

    while (mount) {
        if (vfs_strcmp(mount->target, target) == 0) {
            // Found the mount - unlink it from the list
            *prev_ptr = mount->next;

            vfs_superblock_t* sb = mount->sb;

            // Look up the filesystem type to call its unmount handler
            if (sb && sb->type) {
                fs_type_t* fs_type = fs_lookup_type(sb->type);
                if (fs_type && fs_type->ops && fs_type->ops->unmount) {
                    // Filesystem-specific unmount (frees sb, root inode, fs_data, etc.)
                    fs_type->ops->unmount(sb);
                } else {
                    // No registered unmount handler - fall back to manual cleanup
                    if (sb->root) {
                        vfs_inode_put(sb->root);
                    }
                    kfree(sb);
                }
            }

            kfree(mount);
            kprintf("[VFS] Unmounted %s successfully\n", target);
            return 0;
        }
        prev_ptr = &mount->next;
        mount = mount->next;
    }

    kprintf("[VFS] Unmount failed: %s not mounted\n", target);
    return -1;
}

/**
 * Remove directory
 *
 * Removes an empty directory at the given path. The directory must exist,
 * be a directory, and be empty (contain no entries). Returns 0 on success,
 * -1 on failure.
 */
int vfs_rmdir(const char* path) {
    if (!path) {
        return -1;
    }

    // Find parent directory and directory name
    char parent_path[VFS_MAX_PATH];
    const char* dirname;

    // Find last slash
    const char* last_slash = NULL;
    for (const char* p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }

    if (!last_slash) {
        return -1;
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

    dirname = last_slash + 1;
    if (*dirname == '\0') {
        return -1;  // Can't remove directory with trailing slash
    }

    // Lookup parent directory
    vfs_inode_t* parent = vfs_path_lookup(parent_path);
    if (!parent) {
        return -1;
    }

    if (!(parent->type & VFS_TYPE_DIR)) {
        vfs_inode_put(parent);
        return -1;
    }

    // Find the entry in parent directory
    if (!parent->private_data) {
        vfs_inode_put(parent);
        return -1;
    }

    vfs_dentry_t** entries = (vfs_dentry_t**)parent->private_data;
    int found = -1;

    for (uint64_t i = 0; i < parent->data_capacity; i++) {
        if (entries[i] && vfs_strcmp(entries[i]->name, dirname) == 0) {
            found = i;
            break;
        }
    }

    if (found < 0) {
        vfs_inode_put(parent);
        return -1;  // Directory not found
    }

    vfs_dentry_t* dentry = entries[found];

    // Must be a directory
    if (!dentry->inode || !(dentry->inode->type & VFS_TYPE_DIR)) {
        vfs_inode_put(parent);
        return -1;
    }

    // Directory must be empty
    if (dentry->inode->size != 0) {
        vfs_inode_put(parent);
        return -1;  // Directory not empty
    }

    // Use inode operations if available
    if (parent->ops && parent->ops->rmdir) {
        if (parent->ops->rmdir(parent, dentry) != 0) {
            vfs_inode_put(parent);
            return -1;
        }
    } else {
        // Fallback: manually remove from parent directory
        entries[found] = NULL;
        parent->size--;
    }

    // Free the dentry; vfs_dentry_free() drops the inode's reference (and frees
    // the inode if this was the last link).
    vfs_dentry_free(dentry);

    vfs_inode_put(parent);
    return 0;
}

/**
 * Lookup dentry in directory
 */
vfs_dentry_t* vfs_dentry_lookup(vfs_inode_t* dir, const char* name) {
    if (!dir || !name || !(dir->type & VFS_TYPE_DIR)) {
        return NULL;
    }
    return ramfs_lookup(dir, name);
}

/**
 * Add child dentry to parent
 *
 * Establishes the parent-child relationship in the dentry tree so that ".."
 * navigation works correctly. This function is called AFTER the child dentry
 * has been linked into the parent directory's entry array.
 *
 * The parent pointer is NOT reference-counted: the child dentry's lifetime is
 * tied to its presence in the parent's entry array. When the child is unlinked
 * (vfs_unlink/vfs_rmdir), the parent pointer becomes stale, but the dentry is
 * freed immediately afterward so there's no dangling-pointer window.
 */
void vfs_dentry_add_child(vfs_dentry_t* parent, vfs_dentry_t* child) {
    if (!parent || !child) {
        return;
    }

    /* Validation: parent must point to a directory inode */
    if (!parent->inode || !(parent->inode->type & VFS_TYPE_DIR)) {
        kprintf("[VFS] Warning: vfs_dentry_add_child called with non-directory parent\n");
        return;
    }

    /* Set the child's parent pointer (not reference-counted) */
    child->parent = parent;
}
