/**
 * AutomationOS - PTY Device Nodes
 *
 * VFS integration for PTY devices:
 *  - /dev/ptmx - PTY master allocator (opens new PTY pair)
 *  - /dev/pts/N - PTY slave devices (N = 0 to 31)
 */

#include "../../include/types.h"
#include "../../include/kernel.h"
#include "../../include/vfs.h"
#include "../../include/mem.h"
#include "../../include/string.h"
#include "../../include/errno.h"   /* canonical EFAULT (negative) */
#include "pty.h"

// PTY file private data
typedef struct {
    uint32_t pty_index;
    bool is_master;
} pty_file_data_t;

// Forward declarations
static ssize_t ptmx_read(vfs_file_t *file, void *buf, size_t count);
static ssize_t ptmx_write(vfs_file_t *file, const void *buf, size_t count);
static int ptmx_open(vfs_inode_t *inode, vfs_file_t *file);
static int ptmx_close(vfs_file_t *file);
static off_t ptmx_lseek(vfs_file_t *file, off_t offset, int whence);

static ssize_t pts_read(vfs_file_t *file, void *buf, size_t count);
static ssize_t pts_write(vfs_file_t *file, const void *buf, size_t count);
static int pts_open(vfs_inode_t *inode, vfs_file_t *file);
static int pts_close(vfs_file_t *file);
static off_t pts_lseek(vfs_file_t *file, off_t offset, int whence);

// File operations for /dev/ptmx (master)
static vfs_file_ops_t ptmx_ops = {
    .read = ptmx_read,
    .write = ptmx_write,
    .open = ptmx_open,
    .close = ptmx_close,
    .lseek = ptmx_lseek,
};

// File operations for /dev/pts/* (slaves)
static vfs_file_ops_t pts_ops = {
    .read = pts_read,
    .write = pts_write,
    .open = pts_open,
    .close = pts_close,
    .lseek = pts_lseek,
};

/**
 * Initialize PTY device nodes in VFS
 * Creates /dev/ptmx and /dev/pts/ directory
 */
int pty_dev_init(vfs_inode_t *dev_root) {
    if (!dev_root) {
        return -1;
    }

    kprintf("[PTY] Creating device nodes...\n");

    // Create /dev/ptmx (master allocator)
    vfs_dentry_t *ptmx_dentry = vfs_dentry_alloc("ptmx");
    if (!ptmx_dentry) {
        return -1;
    }

    vfs_inode_t *ptmx_inode = vfs_inode_alloc(dev_root->sb);
    if (!ptmx_inode) {
        vfs_dentry_free(ptmx_dentry);
        return -1;
    }

    ptmx_inode->type = VFS_TYPE_DEVICE;
    ptmx_inode->mode = 0666;  // rw-rw-rw-
    ptmx_inode->ops = &ptmx_ops;
    ptmx_dentry->inode = ptmx_inode;
    /* NOTE: dev_root->private_data is a vfs_dentry_t** child ARRAY (the inode's
     * dentry table), NOT a vfs_dentry_t. The old vfs_dentry_add_child() call cast
     * it to a parent dentry -> type confusion / wild read of *(array) as a dentry.
     * Removed. When this (currently dead) path is integrated, register ptmx via a
     * real dentry parent + child-array insert (see logic-item audit). */

    // Create /dev/pts/ directory
    vfs_dentry_t *pts_dir_dentry = vfs_dentry_alloc("pts");
    if (!pts_dir_dentry) {
        return -1;
    }

    vfs_inode_t *pts_dir_inode = vfs_inode_alloc(dev_root->sb);
    if (!pts_dir_inode) {
        vfs_dentry_free(pts_dir_dentry);
        return -1;
    }

    pts_dir_inode->type = VFS_TYPE_DIR;
    pts_dir_inode->mode = 0755;  // rwxr-xr-x
    pts_dir_inode->private_data = kmalloc(sizeof(vfs_dentry_t *) * 32);  // Will hold slave dentries
    if (!pts_dir_inode->private_data) {
        vfs_inode_free(pts_dir_inode);
        vfs_dentry_free(pts_dir_dentry);
        return -1;
    }
    memset(pts_dir_inode->private_data, 0, sizeof(vfs_dentry_t *) * 32);

    pts_dir_dentry->inode = pts_dir_inode;
    /* Same type confusion as the ptmx case above: dev_root->private_data is a
     * dentry ARRAY, not a parent dentry. Removed the bad cast/call. */

    kprintf("[PTY] Created /dev/ptmx and /dev/pts/\n");
    return 0;
}

/**
 * Create a PTY slave device node /dev/pts/N
 * Called when a new PTY is allocated
 */
