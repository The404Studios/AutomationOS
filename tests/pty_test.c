/**
 * PTY Integration Test
 *
 * Tests PTY driver integration with kernel syscalls
 */

#include <stdint.h>

// Syscall numbers
#define SYS_OPEN    4
#define SYS_CLOSE   5
#define SYS_READ    2
#define SYS_WRITE   3
#define SYS_IOCTL   36

// File flags
#define O_RDWR      0x0002

// ioctl commands
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414

// Window size structure
typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} winsize_t;

// Syscall wrapper
static inline int64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    int64_t ret;
    __asm__ volatile (
        "mov %1, %%rax\n"
        "mov %2, %%rdi\n"
        "mov %3, %%rsi\n"
        "mov %4, %%rdx\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(ret)
        : "r"(num), "r"(arg1), "r"(arg2), "r"(arg3)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

int main(void) {
    // Test 1: Open /dev/ptmx
    int fd = syscall3(SYS_OPEN, (uint64_t)"/dev/ptmx", O_RDWR, 0);
    if (fd < 0) {
        return 1; // Failed to open
    }

    // Test 2: Get window size
    winsize_t ws;
    int ret = syscall3(SYS_IOCTL, fd, TIOCGWINSZ, (uint64_t)&ws);
    if (ret < 0) {
        syscall3(SYS_CLOSE, fd, 0, 0);
        return 2; // Failed to get window size
    }

    // Test 3: Set window size
    ws.ws_row = 30;
    ws.ws_col = 120;
    ret = syscall3(SYS_IOCTL, fd, TIOCSWINSZ, (uint64_t)&ws);
    if (ret < 0) {
        syscall3(SYS_CLOSE, fd, 0, 0);
        return 3; // Failed to set window size
    }

    // Test 4: Verify window size
    winsize_t ws2;
    ret = syscall3(SYS_IOCTL, fd, TIOCGWINSZ, (uint64_t)&ws2);
    if (ret < 0 || ws2.ws_row != 30 || ws2.ws_col != 120) {
        syscall3(SYS_CLOSE, fd, 0, 0);
        return 4; // Window size mismatch
    }

    // Test 5: Write to PTY
    const char* msg = "Hello PTY!\n";
    int len = 11;
    ret = syscall3(SYS_WRITE, fd, (uint64_t)msg, len);
    if (ret != len) {
        syscall3(SYS_CLOSE, fd, 0, 0);
        return 5; // Write failed
    }

    // Close PTY
    syscall3(SYS_CLOSE, fd, 0, 0);

    return 0; // All tests passed
}
