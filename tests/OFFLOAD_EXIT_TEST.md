# Process Exit Stress Test for CPU1 Offload

## Overview

`test_offload_process_exit.c` is a stress test that validates the robustness of the CPU1 offload mechanism when processes are killed mid-job via SIGKILL.

## Test Objectives

1. **CPU1 Completes or Orphans Safely**: Verify that CPU1 does not hang when the requesting process is killed mid-job
2. **No Memory Leaks**: Ensure kernel heap and page tables are properly cleaned up
3. **System Stability**: Confirm the system remains stable after process destruction
4. **Job Queue Cleanup**: Validate that the job queue is properly cleaned up

## Test Design

### Matrix Size
- **128x128** (16,384 elements) for long-running jobs to increase probability of mid-job kill

### Iterations
- **5 iterations** (configurable via `NUM_ITERATIONS`)
- Each iteration forks a child, lets it start an offload job, kills it, and verifies cleanup

### Kill Delay
- **50ms** busy-wait before SIGKILL to ensure job has started
- Adjustable via `KILL_DELAY_MS`

## Test Sequence (Per Iteration)

```
1. Record free memory (SYS_SYSINFO)
2. Fork child process (SYS_FORK)
3. Child: Start large matmul offload job (SYS_CPU1_OFFLOAD)
4. Parent: Wait 50ms
5. Parent: Send SIGKILL to child (SYS_KILL)
6. Parent: Wait for child exit (SYS_WAITPID)
7. Record free memory after cleanup
8. Verify no memory leak (tolerance: 4KB)
```

## Memory Leak Detection

The test uses `SYS_SYSINFO` to query kernel free memory before and after each iteration:

```c
typedef struct {
    u64 total_mem;      // total physical memory in bytes
    u64 free_mem;       // free physical memory in bytes
    u64 uptime_ms;      // milliseconds since boot
    u32 proc_count;     // number of live processes
    u32 _pad;           // reserved
} sysinfo_t;
```

**Leak Detection Logic:**
- If `mem_after + 4096 < mem_before`, report memory leak
- 4KB tolerance accounts for kernel bookkeeping overhead

## Build Instructions

```bash
# Compile
gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
    -fno-pic -fno-pie -mno-red-zone -O2 \
    -c tests/test_offload_process_exit.c -o test_offload_process_exit.o

# Link
ld -nostdlib -static -n -no-pie -e _start -T userspace/userspace.ld \
    test_offload_process_exit.o -o test_offload_process_exit

# Add to initrd
cp test_offload_process_exit initrd/bin/

# Rebuild initrd and boot
./quick_build.sh
```

## Expected Output

### SMP_FOUNDATION Kernel (Offload Available)

```
====================================================================
OFFLOAD_EXIT: Process exit stress test (kill mid-job)
====================================================================
Matrix size: 128x128
Iterations: 5
Kill delay: 50 ms

[PROBE] Checking if SYS_CPU1_OFFLOAD is available...
[PROBE] SYS_CPU1_OFFLOAD available, rc=1

[ITER 0] Starting
[ITER 0] Free memory before: 524288000 bytes
[PARENT] Child PID 42 started
[CHILD 42] Starting long offload job...
[PARENT] Sending SIGKILL to child 42
[PARENT] Waiting for child to exit...
[PARENT] Child exited, exit_code=-9
[ITER 0] Free memory after: 524288000 bytes
[ITER 0] PASS (no leak)

[ITER 1] Starting
...
[ITER 4] PASS (no leak)

[FINAL] Stability check: forking child...
[FINAL CHILD] Exiting cleanly
[FINAL] Stability check passed

====================================================================
OFFLOAD_EXIT: PASS N=5 no-leaks system-stable
====================================================================
```

### DEFAULT Kernel (No Offload Syscall)

```
====================================================================
OFFLOAD_EXIT: Process exit stress test (kill mid-job)
====================================================================
Matrix size: 128x128
Iterations: 5
Kill delay: 50 ms

[PROBE] Checking if SYS_CPU1_OFFLOAD is available...
OFFLOAD_EXIT: SKIP (no SMP offload syscall)
```

## Failure Scenarios

### Memory Leak Detected

```
[ITER 2] Free memory before: 524288000 bytes
[PARENT] Child PID 44 started
[PARENT] Sending SIGKILL to child 44
[PARENT] Waiting for child to exit...
[PARENT] Child exited, exit_code=-9
[ITER 2] Free memory after: 524000000 bytes
[ITER 2] Memory leak detected: 288000 bytes leaked
OFFLOAD_EXIT: FAIL memory-leak
```

