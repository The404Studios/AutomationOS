#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

// System call numbers
#define SYS_EXIT    0
#define SYS_FORK    1
#define SYS_READ    2
#define SYS_WRITE   3
#define SYS_OPEN    4
#define SYS_CLOSE   5
#define SYS_WAITPID 6
#define SYS_EXECVE  7
#define SYS_GETPID  8
#define SYS_SLEEP   9

#define MAX_SYSCALLS 256

// Syscall handler function type
typedef int64_t (*syscall_handler_t)(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                      uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Syscall initialization and dispatcher
void syscall_init(void);
int64_t syscall_dispatch(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Syscall handlers
int64_t sys_exit(uint64_t status, uint64_t arg2, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_fork(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_getpid(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Error codes
#define ESUCCESS    0
#define ENOTSUP     -1   // Not supported
#define EINVAL      -2   // Invalid argument
#define EBADF       -3   // Bad file descriptor
#define ENOMEM      -4   // Out of memory
#define ESRCH       -5   // No such process

#endif