int pty_dev_create_slave(vfs_inode_t *pts_dir, uint32_t index) {
    if (!pts_dir || index >= 32) {
        return -1;
    }

    // Check if already exists
    vfs_dentry_t **slaves = (vfs_dentry_t **)pts_dir->private_data;
    if (slaves[index]) {
        return 0;  // Already exists
    }

    // Create device name (e.g., "0", "1", etc.)
    char name[8];
    snprintf(name, sizeof(name), "%u", index);

    vfs_dentry_t *slave_dentry = vfs_dentry_alloc(name);
    if (!slave_dentry) {
        return -1;
    }

    vfs_inode_t *slave_inode = vfs_inode_alloc(pts_dir->sb);
    if (!slave_inode) {
        vfs_dentry_free(slave_dentry);
        return -1;
    }

    slave_inode->type = VFS_TYPE_DEVICE;
    slave_inode->mode = 0620;  // rw--w---- (owner + group writable)
    slave_inode->ops = &pts_ops;
    slave_inode->private_data = (void *)(uintptr_t)index;  // Store PTY index
    slave_dentry->inode = slave_inode;

    slaves[index] = slave_dentry;  /* correct registration: slaves[] IS pts_dir->private_data */
    /* Removed: vfs_dentry_add_child((vfs_dentry_t *)pts_dir->private_data, ...) --
     * that cast the slaves[] array to a parent dentry (type confusion / wild read).
     * The slave is already registered via slaves[index] above. */

    kprintf("[PTY] Created /dev/pts/%u\n", index);
    return 0;
}

// ============================================================================
// PTMX (MASTER) FILE OPERATIONS
// ============================================================================

/**
 * Open /dev/ptmx - allocates a new PTY pair
 */
static int ptmx_open(vfs_inode_t *inode, vfs_file_t *file) {
    if (!file) {
        return -1;
    }

    // Allocate new PTY pair
    int pty_index = pty_allocate();
    if (pty_index < 0) {
        kprintf("[PTY] Failed to allocate PTY for /dev/ptmx open\n");
        return -1;
    }

    // Create private data
    pty_file_data_t *pdata = (pty_file_data_t *)kmalloc(sizeof(pty_file_data_t));
    if (!pdata) {
        pty_free(pty_index);
        return -1;
    }

    pdata->pty_index = pty_index;
    pdata->is_master = true;

    file->private_data = pdata;
    file->ops = &ptmx_ops;

    kprintf("[PTY] Opened /dev/ptmx → PTY %d\n", pty_index);

    // TODO: Create /dev/pts/N slave node dynamically
    // pty_dev_create_slave(pts_dir_inode, pty_index);

    return 0;
}

/**
 * Close /dev/ptmx - decrements master ref count
 */
static int ptmx_close(vfs_file_t *file) {
    if (!file || !file->private_data) {
        return -1;
    }

    pty_file_data_t *pdata = (pty_file_data_t *)file->private_data;
    uint32_t pty_index = pdata->pty_index;

    kprintf("[PTY] Closed /dev/ptmx (PTY %u)\n", pty_index);

    // Free PTY if no more references
    // TODO: Check slave refs before freeing
    // pty_free(pty_index);

    kfree(pdata);
    file->private_data = NULL;

    return 0;
}

/**
 * Read from /dev/ptmx - reads output from slave process
 */
static ssize_t ptmx_read(vfs_file_t *file, void *buf, size_t count) {
    if (!file || !file->private_data || !buf) {
        return -1;
    }

    pty_file_data_t *pdata = (pty_file_data_t *)file->private_data;
    return pty_master_read(pdata->pty_index, (uint8_t *)buf, count);
}

/**
 * Write to /dev/ptmx - writes input to slave process
 */
static ssize_t ptmx_write(vfs_file_t *file, const void *buf, size_t count) {
    if (!file || !file->private_data || !buf) {
        return -1;
    }

    pty_file_data_t *pdata = (pty_file_data_t *)file->private_data;
    return pty_master_write(pdata->pty_index, (const uint8_t *)buf, count);
}

/**
 * lseek on /dev/ptmx (not supported)
 */
static off_t ptmx_lseek(vfs_file_t *file, off_t offset, int whence) {
    return -1;  // Seeking not supported on PTY
}

// ============================================================================
// PTS (SLAVE) FILE OPERATIONS
// ============================================================================

/**
 * Open /dev/pts/N - opens slave side of PTY
 */
static int pts_open(vfs_inode_t *inode, vfs_file_t *file) {
    if (!file || !inode) {
        return -1;
    }

    // Get PTY index from inode private data
    uint32_t pty_index = (uint32_t)(uintptr_t)inode->private_data;

    // Create private data
    pty_file_data_t *pdata = (pty_file_data_t *)kmalloc(sizeof(pty_file_data_t));
    if (!pdata) {
        return -1;
    }

    pdata->pty_index = pty_index;
    pdata->is_master = false;

    file->private_data = pdata;
    file->ops = &pts_ops;

    kprintf("[PTY] Opened /dev/pts/%u\n", pty_index);
    return 0;
}

