/* Freestanding kernel compatibility shim for <fcntl.h> */
#ifndef _KERNEL_COMPAT_FCNTL_H
#define _KERNEL_COMPAT_FCNTL_H

#include "../types.h"

/* File open flags -- matching vfs.h definitions */
#ifndef O_RDONLY
#define O_RDONLY    0x0000
#endif
#ifndef O_WRONLY
#define O_WRONLY    0x0001
#endif
#ifndef O_RDWR
#define O_RDWR      0x0002
#endif
#ifndef O_CREAT
#define O_CREAT     0x0040
#endif
#ifndef O_TRUNC
#define O_TRUNC     0x0200
#endif
#ifndef O_APPEND
#define O_APPEND    0x0400
#endif
#ifndef O_EXCL
#define O_EXCL      0x0080
#endif

/* Stub open/close for code that uses POSIX-style file operations */
static inline int open(const char* path, int flags, ...) {
    (void)path; (void)flags;
    return -1; /* Not implemented in kernel -- use vfs_open */
}

#endif
