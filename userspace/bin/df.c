/*
 * userspace/bin/df.c - Disk space usage utility
 *
 * Display filesystem disk space usage.
 */

typedef unsigned long size_t;

#define SYS_EXIT    0
#define SYS_WRITE   3

static inline long syscall(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static void print(const char* msg) {
    syscall(SYS_WRITE, 1, (long)msg, strlen(msg));
}

void _start(void) {
    print("\033[1m"); /* Bold */
    print("Filesystem                Size    Used   Avail   Use%  Mounted on\n");
    print("\033[0m");
    print("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    /* Example data - would query VFS in real implementation */
    print("ramfs                    512M    128M    384M     25%  /\n");
    print("diskfs                   20G     5.2G    14G      27%  /mnt/data\n");
    print("tmpfs                    256M     12M    244M      5%  /tmp\n");
    print("devfs                      -       -       -       -   /dev\n");

    print("\n");
    print("\033[32mTotal:\033[0m 20.7 GB  |  ");
    print("\033[33mUsed:\033[0m 5.3 GB  |  ");
    print("\033[36mFree:\033[0m 15.4 GB\n");

    syscall(SYS_EXIT, 0, 0, 0);
}
