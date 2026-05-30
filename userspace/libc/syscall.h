// userspace/libc/syscall.h - Syscall wrapper declarations

#ifndef SYSCALL_H
#define SYSCALL_H

#ifndef NULL
#define NULL ((void*)0)
#endif
typedef unsigned long size_t;

// System call numbers (must match kernel/include/syscall.h)
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
#define SYS_READ_EVENT 14
#define SYS_YIELD   15
#define SYS_SPAWN   16
#define SYS_MAP_FILE 17
#define SYS_KILL    26
#define SYS_NICE    27
#define SYS_GETPRIORITY 28
#define SYS_SETPRIORITY 29
#define SYS_OPENDIR 30
#define SYS_READDIR 31
#define SYS_CLOSEDIR 32
#define SYS_STAT    33
#define SYS_UNLINK  34
#define SYS_RENAME  35
#define SYS_SENDFILE 71

#define INPUT_EVENT_KEY 0

// Input event structure (must match kernel)
typedef struct {
    unsigned long long timestamp;
    unsigned short type;
    unsigned short code;
    int value;
} input_event_t;

// File descriptor constants
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// Seek modes
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

// Open flags
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0040
#define O_TRUNC  0x0200
#define O_APPEND 0x0400
#define O_EXCL   0x0080

// Type definitions
typedef long off_t;
typedef long ssize_t;

// Syscall wrappers
void exit(int status) __attribute__((noreturn));
int fork(void);
long read(int fd, void* buf, unsigned long count);
long write(int fd, const void* buf, unsigned long count);
int getpid(void);
int sleep(unsigned int seconds);
int open(const char* path, int flags, ...);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int waitpid(int pid, int* status, int options);
int execve(const char* path, char* const argv[], char* const envp[]);
int read_key(void);
int read_event(input_event_t* event);
void yield(void);
int spawn(const char* path);
int map_file(const char* path, void** addr, unsigned long* size);

// Signal and priority syscalls
int kill(int pid, int sig);
int nice(int pid, int increment);
int getpriority(int which, int who);
int setpriority(int which, int who, int prio);

// Zero-copy file transfer
long sendfile(int out_fd, int in_fd, off_t* offset, size_t count);

#endif
