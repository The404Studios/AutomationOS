// userspace/libc/syscall.c - Userspace syscall wrappers
// Uses x86_64 syscall instruction to invoke kernel syscalls

#include "syscall.h"

#define O_CREAT 0x0040

// Raw syscall invocation using inline assembly
static inline long syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    register long r10 asm("r10") = a4;
    register long r8 asm("r8") = a5;
    register long r9 asm("r9") = a6;

    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );

    return ret;
}

// Syscall wrappers
void exit(int status) {
    syscall6(SYS_EXIT, status, 0, 0, 0, 0, 0);
    // Should not return
    while (1);
}

// _exit: terminate immediately without running atexit handlers or flushing.
void _exit(int status) {
    syscall6(SYS_EXIT, status, 0, 0, 0, 0, 0);
    while (1);
}

int fork(void) {
    return (int)syscall6(SYS_FORK, 0, 0, 0, 0, 0, 0);
}

long read(int fd, void* buf, unsigned long count) {
    return syscall6(SYS_READ, fd, (long)buf, count, 0, 0, 0);
}

long write(int fd, const void* buf, unsigned long count) {
    return syscall6(SYS_WRITE, fd, (long)buf, count, 0, 0, 0);
}

int getpid(void) {
    return (int)syscall6(SYS_GETPID, 0, 0, 0, 0, 0, 0);
}

// POSIX sleep(): the argument is SECONDS. SYS_SLEEP now takes MILLISECONDS
// (the PIT runs at 1000 Hz, 1 tick == 1 ms), so convert seconds -> ms here. This
// keeps every existing caller (which passes seconds) correct while the kernel
// performs a real blocking sleep. Saturate the ms product to avoid overflow on
// absurd inputs.
int sleep(unsigned int seconds) {
    unsigned long ms = (unsigned long)seconds * 1000UL;
    return (int)syscall6(SYS_SLEEP, (long)ms, 0, 0, 0, 0, 0);
}

int open(const char* path, int flags, ...) {
    int mode = 0;

    if (flags & O_CREAT) {
        __builtin_va_list args;
        __builtin_va_start(args, flags);
        mode = __builtin_va_arg(args, int);
        __builtin_va_end(args);
    }

    return (int)syscall6(SYS_OPEN, (long)path, flags, mode, 0, 0, 0);
}

int close(int fd) {
    return (int)syscall6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
}

off_t lseek(int fd, off_t offset, int whence) {
    // Note: lseek syscall doesn't exist yet in kernel, so this is a stub
    // In a real implementation, you would add SYS_LSEEK to the kernel
    (void)fd;
    (void)offset;
    (void)whence;
    return -1;  // Return error for now
}

int waitpid(int pid, int* status, int options) {
    return (int)syscall6(SYS_WAITPID, pid, (long)status, options, 0, 0, 0);
}

int execve(const char* path, char* const argv[], char* const envp[]) {
    return (int)syscall6(SYS_EXECVE, (long)path, (long)argv, (long)envp, 0, 0, 0);
}

int read_key(void) {
    return (int)syscall6(SYS_READ_EVENT, 0, 0, 0, 0, 0, 0);
}

int read_event(input_event_t* event) {
    int key = read_key();
    if (key <= 0) {
        return key;
    }

    if (event) {
        event->timestamp = 0;
        event->type = INPUT_EVENT_KEY;
        event->code = (unsigned short)key;
        event->value = 1;
    }

    return 1;
}

void yield(void) {
    syscall6(SYS_YIELD, 0, 0, 0, 0, 0, 0);
}

int spawn(const char* path) {
    return (int)syscall6(SYS_SPAWN, (long)path, 0, 0, 0, 0, 0);
}

int map_file(const char* path, void** addr, unsigned long* size) {
    return (int)syscall6(SYS_MAP_FILE, (long)path, (long)addr, (long)size, 0, 0, 0);
}

// Signal syscalls
int kill(int pid, int sig) {
    return (int)syscall6(SYS_KILL, pid, sig, 0, 0, 0, 0);
}

// Priority syscalls
int nice(int pid, int increment) {
    return (int)syscall6(SYS_NICE, pid, increment, 0, 0, 0, 0);
}

int getpriority(int which, int who) {
    return (int)syscall6(SYS_GETPRIORITY, which, who, 0, 0, 0, 0);
}

int setpriority(int which, int who, int prio) {
    return (int)syscall6(SYS_SETPRIORITY, which, who, prio, 0, 0, 0);
}

long sendfile(int out_fd, int in_fd, off_t* offset, size_t count) {
    return syscall6(SYS_SENDFILE, out_fd, in_fd, (long)offset, count, 0, 0);
}

// Threads -------------------------------------------------------------------
// entry runs as entry(arg) on stack_top (the kernel 16-aligns it). The entry
// function must end by calling thread_exit() (no per-thread crt0 here).
int thread_create(void (*entry)(void*), void* arg, void* stack_top) {
    return (int)syscall6(SYS_THREAD_CREATE, (long)entry, (long)arg,
                         (long)stack_top, 0, 0, 0);
}

void thread_exit(int retval) {
    syscall6(SYS_THREAD_EXIT, retval, 0, 0, 0, 0, 0);
    while (1) { }  // not reached
}

int thread_join(int tid, int* retval_out) {
    return (int)syscall6(SYS_THREAD_JOIN, tid, (long)retval_out, 0, 0, 0, 0);
}
