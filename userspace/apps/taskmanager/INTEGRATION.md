# Task Manager Kernel Integration Guide

This document describes how to integrate the Task Manager with the AutomationOS kernel.

## Overview

The Task Manager requires kernel support for:
1. Process enumeration
2. System statistics collection
3. Process control (kill/suspend/resume/priority)

## Required Kernel Features

### 1. Process Enumeration System Call

**Syscall Number**: 100 (`SYS_GET_PROCESS_LIST`)

**Prototype**:
```c
long sys_get_process_list(process_info_t* buffer, int max_count);
```

**Parameters**:
- `buffer`: User-space buffer to receive process information
- `max_count`: Maximum number of processes to return

**Returns**: Number of processes copied to buffer, or negative error code

**Implementation** (`kernel/core/syscall/handlers.c`):
```c
#include "../../include/sched.h"

// Process info structure (must match userspace)
typedef struct {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t state;
    char name[64];
    char username[32];
    uint64_t cpu_percent;
    uint64_t memory_rss;
    uint64_t memory_shared;
    uint64_t memory_virtual;
    uint64_t disk_read;
    uint64_t disk_write;
    uint64_t net_recv;
    uint64_t net_send;
    uint64_t total_cpu_time;
    int32_t priority;
    uint32_t cpu_affinity;
    uint32_t threads;
    uint64_t start_time;
} __attribute__((packed)) process_info_kernel_t;

long sys_get_process_list(process_info_kernel_t* user_buffer, int max_count) {
    // Validate parameters
    if (!user_buffer || max_count <= 0 || max_count > 256) {
        return -EINVAL;
    }

    // TODO: Validate user_buffer is in valid user memory range

    int count = 0;
    
    // Iterate through process table
    for (int i = 0; i < MAX_PROCESSES && count < max_count; i++) {
        process_t* proc = process_get_by_pid(i);
        if (!proc || proc->state == PROCESS_TERMINATED) {
            continue;
        }

        process_info_kernel_t info;
        memset(&info, 0, sizeof(info));

        // Basic info
        info.pid = proc->pid;
        info.parent_pid = proc->parent_pid;
        info.state = (uint32_t)proc->state;
        strncpy(info.name, proc->name, 63);
        info.name[63] = '\0';

        // User info (TODO: implement UID lookup)
        if (proc->uid == 0) {
            strcpy(info.username, "root");
        } else {
            strcpy(info.username, "user");
        }

        // CPU statistics
        info.total_cpu_time = proc->total_time;
        // Calculate CPU percentage from last scheduler tick
        // TODO: Track per-process CPU time delta
        info.cpu_percent = calculate_cpu_percent(proc);

        // Memory statistics
        // TODO: Query VMM for actual memory usage
        info.memory_rss = estimate_rss(proc);
        info.memory_shared = 0;  // TODO: implement
        info.memory_virtual = 0; // TODO: implement

        // I/O statistics
        // TODO: Track per-process I/O in device drivers
        info.disk_read = 0;
        info.disk_write = 0;
        info.net_recv = 0;
        info.net_send = 0;

        // Process properties
        info.priority = 0;  // TODO: add priority to process_t
        info.cpu_affinity = 0xFFFF;  // TODO: add affinity to process_t
        info.threads = 1;  // TODO: implement threading
        info.start_time = 0;  // TODO: track process start time

        // Copy to user space
        // TODO: Use proper copy_to_user() function
        user_buffer[count] = info;
        count++;
    }

    return count;
}
```

### 2. System Statistics System Call

**Syscall Number**: 101 (`SYS_GET_SYSTEM_INFO`)

**Prototype**:
```c
long sys_get_system_info(system_stats_t* stats);
```

