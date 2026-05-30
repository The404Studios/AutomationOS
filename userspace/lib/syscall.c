#include "syscall.h"

// Low-level syscall function
static inline long syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    register long r10 asm("r10") = a4;
    register long r8 asm("r8") = a5;
    register long r9 asm("r9") = a6;

    asm volatile (
        "syscall"
        : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );

    return ret;
}

long sys_read(int fd, void* buf, size_t count) {
    return syscall(SYS_READ, fd, (long)buf, count, 0, 0, 0);
}

long sys_write(int fd, const void* buf, size_t count) {
    return syscall(SYS_WRITE, fd, (long)buf, count, 0, 0, 0);
}

long sys_open(const char* path, int flags) {
    return syscall(SYS_OPEN, (long)path, flags, 0, 0, 0, 0);
}

long sys_close(int fd) {
    return syscall(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
}

void sys_exit(int code) {
    syscall(SYS_EXIT, code, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

long sys_fork(void) {
    return syscall(SYS_FORK, 0, 0, 0, 0, 0, 0);
}

long sys_exec(const char* path, char* const argv[]) {
    return syscall(SYS_EXECVE, (long)path, (long)argv, 0, 0, 0, 0);
}

long sys_wait(int* status) {
    return syscall(SYS_WAITPID, -1, (long)status, 0, 0, 0, 0);
}

long sys_getpid(void) {
    return syscall(SYS_GETPID, 0, 0, 0, 0, 0, 0);
}
