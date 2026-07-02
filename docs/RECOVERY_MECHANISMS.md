# Recovery Mechanisms

AutomationOS implements comprehensive recovery mechanisms to handle failures and maintain system stability across both single-core and multi-core configurations.

## 1. AP Panic Recovery

The Application Processor (AP) panic recovery mechanism ensures system resilience when secondary CPUs encounter fatal errors.

### Exception Handler on CPU1

**Location**: `kernel/arch/x86_64/idt.c:exception_handler()`

When an exception occurs, the handler:
- Determines if the exception originated from user mode or kernel mode (checks CS register)
- Attempts page fault resolution for user-mode faults via `handle_page_fault()`
- Terminates the faulting user process if unrecoverable
- Validates CR3 to ensure the correct process is terminated (prevents killing wrong victim)

**Key features**:
- User-mode exceptions terminate only the offending process
- Kernel-mode exceptions trigger a full system panic with diagnostics
- CR3 mismatch detection prevents incorrect process termination

### BSP Response (-EAPFAULT)

**Location**: `kernel/arch/x86_64/ap_boot.c`

When CPU1 fails to start or becomes unresponsive:

```c
int try_start_cpu1(void) {
    // Send INIT-SIPI-SIPI to CPU1
    lapic_send_init(aid);
    ap_tsc_delay_us(10000);  // >= 10ms settle
    
    lapic_send_startup(aid, AP_SIPI_VECTOR);
    ap_tsc_delay_us(200);    // >= 200us
    
    // Bounded wait with TSC deadline (~100ms)
    uint64_t start = rdtsc();
    uint64_t deadline = AP_WAIT_US * AP_TSC_PER_US;
    
    while (__atomic_load_n(&ap1_online, __ATOMIC_ACQUIRE) == 0) {
        if ((rdtsc() - start) >= deadline) {
            return 0;  // Timeout -> degrade safely
        }
        __asm__ volatile("pause" ::: "memory");
    }
    
    return 1;  // CPU1 online
}
```

**Timeout behavior**:
- **Duration**: ~100ms TSC-based deadline
- **Action**: Returns 0 (failure), BSP logs the failure and continues single-core
- **Hard rule**: AP failure MUST NOT stop or hang the BSP
- **Memory polling**: Polls shared memory flag, NOT MMIO/APIC registers (wedged LAPIC cannot hang BSP)

### System Continues Single-Core

When AP startup fails:
```
[SMP] starting CPU 1 (APIC id 1) via INIT-SIPI-SIPI...
[SMP] CPU 1 FAILED to start (timeout)
[KERNEL] Continuing in single-core mode (BSP only)
```

The kernel gracefully degrades to single-CPU operation with no feature loss for single-threaded workloads.

## 2. Deadlock Detection

**Location**: `kernel/core/health_monitor.c`

The health monitor implements all-CPUs-stalled detection through heartbeat monitoring.

### All-CPUs-Stalled Heuristic

Each CPU maintains a heartbeat counter incremented on every scheduler tick:

```c
typedef struct per_cpu_health {
    volatile uint64_t heartbeat;      // Incremented by timer tick
    uint64_t last_heartbeat;          // Previous sample value
    // ... other fields
} per_cpu_health_t;
```

**Stall detection algorithm**:
```c
uint32_t health_monitor_detect_stalls(void) {
    uint32_t stalled = 0;
    
    // Skip first sample (no baseline)
    if (g_system_health.sample_count <= 1) {
        return 0;
    }
    
    for (int cpu = 0; cpu < smp_num_online; cpu++) {
        per_cpu_health_t* snapshot = &g_system_health.cpu[cpu];
        
        // CPU stalled if heartbeat not advancing
        if (snapshot->heartbeat == snapshot->last_heartbeat) {
            stalled++;
            kprintf("[HEALTH] CPU%d stalled: heartbeat=%lu (no change)\n",
                    cpu, snapshot->heartbeat);
        }
    }
    
    return stalled;
}
```

### 5-Second Monitoring Interval

The health monitor runs as a background kernel thread:

