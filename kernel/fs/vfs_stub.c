/**
 * Virtual File System (VFS) - Stub Implementation
 *
 * DISABLED: Full VFS implementation is in vfs.c.
 * This file is kept for reference but compiles to an empty translation unit
 * to avoid duplicate symbol errors.
 */

#ifdef VFS_USE_STUB  /* Only compile if full VFS is not available */

#include "../include/kernel.h"
#include "../include/vfs.h"

static int vfs_initialized = 0;

void vfs_init(void) {
    kprintf("[VFS] Initializing Virtual File System (stub)...\n");
    vfs_initialized = 1;
    kprintf("[VFS] VFS initialized successfully (stub)\n");
}

int vfs_mount(const char* source, const char* target, const char* fstype) {
    if (!vfs_initialized) return -1;
    kprintf("[VFS] Mounting %s on %s (type: %s) [stub]\n", source, target, fstype);
    return 0;
}

int vfs_open(const char* path, int flags, int mode) {
    (void)path; (void)flags; (void)mode;
    return -1;
}

ssize_t vfs_read(int fd, void* buf, size_t count) {
    (void)fd; (void)buf; (void)count;
    return -1;
}

int vfs_close(int fd) {
    (void)fd;
    return -1;
}

#endif /* VFS_USE_STUB */
