// userspace/libc/unistd.h - POSIX standard symbolic constants and prototypes
//
// These prototypes are thin declarations for wrappers implemented elsewhere in
// the libc (syscall.c, sys_stat.c). It exists so ported tools that
// `#include <unistd.h>` find the expected names without dragging in syscall.h.

#ifndef UNISTD_H
#define UNISTD_H

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef unsigned long size_t;
#endif
typedef long ssize_t;
typedef long off_t;
typedef int pid_t;

// Standard file descriptors.
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// lseek whence (also defined in stdio.h / syscall.h with identical values).
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

// access() mode bits.
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

// I/O (implemented in syscall.c).
ssize_t read(int fd, void* buf, unsigned long count);
ssize_t write(int fd, const void* buf, unsigned long count);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
ssize_t sendfile(int out_fd, int in_fd, off_t* offset, size_t count);

// Filesystem (implemented in sys_stat.c).
int unlink(const char* path);

// Process control (implemented in syscall.c).
int fork(void);
int execve(const char* path, char* const argv[], char* const envp[]);
pid_t getpid(void);
int sleep(unsigned int seconds);
void _exit(int status) __attribute__((noreturn));

#endif /* UNISTD_H */