```c
static void health_monitor_thread(void* arg) {
    while (1) {
        // Sleep for 5 seconds (5000 ms)
        uint64_t now = timer_get_ticks();
        uint64_t wake_time = now + 5000;
        
        while (timer_get_ticks() < wake_time) {
            schedule();  // Yield to other processes
        }
        
        // Sample all CPUs
        health_monitor_sample();
        
        // Detect anomalies
        uint32_t stalls = health_monitor_detect_stalls();
        uint32_t leaks = health_monitor_detect_leaks();
        
        if (stalls > 0) {
            kprintf("[HEALTH] CPU stall detected on %u CPU(s)!\n", stalls);
            health_monitor_report();
        }
    }
}
```

**Sampling process**:
1. Every 5 seconds, snapshot all per-CPU health metrics
2. Compare current heartbeat to previous heartbeat for each CPU
3. Report stalls if heartbeat hasn't advanced
4. Generate full diagnostic report

### Fail-Stop Recovery (Panic)

When all CPUs are stalled (complete system deadlock):

```c
if (stalls == smp_num_online) {
    kprintf("[HEALTH] ALL CPUs stalled - system deadlock!\n");
    health_monitor_report();
    kernel_panic("System deadlock: all CPUs stalled");
}
```

**Recovery action**: Fail-stop with full diagnostic dump rather than silent hang.

## 3. Timeout Diagnostics

The kernel implements bounded waits with clear diagnostics to distinguish failure modes.

### Heartbeat Check Distinguishes States

The health monitor distinguishes three CPU states:

1. **SLOW**: Heartbeat advancing, but at reduced rate
   - Detection: `delta = heartbeat - last_heartbeat`
   - Threshold: `delta < expected_rate * 0.5`
   - Action: Log warning, continue monitoring

2. **WEDGED**: Heartbeat frozen (no progress)
   - Detection: `heartbeat == last_heartbeat` for multiple consecutive samples
   - Threshold: 2-3 samples (~10-15 seconds)
   - Action: Report stall, prepare for recovery

3. **PANIC**: CPU in exception handler (heartbeat frozen + exception flag)
   - Detection: `cpu_state == CPU_STATE_EXCEPTION`
   - Action: Immediate panic with diagnostics

### Clear Error Messages

**Timeout example** (CPU1 job):
```c
int cpu1_wait(uint64_t deadline_tsc) {
    while (__atomic_load_n(&cpu1_job.done, __ATOMIC_ACQUIRE) == 0) {
        if (rdtsc() >= deadline_tsc) {
            kprintf("[CPU1] Job timeout: CPU1 did not complete within deadline\n");
            kprintf("       State: pending=%d, done=%d\n", 
                    cpu1_job.pending, cpu1_job.done);
            return 0;  // Timeout
        }
        __asm__ volatile("pause");
    }
    return 1;  // Success
}
```

**Stall diagnostic**:
```
[HEALTH] CPU stall detected on 1 CPU(s)!
[HEALTH] CPU1 stalled: heartbeat=12345 (no change)

=== Health Monitor Report ===
Samples: 42
Total stalls detected: 1

CPU1:
  Heartbeat: 12345 (prev: 12345, delta: 0)
  Queue depth: 3
  Ownership: allocs=150, frees=148, leaks=2
```

**AP startup failure**:
```
[SMP] starting CPU 1 (APIC id 1) via INIT-SIPI-SIPI...
[SMP] Waiting for CPU 1 online flag (100ms timeout)...
[SMP] CPU 1 FAILED to start (timeout)
[SMP] Possible causes:
       - AP not responding to SIPI
       - Trampoline code fault
       - APIC configuration error
[KERNEL] Degrading to single-core operation (BSP only)
```

## 4. Health Monitor

**Location**: `kernel/core/health_monitor.c`, `kernel/include/health_monitor.h`

The health monitoring subsystem provides runtime observability for early detection of system issues.

### Runtime Observability

