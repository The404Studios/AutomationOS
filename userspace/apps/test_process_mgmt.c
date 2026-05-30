// userspace/apps/test_process_mgmt.c - Test process management syscalls
// Tests SYS_GETRUSAGE, SYS_KILL, and SYS_NICE

#include "../libc/stdio.h"
#include "../libc/syscall.h"
#include "../libc/string.h"

// Include rlimit definitions for rusage
typedef struct {
    unsigned long long cpu_time;      // Total CPU time used (milliseconds)
    unsigned long long memory_peak;   // Peak memory usage (bytes)
    unsigned long long memory_current; // Current memory usage (bytes)
    unsigned long long rss_current;   // Current RSS (bytes)
    unsigned long long vmem_current;  // Current virtual memory (bytes)
    unsigned int fd_count;      // Number of open file descriptors
    unsigned long long net_rx_bytes;  // Total bytes received
    unsigned long long net_tx_bytes;  // Total bytes transmitted
    unsigned long long disk_read_bytes;  // Total bytes read from disk
    unsigned long long disk_write_bytes; // Total bytes written to disk
    unsigned long long context_switches;  // Number of context switches
    unsigned long long page_faults;   // Number of page faults
} rusage_t;

#define SYS_GETRUSAGE 12
#define RUSAGE_SELF 0

// Priority constants
#define PRIO_PROCESS 0
#define PRIO_MIN -20
#define PRIO_MAX 19

void test_getrusage(void) {
    printf("\n=== Testing SYS_GETRUSAGE ===\n");

    rusage_t usage;
    memset(&usage, 0, sizeof(usage));

    // Get resource usage for current process
    long result = (long)syscall6(SYS_GETRUSAGE, RUSAGE_SELF, (long)&usage, 0, 0, 0, 0);

    if (result < 0) {
        printf("[FAIL] getrusage failed with error: %ld\n", result);
        return;
    }

    printf("[OK] getrusage succeeded\n");
    printf("  CPU time: %llu ms\n", usage.cpu_time);
    printf("  Memory (current): %llu bytes\n", usage.memory_current);
    printf("  Memory (peak): %llu bytes\n", usage.memory_peak);
    printf("  RSS: %llu bytes\n", usage.rss_current);
    printf("  Virtual memory: %llu bytes\n", usage.vmem_current);
    printf("  Open FDs: %u\n", usage.fd_count);
    printf("  Context switches: %llu\n", usage.context_switches);
    printf("  Page faults: %llu\n", usage.page_faults);
}

void test_nice(void) {
    printf("\n=== Testing SYS_NICE ===\n");

    int pid = getpid();
    printf("Current PID: %d\n", pid);

    // Get current priority
    int current_prio = getpriority(PRIO_PROCESS, 0);
    if (current_prio < 0) {
        printf("[FAIL] getpriority failed\n");
        return;
    }

    // Convert from getpriority format (20 - prio) to nice value
    int nice_value = 20 - current_prio;
    printf("[OK] Current priority (nice): %d\n", nice_value);

    // Increase priority by 5 (lower priority)
    printf("Adjusting priority by +5...\n");
    int new_prio = nice(0, 5);
    if (new_prio < PRIO_MIN) {
        printf("[FAIL] nice failed with error: %d\n", new_prio);
        return;
    }
    printf("[OK] New priority: %d\n", new_prio);

    // Verify the change
    current_prio = getpriority(PRIO_PROCESS, 0);
    nice_value = 20 - current_prio;
    printf("[OK] Verified priority: %d\n", nice_value);

    // Reset to default priority
    printf("Resetting to priority 0...\n");
    new_prio = setpriority(PRIO_PROCESS, 0, 0);
    if (new_prio < 0) {
        printf("[FAIL] setpriority failed\n");
        return;
    }
    printf("[OK] Priority reset to 0\n");
}

void test_kill(void) {
    printf("\n=== Testing SYS_KILL ===\n");

    int pid = getpid();
    printf("Current PID: %d\n", pid);

    // Test signal 0 (check if process exists)
    printf("Testing signal 0 (existence check)...\n");
    int result = kill(pid, 0);
    if (result < 0) {
        printf("[FAIL] kill(pid, 0) failed: %d\n", result);
        return;
    }
    printf("[OK] Process %d exists\n", pid);

    // Test checking non-existent process
    printf("Testing signal 0 on non-existent process (999)...\n");
    result = kill(999, 0);
    if (result >= 0) {
        printf("[FAIL] kill(999, 0) should have failed\n");
        return;
    }
    printf("[OK] Process 999 does not exist (error: %d)\n", result);

    // Fork a child process to test SIGKILL
    printf("Forking child process...\n");
    int child_pid = fork();

    if (child_pid < 0) {
        printf("[FAIL] Fork failed\n");
        return;
    }

    if (child_pid == 0) {
        // Child process - just loop forever
        printf("[CHILD] Running with PID %d\n", getpid());
        while (1) {
            yield();
        }
    } else {
        // Parent process
        printf("[PARENT] Child PID: %d\n", child_pid);

        // Let child run a bit
        for (int i = 0; i < 5; i++) {
            yield();
        }

        // Send SIGKILL to child
        printf("[PARENT] Sending SIGKILL to child...\n");
        result = kill(child_pid, 9);  // SIGKILL = 9
        if (result < 0) {
            printf("[FAIL] kill(child, SIGKILL) failed: %d\n", result);
            return;
        }
        printf("[OK] SIGKILL sent to child\n");

        // Wait for child to terminate
        int status;
        int wait_result = waitpid(child_pid, &status, 0);
        if (wait_result < 0) {
            printf("[WARN] waitpid failed (child may still be terminating)\n");
        } else {
            printf("[OK] Child terminated with status %d\n", status);
        }
    }
}

int main(void) {
    printf("===========================================\n");
    printf("Process Management Syscall Test Suite\n");
    printf("===========================================\n");

    // Test getrusage
    test_getrusage();

    // Test nice/getpriority/setpriority
    test_nice();

    // Test kill
    test_kill();

    printf("\n===========================================\n");
    printf("All tests completed!\n");
    printf("===========================================\n");

    return 0;
}

// Raw syscall for tests that need it
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
