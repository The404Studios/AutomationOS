/**
 * Minimal User Mode Test Program
 * ===============================
 *
 * This is the simplest possible userspace program to test:
 * 1. ELF loading
 * 2. Ring 0 → Ring 3 transition
 * 3. User mode execution
 *
 * This program just exits immediately via syscall.
 * No libc, no dependencies - pure syscalls.
 */

// System call numbers (must match kernel/include/syscall.h)
#define SYS_EXIT    1
#define SYS_WRITE   3
#define SYS_GETPID  8

/**
 * sys_exit - Terminate process
 */
static inline void sys_exit(int status) {
    asm volatile(
        "mov $1, %%rax\n"     // Syscall number: SYS_EXIT
        "mov %0, %%rdi\n"     // Argument: exit status
        "syscall\n"
        :
        : "r"((long)status)
        : "rax", "rdi", "rcx", "r11", "memory"
    );
    __builtin_unreachable();
}

/**
 * sys_write - Write to file descriptor
 */
static inline long sys_write(int fd, const void* buf, unsigned long count) {
    long ret;
    asm volatile(
        "mov $3, %%rax\n"     // Syscall number: SYS_WRITE
        "mov %1, %%rdi\n"     // Arg 1: fd
        "mov %2, %%rsi\n"     // Arg 2: buffer
        "mov %3, %%rdx\n"     // Arg 3: count
        "syscall\n"
        "mov %%rax, %0\n"     // Return value
        : "=r"(ret)
        : "r"((long)fd), "r"(buf), "r"(count)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

/**
 * sys_getpid - Get process ID
 */
static inline long sys_getpid(void) {
    long ret;
    asm volatile(
        "mov $8, %%rax\n"     // Syscall number: SYS_GETPID
        "syscall\n"
        "mov %%rax, %0\n"     // Return value
        : "=r"(ret)
        :
        : "rax", "rcx", "r11", "memory"
    );
    return ret;
}

/**
 * _start - Entry point for user mode program
 *
 * This is called by the kernel after ELF loading and ring transition.
 * At this point, we're in ring 3 with a user stack.
 */
void _start(void) {
    // Test 1: Write a message to stdout (fd=1)
    const char msg[] = "Hello from Ring 3!\n";
    sys_write(1, msg, sizeof(msg) - 1);

    // Test 2: Get our PID (should be non-zero if scheduler works)
    long pid = sys_getpid();

    // Test 3: Exit cleanly with status code
    // Use PID as exit status for easy verification
    sys_exit((int)pid);
}
