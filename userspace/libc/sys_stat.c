// userspace/libc/sys_stat.c - File status implementation

#include "sys_stat.h"
#include "syscall.h"

// Internal syscall wrapper for stat
static inline int sys_stat_raw(const char* path, struct stat* buf) {
    long result;
    __asm__ volatile (
        "movq $33, %%rax\n"    // SYS_STAT = 33
        "movq %1, %%rdi\n"
        "movq %2, %%rsi\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r"(result)
        : "r"(path), "r"(buf)
        : "rax", "rdi", "rsi", "rcx", "r11", "memory"
    );
    return (int)result;
}

// Internal syscall wrapper for unlink
static inline int sys_unlink_raw(const char* path) {
    long result;
    __asm__ volatile (
        "movq $34, %%rax\n"    // SYS_UNLINK = 34
        "movq %1, %%rdi\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r"(result)
        : "r"(path)
        : "rax", "rdi", "rcx", "r11", "memory"
    );
    return (int)result;
}

// Internal syscall wrapper for rename
static inline int sys_rename_raw(const char* oldpath, const char* newpath) {
    long result;
    __asm__ volatile (
        "movq $35, %%rax\n"    // SYS_RENAME = 35
        "movq %1, %%rdi\n"
        "movq %2, %%rsi\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r"(result)
        : "r"(oldpath), "r"(newpath)
        : "rax", "rdi", "rsi", "rcx", "r11", "memory"
    );
    return (int)result;
}

// Get file status
int stat(const char* path, struct stat* buf) {
    if (!path || !buf) {
        return -1;
    }
    return sys_stat_raw(path, buf);
}

// Get file status by descriptor (not implemented yet)
int fstat(int fd, struct stat* buf) {
    (void)fd;
    (void)buf;
    return -1;  // Not implemented
}

// Get file status (same as stat for now - no symlink support)
int lstat(const char* path, struct stat* buf) {
    return stat(path, buf);
}

// Create directory (stub for now)
int mkdir(const char* path, mode_t mode) {
    (void)path;
    (void)mode;
    return -1;  // Not implemented
}

// Change file permissions (stub for now)
int chmod(const char* path, mode_t mode) {
    (void)path;
    (void)mode;
    return -1;  // Not implemented
}

// Change file permissions by descriptor (stub for now)
int fchmod(int fd, mode_t mode) {
    (void)fd;
    (void)mode;
    return -1;  // Not implemented
}

// Set file mode creation mask (stub for now)
mode_t umask(mode_t mask) {
    (void)mask;
    return 0;  // Not implemented
}

// Delete a file
int unlink(const char* path) {
    if (!path) {
        return -1;
    }
    return sys_unlink_raw(path);
}

// Rename a file
int rename(const char* oldpath, const char* newpath) {
    if (!oldpath || !newpath) {
        return -1;
    }
    return sys_rename_raw(oldpath, newpath);
}