**Per-CPU metrics tracked**:
```c
typedef struct per_cpu_health {
    /* Liveness tracking */
    uint64_t heartbeat;           // Scheduler tick counter
    uint64_t last_heartbeat;      // Previous sample
    
    /* Work queue metrics */
    uint32_t queue_depth;         // Pending runnable threads
    
    /* Memory ownership tracking */
    uint32_t ownership_allocs;    // Total kmalloc_ref() calls
    uint32_t ownership_frees;     // Total kput(refcount=0) calls
    uint32_t ownership_leaks;     // allocs - frees
} per_cpu_health_t;
```

**System-wide state**:
```c
typedef struct system_health {
    per_cpu_health_t cpu[MAX_CPUS];
    
    uint64_t sample_count;
    uint32_t stalls_detected;
    uint32_t deadlocks_detected;  // Reserved for future
    uint32_t panics_detected;
} system_health_t;
```

### 5-Second Sampling

The monitoring thread samples metrics at regular intervals:

```c
void health_monitor_sample(void) {
    g_system_health.sample_count++;
    
    for (int cpu = 0; cpu < smp_num_online; cpu++) {
        percpu_data_t* cpu_data_ptr = cpu_data(cpu);
        per_cpu_health_t* snapshot = &g_system_health.cpu[cpu];
        
        // Save previous heartbeat for stall detection
        snapshot->last_heartbeat = snapshot->heartbeat;
        
        // Sample current values
        snapshot->heartbeat = cpu_data_ptr->health.heartbeat;
        snapshot->queue_depth = 0;  // TODO: integrate with scheduler
        snapshot->ownership_allocs = cpu_data_ptr->health.ownership_allocs;
        snapshot->ownership_frees = cpu_data_ptr->health.ownership_frees;
    }
}
```

### Stall/Leak/Deadlock Detection

**Stall detection** (covered in section 2):
- Heartbeat frozen detection
- Per-CPU and system-wide reporting

**Leak detection**:
```c
uint32_t health_monitor_detect_leaks(void) {
    uint32_t leaked_cpus = 0;
    const uint32_t LEAK_THRESHOLD = 100;
    
    for (int cpu = 0; cpu < smp_num_online; cpu++) {
        per_cpu_health_t* snapshot = &g_system_health.cpu[cpu];
        uint32_t allocs = snapshot->ownership_allocs;
        uint32_t frees = snapshot->ownership_frees;
        uint32_t leaks = (allocs > frees) ? (allocs - frees) : 0;
        
        snapshot->ownership_leaks = leaks;
        
        if (leaks > LEAK_THRESHOLD) {
            kprintf("[HEALTH] CPU%d leak: %u objects (%u allocs - %u frees)\n",
                    cpu, leaks, allocs, frees);
            leaked_cpus++;
        }
    }
    
    return leaked_cpus;
}
```

**Deadlock detection** (future):
- Lock dependency graph analysis
- Cycle detection in lock acquisition order
- Reserved field in `system_health_t` for deadlock counter

## Integration Points

### Scheduler Integration

Heartbeat increment on timer tick:
```c
void health_monitor_tick(void) {
    uint32_t cpu = cpu_id();
    if (cpu >= MAX_CPUS) return;
    
    percpu_data_t* cpu_data_ptr = cpu_data(cpu);
    __atomic_add_fetch(&cpu_data_ptr->health.heartbeat, 1, __ATOMIC_RELAXED);
}
```

Called from: `kernel/core/sched/scheduler.c:scheduler_tick()`

### Memory Allocator Integration

Ownership tracking:
```c
void health_monitor_record_alloc(void) {
    uint32_t cpu = cpu_id();
    if (cpu >= smp_num_online) return;
    __atomic_add_fetch(&cpu_data(cpu)->health.ownership_allocs, 1, __ATOMIC_RELAXED);
}

void health_monitor_record_free(void) {
    uint32_t cpu = cpu_id();
    if (cpu >= smp_num_online) return;
    __atomic_add_fetch(&cpu_data(cpu)->health.ownership_frees, 1, __ATOMIC_RELAXED);
}
```

Called from:
- `kernel/core/mem/ownership.c:kmalloc_ref()`
- `kernel/core/mem/ownership.c:kput()`

