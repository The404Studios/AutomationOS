#ifndef SYSCALL_H
#define SYSCALL_H

// Basic types (no stddef.h in freestanding)
typedef unsigned long size_t;

#ifndef NULL
#define NULL ((void*)0)
#endif

// Syscall numbers (must match kernel/include/syscall.h)
#define SYS_EXIT       0
#define SYS_FORK       1
#define SYS_READ       2
#define SYS_WRITE      3
#define SYS_OPEN       4
#define SYS_CLOSE      5
#define SYS_WAITPID    6
#define SYS_EXECVE     7
#define SYS_GETPID     8
#define SYS_SLEEP      9
#define SYS_READ_EVENT 14
#define SYS_YIELD      15
#define SYS_SPAWN      16
#define SYS_MAP_FILE   17

#define SYS_EXEC SYS_EXECVE
#define SYS_WAIT SYS_WAITPID

// Syscall wrappers
long sys_read(int fd, void* buf, size_t count);
long sys_write(int fd, const void* buf, size_t count);
long sys_open(const char* path, int flags);
long sys_close(int fd);
void sys_exit(int code) __attribute__((noreturn));
long sys_fork(void);
long sys_execve(const char* path, char* const argv[], char* const envp[]);
long sys_exec(const char* path, char* const argv[]);
long sys_wait(int* status);
long sys_getpid(void);

#endif
