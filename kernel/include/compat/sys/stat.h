/* Freestanding kernel compatibility shim for <sys/stat.h> */
#ifndef _KERNEL_COMPAT_SYS_STAT_H
#define _KERNEL_COMPAT_SYS_STAT_H

#include "../../types.h"
#include "../../vfs.h"

/* File mode bits */
#define S_IFMT   0170000  /* type mask */
#define S_IFREG  0100000  /* regular file */
#define S_IFDIR  0040000  /* directory */
#define S_IFLNK  0120000  /* symbolic link */
#define S_IFCHR  0020000  /* character device */
#define S_IFBLK  0060000  /* block device */
#define S_IFIFO  0010000  /* FIFO */
#define S_IFSOCK 0140000  /* socket */

/* Permission bits */
#define S_IRUSR  0400
#define S_IWUSR  0200
#define S_IXUSR  0100
#define S_IRGRP  0040
#define S_IWGRP  0020
#define S_IXGRP  0010
#define S_IROTH  0004
#define S_IWOTH  0002
#define S_IXOTH  0001

/* Type test macros */
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)

/*
 * POSIX 'struct stat' -- layout-identical to vfs_stat_t from vfs.h.
 * PE loader and autofs code that uses 'struct stat' with vfs_stat/vfs_fstat
 * will work because the memory layout matches exactly.
 * We define it as a real struct so 'struct stat st;' compiles.
 */
struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
    uint64_t st_atime;
    uint64_t st_mtime;
    uint64_t st_ctime;
};

/*
 * Override vfs_stat and vfs_fstat to accept struct stat* by casting.
 * This is safe because struct stat and vfs_stat_t have identical layouts.
 */
#define vfs_stat(path, buf) vfs_stat((path), (vfs_stat_t*)(void*)(buf))
#define vfs_fstat(fd, buf)  vfs_fstat((fd), (vfs_stat_t*)(void*)(buf))

#endif