### SMP Initialization

```c
int smp_init(void) {
    // ... BSP setup ...
    
    // Initialize health monitoring
    health_monitor_init();
    
    // ... AP startup ...
    
    // Start monitoring thread after scheduler is running
    health_monitor_start_thread();
    
    return smp_num_cpus;
}
```

## Configuration

### Compile-Time Options

- `SMP_ENABLE`: Enable multi-core support (default: disabled)
- `SMP_FOUNDATION`: Enable SMP foundation brick (CPU1 worker loop)
- `SMP_FORCE_AP_FAIL`: Force AP startup failure for testing recovery

### Runtime Tuning

**Health monitor intervals** (`kernel/core/health_monitor.c`):
- `HEALTH_SAMPLE_INTERVAL`: 5000ms (5 seconds)
- `LEAK_THRESHOLD`: 100 objects
- `STALL_THRESHOLD`: 2 consecutive zero-delta samples

**Timeout values** (`kernel/arch/x86_64/ap_boot.c`):
- `AP_WAIT_US`: 100000 (100ms, liveness bound)
- `AP_TSC_PER_US`: 3000 (3 GHz estimate)
- Generous compute timeout: 2-5 seconds for real work

## Best Practices

1. **Always use bounded waits**: Never spin indefinitely on memory or MMIO
2. **Poll memory, not MMIO**: Wedged hardware cannot hang software
3. **Fail-stop over silent hang**: Panic with diagnostics beats silent freeze
4. **Validate state transitions**: Check owner_pid, job_seq before acting
5. **Serialize critical sections**: Use spinlocks for ownership transitions
6. **Report clear diagnostics**: Distinguish slow/wedged/panic states

## Testing

### Health Monitor Self-Test

```bash
# Enable health monitoring
make SMP=1 -j$(nproc)

# Boot and observe health reports
./quick_build.sh
```

Expected output:
```
[HEALTH] Health monitoring initialized
[HEALTH] Health monitor thread started
[HEALTH] Health monitor thread scheduled
[HEALTH] CPU0 heartbeat: 1234 -> 1290 (delta: 56)
[HEALTH] CPU1 heartbeat: 567 -> 623 (delta: 56)
```

### AP Failure Test

```bash
# Force AP startup failure
make SMP=1 SMP_FORCE_AP_FAIL=1 -j$(nproc)
```

Expected output:
```
[SMP] SMP_FORCE_AP_FAIL: targeting bogus APIC id 0xfe
[SMP] starting CPU 1 (APIC id 254) via INIT-SIPI-SIPI...
[SMP] CPU 1 FAILED to start (timeout)
[KERNEL] Continuing in single-core mode (BSP only)
```

### Stall Injection Test

Inject artificial stall:
```c
// In test code: freeze CPU1 heartbeat
cpu_data(1)->health.heartbeat = 0;
while (1) __asm__ volatile("pause");  // Spin without incrementing
```

Expected detection:
```
[HEALTH] CPU stall detected on 1 CPU(s)!
[HEALTH] CPU1 stalled: heartbeat=0 (no change)
```

## References

- **SMP Brick 3 (AP Startup)**: `kernel/arch/x86_64/ap_boot.c`
- **Health Monitor**: `kernel/core/health_monitor.c`, `kernel/include/health_monitor.h`
- **Exception Handler**: `kernel/arch/x86_64/idt.c:exception_handler()`
- **Panic Handler**: `kernel/lib/panic.c`
- **SMP State**: `kernel/arch/x86_64/smp.c`, `kernel/include/smp.h`

## Future Enhancements

1. **IPI-based recovery**: Send IPI to stalled CPU, escalate to panic if no response
2. **Lock dependency graph**: Runtime deadlock prediction via lock ordering analysis
3. **Watchdog timer**: Hardware watchdog integration for fail-stop guarantee
4. **Per-CPU panic**: AP panic recovery without BSP halt (graceful CPU offline)
5. **Adaptive timeouts**: TSC calibration for accurate deadline computation
