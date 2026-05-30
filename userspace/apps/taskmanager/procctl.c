// userspace/apps/taskmanager/procctl.c - Process control operations

#include "taskmanager.h"
#include "../../libc/stdio.h"
#include "../../libc/syscall.h"

// System call numbers - now defined in syscall.h
#define SYS_KILL 26
#define SYS_SET_PRIORITY 27  // Uses SYS_NICE internally
#define SYS_SET_AFFINITY 203  // Not implemented yet

// Signals (standard UNIX-like)
#define SIGTERM 15
#define SIGKILL 9
#define SIGSTOP 19
#define SIGCONT 18

// System call wrapper for kill
static long sys_kill(uint32_t pid, int signal) {
    long result;
    __asm__ volatile (
        "mov %1, %%rdi\n"
        "mov %2, %%rsi\n"
        "mov %3, %%rax\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r" (result)
        : "r" ((uint64_t)pid), "r" ((uint64_t)signal), "r" ((uint64_t)SYS_KILL)
        : "rax", "rdi", "rsi", "rcx", "r11", "memory"
    );
    return result;
}

// System call wrapper for priority
static long sys_set_priority(uint32_t pid, int priority) {
    long result;
    __asm__ volatile (
        "mov %1, %%rdi\n"
        "mov %2, %%rsi\n"
        "mov %3, %%rax\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r" (result)
        : "r" ((uint64_t)pid), "r" ((uint64_t)priority), "r" ((uint64_t)SYS_SET_PRIORITY)
        : "rax", "rdi", "rsi", "rcx", "r11", "memory"
    );
    return result;
}

// System call wrapper for CPU affinity
static long sys_set_affinity(uint32_t pid, uint32_t affinity) {
    long result;
    __asm__ volatile (
        "mov %1, %%rdi\n"
        "mov %2, %%rsi\n"
        "mov %3, %%rax\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r" (result)
        : "r" ((uint64_t)pid), "r" ((uint64_t)affinity), "r" ((uint64_t)SYS_SET_AFFINITY)
        : "rax", "rdi", "rsi", "rcx", "r11", "memory"
    );
    return result;
}

// Kill a process (send termination signal)
int kill_process(uint32_t pid, int signal) {
    if (pid == 0) {
        printf("[ERROR] Cannot kill kernel process\n");
        return -1;
    }

    if (pid == 1) {
        printf("[ERROR] Cannot kill init process\n");
        return -1;
    }

    // Use SIGKILL if signal not specified
    if (signal == 0) {
        signal = SIGKILL;
    }

    long result = sys_kill(pid, signal);
    if (result < 0) {
        printf("[ERROR] Failed to kill process %d: error %ld\n", pid, result);
        return -1;
    }

    printf("[OK] Sent signal %d to process %d\n", signal, pid);
    return 0;
}

// Suspend a process
int suspend_process(uint32_t pid) {
    if (pid == 0 || pid == 1) {
        printf("[ERROR] Cannot suspend critical system process\n");
        return -1;
    }

    long result = sys_kill(pid, SIGSTOP);
    if (result < 0) {
        printf("[ERROR] Failed to suspend process %d\n", pid);
        return -1;
    }

    printf("[OK] Suspended process %d\n", pid);
    return 0;
}

// Resume a suspended process
int resume_process(uint32_t pid) {
    long result = sys_kill(pid, SIGCONT);
    if (result < 0) {
        printf("[ERROR] Failed to resume process %d\n", pid);
        return -1;
    }

    printf("[OK] Resumed process %d\n", pid);
    return 0;
}

// Set process priority (nice value)
int set_process_priority(uint32_t pid, int priority) {
    // Priority range: -20 (highest) to +19 (lowest)
    if (priority < -20 || priority > 19) {
        printf("[ERROR] Invalid priority %d (must be -20 to +19)\n", priority);
        return -1;
    }

    long result = sys_set_priority(pid, priority);
    if (result < 0) {
        printf("[ERROR] Failed to set priority for process %d\n", pid);
        return -1;
    }

    printf("[OK] Set priority of process %d to %d\n", pid, priority);
    return 0;
}

// Set CPU affinity mask
int set_cpu_affinity(uint32_t pid, uint32_t affinity_mask) {
    if (affinity_mask == 0) {
        printf("[ERROR] Invalid affinity mask (must have at least one CPU)\n");
        return -1;
    }

    long result = sys_set_affinity(pid, affinity_mask);
    if (result < 0) {
        printf("[ERROR] Failed to set affinity for process %d\n", pid);
        return -1;
    }

    printf("[OK] Set CPU affinity of process %d to 0x%X\n", pid, affinity_mask);
    return 0;
}
