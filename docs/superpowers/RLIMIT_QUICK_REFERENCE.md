# Resource Limits Quick Reference

## Integration Checklist

### 1. Process Creation

When creating a new process:

```c
process_t* process_create(const char* name, void* entry_point) {
    process_t* proc = kmalloc(sizeof(process_t));
    // ... initialize other fields ...

    // Create resource limit container
    if (current_process && current_process->rlimits) {
        // Child inherits parent's limits
        proc->rlimits = rlimit_inherit_container(current_process->rlimits);
    } else {
        // Root process gets default limits
        proc->rlimits = rlimit_create_container();
    }

    return proc;
}
```

### 2. Process Destruction

When destroying a process:

```c
void process_destroy(process_t* proc) {
    if (proc->rlimits) {
        rlimit_destroy_container(proc->rlimits);
        proc->rlimits = NULL;
    }
    // ... cleanup other resources ...
}
```

### 3. Scheduler Integration

On every timer tick:

```c
void schedule(void) {
    process_t* current = process_get_current();

    // Account CPU time and check quota
    if (current && current->rlimits) {
        rlimit_cpu_tick(current->rlimits, 1);

        // Check if throttled
        if (!rlimit_can_schedule(current->rlimits)) {
            current->state = PROCESS_BLOCKED;
        }
    }

    // ... pick next process ...
}
```

### 4. Memory Allocator Integration

Before allocating memory:

```c
void* kmalloc(size_t size) {
    process_t* current = process_get_current();

    // Check limit
    if (current && current->rlimits) {
        if (!rlimit_check_memory(current->rlimits, size)) {
            rlimit_oom_kill(current);
            return NULL;
        }
    }

    void* ptr = internal_alloc(size);

    // Account allocation
    if (ptr && current && current->rlimits) {
        rlimit_account_memory_alloc(current->rlimits, size);
    }

    return ptr;
}

void kfree(void* ptr, size_t size) {
    internal_free(ptr);

    process_t* current = process_get_current();
    if (current && current->rlimits) {
        rlimit_account_memory_free(current->rlimits, size);
    }
}
```

### 5. File System Integration

Before opening a file:

```c
int sys_open(const char* path, int flags) {
    process_t* current = process_get_current();

    // Check FD limit
    if (current && current->rlimits) {
        if (!rlimit_check_fd(current->rlimits)) {
            return -EMFILE;
        }
    }

    int fd = vfs_open(path, flags);

    // Account FD
    if (fd >= 0 && current && current->rlimits) {
        rlimit_account_fd_open(current->rlimits);
    }

    return fd;
}

int sys_close(int fd) {
    int result = vfs_close(fd);

    if (result == 0) {
        process_t* current = process_get_current();
        if (current && current->rlimits) {
            rlimit_account_fd_close(current->rlimits);
        }
    }

    return result;
}
```

### 6. Network Stack Integration

Before sending data:

```c
int socket_send(int sockfd, const void* buf, size_t len) {
    process_t* current = process_get_current();

    // Check bandwidth limit
    if (current && current->rlimits) {
        if (!rlimit_check_network_tx(current->rlimits, len)) {
            return -EAGAIN;  // Rate limited
        }
    }

    int sent = net_send(sockfd, buf, len);

    // Account bytes
    if (sent > 0 && current && current->rlimits) {
        rlimit_account_network_tx(current->rlimits, sent);
    }

    return sent;
}

int socket_recv(int sockfd, void* buf, size_t len) {
    process_t* current = process_get_current();

    // Check bandwidth limit
    if (current && current->rlimits) {
        if (!rlimit_check_network_rx(current->rlimits, len)) {
            return -EAGAIN;
        }
    }

    int received = net_recv(sockfd, buf, len);

    if (received > 0 && current && current->rlimits) {
        rlimit_account_network_rx(current->rlimits, received);
    }

    return received;
}
```

## Common Patterns

### Pattern 1: Check-Then-Account

Always check limit BEFORE allocation, account AFTER successful allocation:

```c
// ✓ CORRECT
if (!rlimit_check_memory(rl, size)) return NULL;
void* ptr = alloc(size);
if (ptr) rlimit_account_memory_alloc(rl, size);

// ✗ WRONG (race condition)
void* ptr = alloc(size);
if (!rlimit_check_memory(rl, size)) { free(ptr); return NULL; }
```

### Pattern 2: Graceful Degradation

Don't panic if limits not set:

```c
// ✓ CORRECT
if (current && current->rlimits) {
    rlimit_check_memory(current->rlimits, size);
}

// ✗ WRONG (crashes if no limits)
rlimit_check_memory(current->rlimits, size);
```

### Pattern 3: Cleanup on Failure

Always clean up on limit violations:

```c
// ✓ CORRECT
if (!rlimit_check_fd(rl)) {
    vfs_close(fd);
    return -EMFILE;
}

// ✗ WRONG (leaks FD)
if (!rlimit_check_fd(rl)) return -EMFILE;
```

## Common Mistakes

### Mistake 1: Forgetting to Account

```c
// ✗ WRONG
void* ptr = kmalloc(size);
// Forgot to call rlimit_account_memory_alloc!
```

### Mistake 2: Double Accounting

