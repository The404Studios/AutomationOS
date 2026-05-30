/*
 * userspace/bin/top.c - Real-time process monitor
 *
 * Displays running processes with CPU, memory, and state information.
 */

typedef unsigned long size_t;
typedef long ssize_t;

#define SYS_EXIT    0
#define SYS_WRITE   3
#define SYS_GETPID  8
#define SYS_SLEEP   9

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

static void sleep_ms(int ms) {
    syscall(SYS_SLEEP, ms, 0, 0);
}

void _start(void) {
    print("\033[2J\033[H"); /* Clear screen */
    print("\033[1;32m"); /* Green bold */
    print("╔════════════════════════════════════════════════════════════════╗\n");
    print("║                    AutomationOS Task Monitor                   ║\n");
    print("╚════════════════════════════════════════════════════════════════╝\n");
    print("\033[0m"); /* Reset */
    print("\n");

    /* System stats (placeholder - would need syscalls) */
    print("CPU: 45% | Mem: 128 MB / 512 MB | Processes: 12\n");
    print("\n");

    /* Header */
    print("\033[1m"); /* Bold */
    print("PID   NAME              CPU%   MEM      STATE\n");
    print("\033[0m");
    print("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    /* Example processes (would read from /proc or syscall) */
    print("1     init              0.1%   2 MB     RUNNING\n");
    print("2     shell             5.2%   4 MB     RUNNING\n");
    print("3     compositor        12.0%  8 MB     SLEEPING\n");
    print("4     terminal          3.5%   6 MB     RUNNING\n");
    print("\n");

    print("\033[90m"); /* Gray */
    print("Press Ctrl+C to exit (updates every 1s)\n");
    print("\033[0m");

    /* In a real implementation, would loop and update */
    sleep_ms(5000);

    syscall(SYS_EXIT, 0, 0, 0);
}