**Diagnosis**: The kernel did not properly free the job's argument/result buffers or page tables when the process was killed.

### SIGKILL Failed

```
[PARENT] Sending SIGKILL to child 42
[PARENT] SIGKILL failed, rc=-3
OFFLOAD_EXIT: FAIL sigkill-failed
```

**Diagnosis**: `SYS_KILL` returned an error (likely `-ESRCH` if child already exited).

### WAITPID Failed

```
[PARENT] Waiting for child to exit...
[PARENT] WAITPID failed, rc=-10
OFFLOAD_EXIT: FAIL waitpid-failed
```

**Diagnosis**: Child process was not reaped properly, or PID is invalid.

### Final Stability Check Failed

```
[FINAL] Stability check: forking child...
OFFLOAD_EXIT: FAIL final-fork-failed
```

**Diagnosis**: The system is in an unstable state after the stress test (e.g., process table corrupted, scheduler hung).

## Kernel Implementation Notes

### What the Kernel Must Handle

1. **Process Exit Mid-Job**:
   - Detect when a process with a pending offload job exits
   - Mark the job as orphaned or aborted
   - Prevent CPU1 from writing to freed memory

2. **Memory Cleanup**:
   - Free argument and result buffers (if kernel-owned)
   - Unmap user pages (if user-owned)
   - Free page table entries

3. **Job Queue Cleanup**:
   - Remove the pending job from the queue
   - Wake CPU1 if it's waiting on the job
   - Ensure no dangling pointers

4. **Synchronization**:
   - Proper locking between CPU0 (process exit) and CPU1 (job execution)
   - Avoid race conditions where CPU1 writes after the process exits

### Recommended Approach

```c
/* In process exit handler (do_exit or similar): */
void cleanup_offload_jobs(process_t *proc) {
    spin_lock(&g_offload_lock);
    
    /* Check if this process has a pending job */
    if (g_offload_job.owner_pid == proc->pid && g_offload_job.pending) {
        /* Mark job as aborted */
        g_offload_job.aborted = 1;
        
        /* Free kernel copies if any */
        if (g_offload_job.arg_kbuf) {
            kfree(g_offload_job.arg_kbuf);
            g_offload_job.arg_kbuf = NULL;
        }
        
        /* Clear job */
        g_offload_job.pending = 0;
        g_offload_job.owner_pid = 0;
    }
    
    spin_unlock(&g_offload_lock);
}

/* In CPU1 job executor: */
void cpu1_executor(void) {
    while (1) {
        spin_lock(&g_offload_lock);
        
        if (!g_offload_job.pending) {
            spin_unlock(&g_offload_lock);
            cpu_relax();
            continue;
        }
        
        /* Check if job was aborted */
        if (g_offload_job.aborted) {
            g_offload_job.pending = 0;
            spin_unlock(&g_offload_lock);
            continue;
        }
        
        /* Execute job ... */
        
        spin_unlock(&g_offload_lock);
    }
}
```

## Integration with Smoke Tests

Add to `smoke_persist.sh` or similar:

```bash
# Test 42: Offload process exit stress test
echo "TEST 42: Offload process exit (kill mid-job)..."
test_offload_process_exit
if grep -q "OFFLOAD_EXIT: PASS\|OFFLOAD_EXIT: SKIP" /tmp/smoke_results.txt; then
    echo "  ✓ PASS"
else
    echo "  ✗ FAIL"
    exit 1
fi
```

## Tuning Parameters

### For Faster Testing
```c
#define N 64                   // Smaller matrix (faster job)
#define NUM_ITERATIONS 3       // Fewer iterations
#define KILL_DELAY_MS 10       // Shorter delay
```

### For Stress Testing
```c
#define N 256                  // Larger matrix (longer job)
#define NUM_ITERATIONS 20      // More iterations
#define KILL_DELAY_MS 100      // Longer delay (higher kill probability)
```

## Related Tests

- `test_concurrent_offload.c`: Multi-threaded offload stress test
- `test_smp.c`: General SMP smoke tests
- `smp_load_balance_test.c`: Load balancing validation

## References

- Kernel: `kernel/core/syscall/cpu1_offload.c` (offload implementation)
- Kernel: `kernel/core/process.c` (process exit handler)
- Kernel: `kernel/include/syscall.h` (syscall numbers)
- Kernel: `kernel/include/procapi.h` (sysinfo_t structure)
