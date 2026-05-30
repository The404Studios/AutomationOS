/*
 * userspace/bin/ifconfig.c - Network interface configuration display
 *
 * Shows network interface status, addresses, and statistics.
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
    print("\033[1;36m"); /* Cyan bold */
    print("Network Interfaces\n");
    print("==================\n");
    print("\033[0m\n");

    /* eth0 interface (would read from kernel) */
    print("\033[1m"); /* Bold */
    print("eth0:\033[0m ");
    print("\033[1;32mUP\033[0m, "); /* Green UP */
    print("192.168.1.100/24\n");

    print("      MAC: 52:54:00:12:34:56\n");
    print("      RX: 1024 packets, 512 KB\n");
    print("      TX: 2048 packets, 1 MB\n");
    print("      Errors: 0\n");
    print("\n");

    /* lo interface */
    print("\033[1m");
    print("lo:\033[0m ");
    print("\033[1;32mUP\033[0m, ");
    print("127.0.0.1/8\n");
    print("      Loopback interface\n");
    print("      RX: 256 packets, 64 KB\n");
    print("      TX: 256 packets, 64 KB\n");
    print("\n");

    /* wlan0 down */
    print("\033[1m");
    print("wlan0:\033[0m ");
    print("\033[1;31mDOWN\033[0m\n"); /* Red DOWN */
    print("      Wireless interface (not configured)\n");
    print("\n");

    syscall(SYS_EXIT, 0, 0, 0);
}
