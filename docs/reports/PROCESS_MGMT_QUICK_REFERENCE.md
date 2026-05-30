# Process Management Syscalls - Quick Reference

## Syscall Numbers

| Syscall | Number | Description |
|---------|--------|-------------|
| `SYS_GETRUSAGE` | 12 | Get resource usage statistics |
| `SYS_KILL` | 26 | Send signal to process |
| `SYS_NICE` | 27 | Adjust process priority |
| `SYS_GETPRIORITY` | 28 | Get process priority |
| `SYS_SETPRIORITY` | 29 | Set process priority |

## Signal Numbers

| Signal | Value | Description |
|--------|-------|-------------|
| 0 | 0 | Check if process exists (no signal sent) |
| `SIGKILL` | 9 | Force terminate process (cannot be caught) |
| `SIGTERM` | 15 | Request graceful termination |
| `SIGCONT` | 18 | Resume suspended process |
| `SIGSTOP` | 19 | Suspend process (cannot be caught) |

## Priority Values

| Value | Meaning |
|-------|---------|
| -20 | Highest priority (most CPU time) |
| 0 | Default priority |
| +19 | Lowest priority (least CPU time) |

## C API (userspace/libc)

### Resource Usage
```c
#include <syscall.h>

typedef struct {
    uint64_t cpu_time;        // CPU time in milliseconds
    uint64_t memory_current;  // Current memory usage
    uint64_t memory_peak;     // Peak memory usage
    uint64_t rss_current;     // Resident set size
    uint64_t context_switches; // Context switch count
    // ... more fields
} rusage_t;

// Get resource usage
rusage_t usage;
int result = syscall6(SYS_GETRUSAGE, RUSAGE_SELF, (long)&usage, 0, 0, 0, 0);
```

### Signal Sending
```c
#include <syscall.h>

// Kill wrapper
int kill(int pid, int sig);

// Examples:
kill(pid, SIGKILL);  // Force kill
kill(pid, SIGTERM);  // Graceful shutdown
kill(pid, SIGSTOP);  // Suspend
kill(pid, SIGCONT);  // Resume
kill(pid, 0);        // Check if exists
```

### Priority Management
```c
#include <syscall.h>

// Adjust current process priority
int nice(int pid, int increment);

// Get priority
int getpriority(int which, int who);

// Set priority
int setpriority(int which, int who, int prio);

// Examples:
nice(0, 5);                          // Lower current process priority
nice(pid, 10);                       // Set process priority to 10
int prio = getpriority(0, 0);        // Get current priority
setpriority(0, pid, -5);             // Set high priority
```

## Assembly Interface (x86_64)

### Direct Syscall
```asm
; SYS_KILL example
mov rax, 26          ; SYS_KILL
mov rdi, pid         ; arg1: process ID
mov rsi, signal      ; arg2: signal number
syscall
; Return value in rax

; SYS_NICE example
mov rax, 27          ; SYS_NICE
mov rdi, pid         ; arg1: process ID (0 = current)
mov rsi, increment   ; arg2: priority adjustment
syscall
; Return value in rax (new priority)

; SYS_GETRUSAGE example
mov rax, 12          ; SYS_GETRUSAGE
mov rdi, 0           ; arg1: RUSAGE_SELF
mov rsi, addr        ; arg2: pointer to rusage_t
syscall
; Return value in rax (0 = success)
```

## Error Codes

| Code | Name | Meaning |
|------|------|---------|
| 0 | `ESUCCESS` | Success |
| -1 | `ENOTSUP` | Not supported |
| -3 | `ESRCH` | No such process |
| -14 | `EFAULT` | Bad address |
| -22 | `EINVAL` | Invalid argument |

## Task Manager Integration

### Kill Process
```c
void kill_process(uint32_t pid) {
    if (kill(pid, SIGKILL) < 0) {
        printf("Failed to kill process %d\n", pid);
    }
}
```

### Suspend/Resume
```c
void suspend_process(uint32_t pid) {
    kill(pid, SIGSTOP);
}

void resume_process(uint32_t pid) {
    kill(pid, SIGCONT);
}
```

### Adjust Priority
```c
void set_priority(uint32_t pid, int priority) {
    // Priority range: -20 to +19
    if (priority < -20) priority = -20;
    if (priority > 19) priority = 19;
    
    nice(pid, priority);  // Sets absolute priority
}
```

### Monitor Resources
```c
void show_process_stats(uint32_t pid) {
    rusage_t usage;
    if (syscall6(SYS_GETRUSAGE, RUSAGE_SELF, (long)&usage, 0, 0, 0, 0) == 0) {
        printf("CPU: %llu ms\n", usage.cpu_time);
        printf("Memory: %llu bytes\n", usage.memory_current);
        printf("Context switches: %llu\n", usage.context_switches);
    }
}
```

## Implementation Files

### Kernel
- `kernel/core/signal/kill.c` - Signal sending
- `kernel/core/sched/nice.c` - Priority management
- `kernel/core/rlimit/syscall.c` - Resource usage (getrusage)

### Userspace
- `userspace/libc/syscall.c` - Syscall wrappers
- `userspace/libc/syscall.h` - Function declarations
- `userspace/libc/signal.c` - Signal constants and utilities

### Testing
- `userspace/apps/test_process_mgmt.c` - Test suite

## Notes

1. **Signal 0:** Can be used to check if a process exists without sending a signal
2. **Priority Clamping:** All priority values are automatically clamped to [-20, +19]
3. **POSIX getpriority:** Returns `20 - priority` to avoid negative values
4. **No Permissions:** Currently no UID/GID checks (TODO)
5. **Scheduler:** Priority values are stored but not yet used by scheduler (TODO)

## See Also

- `PROCESS_MGMT_SYSCALLS_SUMMARY.md` - Full implementation details
- `kernel/core/signal/README.md` - Signal system documentation
- `kernel/core/sched/README_NICE.md` - Priority system documentation