**Implementation**:
```c
typedef struct {
    uint32_t total_processes;
    uint32_t running_processes;
    uint32_t blocked_processes;
    uint8_t cpu_count;
    uint64_t cpu_usage[16];
    uint64_t cpu_total_usage;
    uint64_t memory_total;
    uint64_t memory_used;
    uint64_t memory_free;
    uint64_t memory_cached;
    uint64_t memory_buffers;
    uint64_t disk_read_rate;
    uint64_t disk_write_rate;
    uint64_t disk_read_total;
    uint64_t disk_write_total;
    uint64_t net_recv_rate;
    uint64_t net_send_rate;
    uint64_t net_recv_total;
    uint64_t net_send_total;
    uint64_t uptime_seconds;
} __attribute__((packed)) system_stats_kernel_t;

long sys_get_system_info(system_stats_kernel_t* user_stats) {
    if (!user_stats) {
        return -EINVAL;
    }

    system_stats_kernel_t stats;
    memset(&stats, 0, sizeof(stats));

    // Process counts
    stats.total_processes = count_processes();
    stats.running_processes = count_processes_by_state(PROCESS_RUNNING);
    stats.blocked_processes = count_processes_by_state(PROCESS_BLOCKED);

    // CPU statistics
    stats.cpu_count = get_cpu_count();
    for (int i = 0; i < stats.cpu_count; i++) {
        stats.cpu_usage[i] = get_cpu_usage(i);
    }
    stats.cpu_total_usage = get_total_cpu_usage();

    // Memory statistics
    stats.memory_total = pmm_get_total_memory();
    stats.memory_used = pmm_get_used_memory();
    stats.memory_free = stats.memory_total - stats.memory_used;
    stats.memory_cached = get_cache_memory();
    stats.memory_buffers = get_buffer_memory();

    // Disk I/O statistics
    // TODO: Aggregate from all block devices
    stats.disk_read_rate = 0;
    stats.disk_write_rate = 0;
    stats.disk_read_total = 0;
    stats.disk_write_total = 0;

    // Network statistics
    // TODO: Aggregate from all network interfaces
    stats.net_recv_rate = 0;
    stats.net_send_rate = 0;
    stats.net_recv_total = 0;
    stats.net_send_total = 0;

    // System uptime
    stats.uptime_seconds = get_uptime_seconds();

    // Copy to user space
    // TODO: Use proper copy_to_user()
    *user_stats = stats;

    return 0;
}
```

### 3. Process Control System Calls

**Set Priority** (`SYS_SET_PRIORITY` = 202):
```c
long sys_set_priority(uint32_t pid, int priority) {
    if (priority < -20 || priority > 19) {
        return -EINVAL;
    }

    process_t* proc = process_get_by_pid(pid);
    if (!proc) {
        return -ESRCH;
    }

    // Check permissions
    process_t* current = process_get_current();
    if (current->uid != 0 && current->uid != proc->uid) {
        return -EPERM;
    }

    // Set priority
    proc->priority = priority;
    
    // Adjust scheduler time slice based on priority
    scheduler_update_priority(proc);

    return 0;
}
```

**Set CPU Affinity** (`SYS_SET_AFFINITY` = 203):
```c
long sys_set_cpu_affinity(uint32_t pid, uint32_t affinity_mask) {
    if (affinity_mask == 0) {
        return -EINVAL;
    }

    process_t* proc = process_get_by_pid(pid);
    if (!proc) {
        return -ESRCH;
    }

    // Check permissions
    process_t* current = process_get_current();
    if (current->uid != 0 && current->uid != proc->uid) {
        return -EPERM;
    }

    // Validate affinity mask against available CPUs
    uint32_t max_mask = (1 << get_cpu_count()) - 1;
    if (affinity_mask & ~max_mask) {
        return -EINVAL;
    }

    proc->cpu_affinity = affinity_mask;

    return 0;
}
```

## Kernel Modifications Required

### 1. Add Fields to `process_t` (kernel/include/sched.h)
```c
typedef struct process {
    // ... existing fields ...
    
    // New fields for task manager
    int32_t priority;           // Nice value (-20 to +19)
    uint32_t cpu_affinity;      // CPU affinity bitmask
    uint64_t start_time;        // Process start time (ticks)
    
    // I/O statistics
    uint64_t disk_read_bytes;
    uint64_t disk_write_bytes;
    uint64_t net_recv_bytes;
    uint64_t net_send_bytes;
} process_t;
```