/**
 * Close /dev/pts/N - decrements slave ref count
 */
static int pts_close(vfs_file_t *file) {
    if (!file || !file->private_data) {
        return -1;
    }

    pty_file_data_t *pdata = (pty_file_data_t *)file->private_data;
    uint32_t pty_index = pdata->pty_index;

    kprintf("[PTY] Closed /dev/pts/%u\n", pty_index);

    kfree(pdata);
    file->private_data = NULL;

    return 0;
}

/**
 * Read from /dev/pts/N - reads input from master (stdin for shell)
 */
static ssize_t pts_read(vfs_file_t *file, void *buf, size_t count) {
    if (!file || !file->private_data || !buf) {
        return -1;
    }

    pty_file_data_t *pdata = (pty_file_data_t *)file->private_data;
    return pty_slave_read(pdata->pty_index, (uint8_t *)buf, count);
}

/**
 * Write to /dev/pts/N - writes output to master (stdout from shell)
 */
static ssize_t pts_write(vfs_file_t *file, const void *buf, size_t count) {
    if (!file || !file->private_data || !buf) {
        return -1;
    }

    pty_file_data_t *pdata = (pty_file_data_t *)file->private_data;
    return pty_slave_write(pdata->pty_index, (const uint8_t *)buf, count);
}

/**
 * lseek on /dev/pts/N (not supported)
 */
static off_t pts_lseek(vfs_file_t *file, off_t offset, int whence) {
    return -1;  // Seeking not supported on PTY
}

/**
 * ioctl handler for PTY devices
 * Supports TIOCGWINSZ, TIOCSWINSZ, TCGETS, TCSETS
 *
 * SECURITY: argp is a raw user-space pointer.  We NEVER dereference it
 * directly.  All GET operations copy data into a kernel staging struct
 * first, then copy_to_user() it out.  All SET operations copy_from_user()
 * into a kernel staging struct and use only those values.  Any failure to
 * copy (bad/unmapped user pointer) returns EFAULT immediately.
 */
int pty_ioctl(vfs_file_t *file, uint32_t request, void *argp) {
    if (!file || !file->private_data) {
        return -1;
    }

    /* argp must be non-NULL for every command we handle */
    if (!argp) {
        return -1;
    }

    pty_file_data_t *pdata = (pty_file_data_t *)file->private_data;
    uint32_t pty_index = pdata->pty_index;

    switch (request) {
        case TIOCGWINSZ: {
            /*
             * GET window size: read from kernel PTY state into a local
             * kernel struct, then copy the struct out to user space.
             * The user pointer (argp) is never read or written directly.
             */
            struct {
                uint16_t ws_row;
                uint16_t ws_col;
                uint16_t ws_xpixel;
                uint16_t ws_ypixel;
            } kwinsize = {0, 0, 0, 0};

            int ret = pty_get_winsize(pty_index, &kwinsize.ws_row, &kwinsize.ws_col);
            if (ret != 0) {
                return ret;
            }

            if (copy_to_user(argp, &kwinsize, sizeof(kwinsize)) != COPY_SUCCESS) {
                return EFAULT;
            }
            return 0;
        }

        case TIOCSWINSZ: {
            /*
             * SET window size: copy the struct in from user space into a
             * kernel staging struct, then apply only the kernel-local values.
             */
            struct {
                uint16_t ws_row;
                uint16_t ws_col;
                uint16_t ws_xpixel;
                uint16_t ws_ypixel;
            } kwinsize;

            if (copy_from_user(&kwinsize, argp, sizeof(kwinsize)) != COPY_SUCCESS) {
                return EFAULT;
            }

            return pty_set_winsize(pty_index, kwinsize.ws_row, kwinsize.ws_col);
        }

        case TCGETS: {
            /*
             * GET termios flags: read from kernel PTY state, then copy
             * the value out to user space.
             */
            uint32_t kflags = 0;

            int ret = pty_get_termios(pty_index, &kflags);
            if (ret != 0) {
                return ret;
            }

            if (copy_to_user(argp, &kflags, sizeof(kflags)) != COPY_SUCCESS) {
                return EFAULT;
            }
            return 0;
        }

        case TCSETS: {
            /*
             * SET termios flags: copy the value in from user space, then
             * pass only the kernel-local copy to pty_set_termios().
             */
            uint32_t kflags = 0;

            if (copy_from_user(&kflags, argp, sizeof(kflags)) != COPY_SUCCESS) {
                return EFAULT;
            }

            return pty_set_termios(pty_index, kflags);
        }

        default:
            kprintf("[PTY] Unsupported ioctl: 0x%x\n", request);
            return -1;
    }
}
