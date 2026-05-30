/* Freestanding kernel compatibility shim for <unistd.h> */
#ifndef _KERNEL_COMPAT_UNISTD_H
#define _KERNEL_COMPAT_UNISTD_H

#include "../types.h"

/* Stub POSIX read/write/close for code that uses them directly */
static inline ssize_t read(int fd, void* buf, size_t count) {
    (void)fd; (void)buf; (void)count;
    return -1; /* Not implemented -- use vfs_read */
}

static inline ssize_t write(int fd, const void* buf, size_t count) {
    (void)fd; (void)buf; (void)count;
    return -1; /* Not implemented -- use vfs_write */
}

static inline int close(int fd) {
    (void)fd;
    return -1; /* Not implemented -- use vfs_close */
}

static inline off_t lseek(int fd, off_t offset, int whence) {
    (void)fd; (void)offset; (void)whence;
    return -1; /* Not implemented -- use vfs_lseek */
}

/* pread/pwrite stubs */
static inline ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
    (void)fd; (void)buf; (void)count; (void)offset;
    return -1;
}

static inline ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) {
    (void)fd; (void)buf; (void)count; (void)offset;
    return -1;
}

#endif