### 2. Add Helper Functions (kernel/core/sched/process.c)
```c
// Count processes by state
int count_processes_by_state(process_state_t state) {
    int count = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* proc = process_get_by_pid(i);
        if (proc && proc->state == state) {
            count++;
        }
    }
    return count;
}

// Calculate CPU percentage for a process
uint64_t calculate_cpu_percent(process_t* proc) {
    // TODO: Track per-process CPU time over last second
    // For now, estimate from time slice usage
    return 0;
}

// Estimate RSS (resident set size)
uint64_t estimate_rss(process_t* proc) {
    // TODO: Walk page tables to count mapped pages
    return 0;
}
```

### 3. Register System Calls (kernel/core/syscall/syscall.c)
```c
static void* syscall_table[] = {
    [0]   = sys_exit,
    [1]   = sys_fork,
    // ... existing syscalls ...
    [100] = sys_get_process_list,
    [101] = sys_get_system_info,
    [200] = sys_suspend,
    [201] = sys_resume,
    [202] = sys_set_priority,
    [203] = sys_set_cpu_affinity,
};
```

### 4. Track I/O Statistics

In device drivers, increment process I/O counters:
```c
// In disk driver (kernel/drivers/storage/block.c)
void block_read(block_device_t* dev, uint64_t sector, void* buffer) {
    // ... perform read ...
    
    process_t* current = process_get_current();
    if (current) {
        current->disk_read_bytes += 512;
    }
}

// In network driver (kernel/drivers/net/...)
void net_receive(net_device_t* dev, void* packet, size_t len) {
    // ... process packet ...
    
    process_t* current = process_get_current();
    if (current) {
        current->net_recv_bytes += len;
    }
}
```

## Testing Integration

### Phase 1: Mock Data (Current)
- Task manager uses mock data
- Tests UI rendering and interaction
- No kernel dependencies

### Phase 2: Partial Integration
1. Implement `SYS_GET_PROCESS_LIST` with basic info
2. Test with real process table
3. Verify process enumeration works

### Phase 3: Full Integration
1. Implement all system calls
2. Add I/O tracking to drivers
3. Add CPU usage tracking to scheduler
4. Test all process control features

### Phase 4: Optimization
1. Profile system call overhead
2. Optimize process iteration
3. Add caching for frequently-accessed data
4. Batch updates for better performance

## Performance Considerations

### CPU Overhead
- Process enumeration: O(n) where n = number of processes
- Expected overhead: < 1ms for 256 processes
- Called once per second, negligible impact

### Memory Overhead
- Temporary buffer for process list: ~50 KB
- Allocated in kernel, copied to userspace
- Freed immediately after copy

### Optimization Strategies
1. **Lazy evaluation**: Only collect detailed stats on demand
2. **Caching**: Cache system-wide stats, update every 100ms
3. **Incremental updates**: Return only changed processes
4. **Priority scheduling**: Run task manager at lower priority

## Security Considerations

### Permission Checks
- Only root can see all processes
- Regular users see only their own processes
- Process control requires ownership or CAP_KILL capability

### Information Leakage
- Sanitize process names (no kernel pointers)
- Hide kernel threads from non-root users
- Validate all user pointers before dereferencing

### Resource Limits
- Limit frequency of system calls (rate limiting)
- Cap maximum process count returned
- Prevent DoS via repeated enumeration

## Future Enhancements

### Kernel-Side
1. Process tree reconstruction
2. Per-process memory maps
3. Open file descriptor tracking
4. Network connection tracking
5. Real-time CPU usage calculation
6. Historical statistics collection

### Userspace-Side
1. Better syscall wrappers
2. Efficient diff-based updates
3. Configurable refresh rates
4. Plugin system for custom monitors

## References

- Linux `/proc` filesystem design
- Windows Task Manager implementation
- FreeBSD `top` command source code
- htop source code for UI inspiration
