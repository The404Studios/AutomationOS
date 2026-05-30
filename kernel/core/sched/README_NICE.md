# Process Priority Management

This file documents the priority management syscalls in `nice.c`.

## Files

- `nice.c` - Implementation of priority management syscalls

## Implemented Syscalls

### SYS_NICE (27)

Adjust process priority (nice value).

**Signature:**
```c
int64_t sys_nice(uint64_t pid, uint64_t increment, ...);
```

**Arguments:**
- `pid`: Target process ID (0 for current process)
- `increment`: Priority adjustment or absolute priority value

**Behavior:**
- If `pid == 0`: Add `increment` to current process priority (POSIX nice behavior)
- If `pid != 0`: Set absolute priority for specified process (taskmanager use case)

**Returns:**
- New priority value on success (-20 to +19)
- -ESRCH if process not found
- Automatically clamped to valid range

**Priority Range:**
- -20: Highest priority (most CPU time)
- 0: Default priority
- +19: Lowest priority (least CPU time)

### SYS_GETPRIORITY (28)

Get process priority.

**Signature:**
```c
int64_t sys_getpriority(uint64_t which, uint64_t who, ...);
```

**Arguments:**
- `which`: Priority type (PRIO_PROCESS=0, PRIO_PGRP, PRIO_USER)
- `who`: Target ID (0 for current process)

**Returns:**
- Priority value as `20 - nice_value` (0-40 range to avoid negative returns)
- -ESRCH if process not found
- -EINVAL if `which` is not PRIO_PROCESS (others not implemented yet)

**Note:** POSIX getpriority returns `20 - priority` to avoid returning negative
values (which would be indistinguishable from error codes).

### SYS_SETPRIORITY (29)

Set process priority.

**Signature:**
```c
int64_t sys_setpriority(uint64_t which, uint64_t who, uint64_t prio, ...);
```

**Arguments:**
- `which`: Priority type (PRIO_PROCESS=0, PRIO_PGRP, PRIO_USER)
- `who`: Target ID (0 for current process)
- `prio`: New priority value (-20 to +19)

**Returns:**
- 0 on success
- -ESRCH if process not found
- -EINVAL if `which` is not PRIO_PROCESS

## Process Structure

The `process_t` structure in `kernel/include/sched.h` now includes:

```c
typedef struct process {
    ...
    int32_t priority;  // Process priority (nice value: -20 to +19)
    ...
} process_t;
```

Default priority is 0, set during `process_create()`.

## Scheduler Integration

**Current Status:** The scheduler does not yet use priority values for scheduling
decisions. All processes are scheduled with equal time slices.

**Future Work:**
- Priority-based time slice calculation
- Multi-level feedback queue scheduler
- Real-time priority classes (SCHED_FIFO, SCHED_RR)

## Usage Example

```c
#include <syscall.h>

// Get current priority
int prio = getpriority(PRIO_PROCESS, 0);
int nice_val = 20 - prio;
printf("Current priority: %d\n", nice_val);

// Lower priority (higher nice value)
nice(0, 5);  // Add 5 to current priority

// Set specific priority for another process
setpriority(PRIO_PROCESS, pid, 10);

// Set priority via nice with absolute value
nice(pid, -10);  // Set priority to -10 for process pid
```

## Permission Model

**Current Implementation:** No permission checks. Any process can adjust any
priority.

**Future Work:**
- Lowering priority (increasing nice) should always be allowed
- Raising priority (decreasing nice) requires CAP_SYS_NICE capability
- Setting priority for other processes requires CAP_SYS_NICE or same UID
- See Linux capabilities system for reference

## Testing

See `userspace/apps/test_process_mgmt.c` for comprehensive tests.