```c
// ✗ WRONG
rlimit_account_memory_alloc(rl, size);
rlimit_account_rss(rl, size);  // RSS already tracked in account_memory_alloc
```

### Mistake 3: Wrong Order

```c
// ✗ WRONG
rlimit_account_memory_alloc(rl, size);
if (!rlimit_check_memory(rl, size)) { ... }  // Too late!
```

### Mistake 4: Not Handling Rate Limiting

```c
// ✗ WRONG
if (!rlimit_check_network_tx(rl, len)) {
    return -EPERM;  // Should return -EAGAIN for retry
}
```

## Performance Tips

1. **Cache current process**: Don't call `process_get_current()` repeatedly
2. **Batch accounting**: Account once per transaction, not per byte
3. **Fast path**: Check limit only for user processes, not kernel threads
4. **Inline checks**: Small limit checks should be inlined

```c
// ✓ EFFICIENT
process_t* current = process_get_current();
if (current && current->rlimits) {
    for (int i = 0; i < n; i++) {
        // ... work ...
    }
    rlimit_account_cpu(current->rlimits, n);
}

// ✗ INEFFICIENT
for (int i = 0; i < n; i++) {
    process_t* current = process_get_current();  // Repeated!
    if (current && current->rlimits) {
        rlimit_account_cpu(current->rlimits, 1);  // Too granular!
    }
}
```

## Debugging

### Enable Debug Logging

Set debug flag in `rlimit.c`:

```c
#define RLIMIT_DEBUG 1
```

### Print Resource Usage

```c
rlimit_print_usage(proc->rlimits);
```

Output:
```
[RLIMIT] Resource Usage:
  CPU time: 1234 ms
  Memory: 67108864 / 134217728 bytes (peak: 67108864)
  RSS: 67108864 bytes
  Virtual memory: 67108864 bytes
  File descriptors: 42 / 1024
  Network RX: 1048576 bytes
  Network TX: 2097152 bytes
```

### Check Memory Pressure

```c
int pressure = rlimit_get_memory_pressure(rl);
// 0=LOW, 1=MEDIUM, 2=HIGH, 3=CRITICAL
```

### Check If Process Throttled

```c
if (rlimit_cpu_quota_exceeded(rl)) {
    kprintf("Process throttled due to CPU quota\n");
}
```

## Examples

### Example 1: Set Strict Memory Limit

```c
rlimit_t limit = {
    .soft = 32 * 1024 * 1024,   // 32MB soft
    .hard = 64 * 1024 * 1024    // 64MB hard
};
rlimit_set(proc->rlimits, RLIMIT_MEMORY, &limit);
```

### Example 2: Limit to 50% CPU

```c
rlimit_set_cpu_quota(proc->rlimits, 50000, 100000);
// 50ms per 100ms period = 50% CPU
```

### Example 3: Set High Priority Process

```c
rlimit_set_cpu_shares(proc->rlimits, 4096);
// 4x shares = 4x CPU time vs default process
```

### Example 4: Network Rate Limiting

```c
// 5 MB/s bandwidth, 10 MB burst
token_bucket_init(&proc->rlimits->net_tx_bucket, 5*1024*1024, 10*1024*1024);
```

## Testing Your Integration

### Unit Test Template

```c
void test_my_subsystem_limits(void) {
    rlimit_container_t* rl = rlimit_create_container();

    // Set strict limit
    rlimit_t limit = { .soft = 100, .hard = 100 };
    rlimit_set(rl, RLIMIT_MY_RESOURCE, &limit);

    // Test within limit
    ASSERT(my_operation(rl, 50) == SUCCESS);

    // Test exceeding limit
    ASSERT(my_operation(rl, 60) == FAILURE);

    rlimit_destroy_container(rl);
}
```

### Integration Test Checklist

- [ ] Process can be created with limits
- [ ] Child inherits parent limits
- [ ] Limit violations are blocked
- [ ] Soft limit triggers warning
- [ ] Hard limit terminates process
- [ ] Resource accounting is accurate
- [ ] No memory leaks on limit violations
- [ ] Performance overhead < 5%

## Kernel Configuration

Add to kernel initialization:

```c
void kernel_main(void) {
    // ... other init ...

    rlimit_init();  // Initialize resource limits

    // ... create processes ...
}
```

Add to syscall table:

```c
syscall_table[SYS_SETRLIMIT] = sys_setrlimit;
syscall_table[SYS_GETRLIMIT] = sys_getrlimit;
syscall_table[SYS_GETRUSAGE] = sys_getrusage;
syscall_table[SYS_PRLIMIT] = sys_prlimit;
```

## Build Configuration

Add to Makefile:

```makefile
RLIMIT_OBJS = kernel/core/rlimit/rlimit.o \
              kernel/core/rlimit/cpu.o \
              kernel/core/rlimit/memory.o \
              kernel/core/rlimit/fd.o \
              kernel/core/rlimit/network.o \
              kernel/core/rlimit/syscall.o

kernel.elf: $(RLIMIT_OBJS)
```

---

**Quick Links**:
- Full documentation: `RESOURCE_LIMITS.md`
- Unit tests: `tests/unit/test_rlimit.c`
- Integration tests: `tests/integration/test_resource_exhaustion.py`
