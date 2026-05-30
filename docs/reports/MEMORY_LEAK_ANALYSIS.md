# AutomationOS Memory Leak Analysis Report

## Executive Summary

This document catalogs all identified memory leaks in AutomationOS, categorized by subsystem and severity. Each leak includes:
- **Location**: File and line number
- **Type**: Boot-time, runtime, or error-path leak
- **Severity**: Critical (always leaks), High (common path), Medium (error path), Low (rare)
- **Impact**: Amount of memory leaked per occurrence
- **Fix**: Specific remediation steps

---

## 1. Boot-Time Leaks

### LEAK-001: UEFI Memory Map Allocation
**Location**: `boot/loader.c:206`  
**Severity**: LOW (intentional, passed to kernel)  
**Type**: Boot-time  
**Status**: NOT A LEAK - Memory passed to kernel

**Description**:
```c
status = BS->AllocatePool(EfiLoaderData, memory_map_size, (void**)&uefi_memory_map);
```

The UEFI memory map is allocated but never freed. However, this is intentional - the map is passed to the kernel via `boot_info` structure and used throughout kernel lifetime.

**Action**: Document as intentional, no fix needed.

---

### LEAK-002: File Info Buffer Not Freed
**Location**: `boot/loader.c:321-325`  
**Severity**: MEDIUM  
**Type**: Boot-time  
**Impact**: ~512 bytes per boot

**Description**:
```c
status = BS->AllocatePool(EfiLoaderData, file_info_size, (void**)&file_info);
// ... use file_info ...
// Missing: BS->FreePool(file_info);
```

After getting kernel file info, the buffer is never freed before exiting boot services.

**Fix**:
```c
// After line 336, before allocating kernel buffer:
BS->FreePool(file_info);
file_info = NULL;
```

---

### LEAK-003: Kernel Load Buffer Not Freed
**Location**: `boot/loader.c:339-357`  
**Severity**: HIGH  
**Type**: Boot-time  
**Impact**: Kernel size (typically 2-5 MB)

**Description**:
The kernel is read into `kernel_buffer` allocated at line 343, but after parsing ELF and loading segments, the original buffer is never freed. The kernel is copied to proper addresses but the temporary buffer remains allocated.

**Fix**:
```c
// After line 410 (after all segments loaded), before exiting boot services:
BS->FreePages(kernel_addr, pages);
kernel_buffer = NULL;
```

---

## 2. Runtime Leaks - Process Management

### LEAK-004: PID Not Returned to Pool on Error
**Location**: `kernel/core/sched/process.c:36-44`  
**Severity**: MEDIUM  
**Type**: Runtime error path  
**Impact**: 1 PID per failed process_create

**Description**:
```c
uint32_t pid = allocate_pid();  // Line 36

process_t* proc = (process_t*)kmalloc(sizeof(process_t));
if (!proc) {
    // TODO: Return PID to pool  // Line 42
    return NULL;  // PID leaked!
}
```

When `kmalloc` fails, the allocated PID is lost. With only 256 PIDs available, this can exhaust PID space.

**Fix**: Implement `free_pid()` function and call it on all error paths.

```c
static void free_pid(uint32_t pid) {
    // Add PID back to free list or decrement next_pid if possible
    if (pid == next_pid - 1) {
        next_pid--;
    }
    // TODO: Implement proper PID reuse with free list
}

// In process_create error paths:
if (!proc) {
    free_pid(pid);
    return NULL;
}
```

---

### LEAK-005: Kernel Stack Not Freed on Namespace Error
**Location**: `kernel/core/sched/process.c:109-113`  
**Severity**: HIGH  
**Type**: Runtime error path  
**Impact**: 4KB per failed namespace creation

**Description**:
```c
if (!proc->namespaces) {
    kprintf("[PROCESS] Failed to create namespaces for PID %d\n", pid);
    pmm_free_page(proc->kernel_stack);  // ✓ Good
    kfree(proc);  // ✓ Good
    return NULL;  // BUT: PID still leaked!
}
```

While kernel stack and proc structure are properly freed, the PID allocated at line 36 is not returned to the pool.

**Fix**: Add `free_pid(pid);` before `return NULL;`

---

## 3. Runtime Leaks - Namespace System

### LEAK-006: Non-Atomic Reference Counting
**Location**: `kernel/security/namespace.c:73-77, 90-104`  
**Severity**: CRITICAL (race condition)  
**Type**: Runtime (SMP systems)  
**Impact**: Variable - namespaces never freed under race

**Description**:
```c
// Line 73-77: Non-atomic increment
ns->pid_ns->ref_count++;
ns->mount_ns->ref_count++;
ns->net_ns->ref_count++;
ns->ipc_ns->ref_count++;
ns->uts_ns->ref_count++;

// Line 90-104: Non-atomic decrement
if (ns->pid_ns && --ns->pid_ns->ref_count == 0) {
    pid_namespace_destroy(ns->pid_ns);
}
```

On SMP systems, two CPUs could simultaneously decrement from 2 to 1, both seeing non-zero, and namespace never gets destroyed.

**Fix**: Use atomic operations throughout.

```c
__atomic_add_fetch(&ns->pid_ns->ref_count, 1, __ATOMIC_SEQ_CST);

uint32_t old_count = __atomic_sub_fetch(&ns->pid_ns->ref_count, 1, __ATOMIC_SEQ_CST);
if (old_count == 0) {
    pid_namespace_destroy(ns->pid_ns);
}
```

---

### LEAK-007: Namespace Clone Partial Failure
**Location**: `kernel/security/namespace.c:187-220`  
**Severity**: HIGH  
**Type**: Runtime error path  
**Impact**: Variable - depends on which namespace failed

**Description**:
The `error_cleanup` label properly cleans up some namespaces, but if allocation fails midway, earlier namespaces may have incremented refcounts on shared namespaces that are then decremented, but newly created ones may leak.

**Current code audit**: Actually looks correct! Each namespace is checked for whether it was created (CLONE_NEW*) or shared. Shared ones get refcount decremented, created ones get destroyed.

**Status**: FALSE ALARM - Code is correct.

---

## 4. Runtime Leaks - Syscall Handlers

### LEAK-008: sys_write Buffer Cleanup on Error
**Location**: `kernel/core/syscall/handlers.c:132-149`  
**Severity**: LOW  
**Type**: Runtime error path  
**Status**: FIXED ✓

**Description**:
Current code properly frees kernel buffer on all error paths:
```c
char* kernel_buf = kmalloc(count);
if (!kernel_buf) { return ENOMEM; }  // OK - nothing to clean

if (copy_from_user(...) != COPY_SUCCESS) {
    kfree(kernel_buf);  // ✓ Cleaned up
    return EFAULT;
}
// ... normal path ...
kfree(kernel_buf);  // ✓ Cleaned up
```

**Status**: No leak - properly handled.

---

## 5. Runtime Leaks - Driver System

### LEAK-009: Driver Registration Failure
**Location**: `kernel/drivers/core/driver.c:18-41`  
**Severity**: MEDIUM  
**Type**: Runtime error path  
**Impact**: Depends on driver allocations

**Description**:
If `bus_add_driver` fails at line 24, the function returns immediately. However, the driver structure itself might have allocated resources during initialization that are never freed.

**Analysis**: The driver structure is typically statically allocated or managed by caller, so this may not be a leak. Need to check driver initialization patterns.

**Status**: NEEDS REVIEW - Check each driver's init code.

---

## 6. Runtime Leaks - PE Loader

### LEAK-010: PE Parse Allocation on DLL Load
**Location**: `kernel/pe/pe_loader.c:60-71`  
**Severity**: HIGH  
**Type**: Runtime  
**Impact**: Size of PE file per load

**Description**:
```c
pe_file_t *pe = calloc(1, sizeof(pe_file_t));
pe->file_data = malloc(size);
if (!pe->file_data) {
    free(pe);  // ✓ Good
    return NULL;
}
memcpy(pe->file_data, data, size);
```

The PE structure allocates a full copy of the file data. If the PE is loaded, used, and the process exits without calling `pe_free()`, both allocations leak.

**Fix**: Ensure `pe_free()` is called in all process cleanup paths.

**Location to fix**: `kernel/core/sched/process.c` - Add PE cleanup in `process_destroy()` or `process_unref()`.

---

### LEAK-011: PE Parse File - Data Buffer Leak
**Location**: `kernel/pe/pe_loader.c:158-193`  
**Severity**: HIGH  
**Type**: Runtime  
**Impact**: Size of PE file

**Description**:
```c
void *data = malloc(st.st_size);  // Line 174
// ... read file ...
pe_file_t *pe = pe_parse(data, st.st_size);  // Line 189
free(data);  // Line 190 - ✓ Good!
return pe;
```

**Status**: NO LEAK - Data is properly freed after parsing.

---

## 7. Error Path Audit

### Areas with Proper Cleanup ✓

1. **heap.c**: No leaks - heap is self-managing
2. **syscall handlers**: All error paths clean up allocations
3. **process reference counting**: Atomic operations prevent races (BUG-008 fix)
4. **namespace reference counting**: Needs atomic fixes (LEAK-006)

---

## 8. Memory Leak Testing Strategy

### Unit Tests Needed

1. **test_process_create_failures.c**
   - Test kmalloc failure after PID allocation
   - Test namespace creation failure
   - Verify PID is returned to pool
   - Verify kernel stack is freed

2. **test_namespace_refcount.c**
   - Stress test on SMP: create/destroy namespaces from multiple CPUs
   - Verify refcounts never go negative
   - Verify namespaces are eventually freed

3. **test_pe_loader_leak.c**
   - Load PE, exit process, check memory
   - Load 1000 PEs sequentially, verify memory returns to baseline

### Integration Tests Needed

1. **stress_process_lifecycle.c**
   - Create 1000 processes, exit all, repeat 10 times
   - Verify `pmm_get_free_memory()` returns to baseline

2. **stress_namespace_operations.c**
   - Clone/destroy namespaces 10,000 times
   - Verify namespace memory usage returns to baseline

### Valgrind Testing

```bash
# Build with AddressSanitizer
make CFLAGS="-fsanitize=address -g" clean all

# Run in QEMU with ASAN
qemu-system-x86_64 -kernel build/kernel.elf -serial stdio

# Check for leak reports on shutdown
```

---

## 9. Priority Fix List

| Priority | Leak ID | Description | Impact |
|----------|---------|-------------|--------|
| P0 | LEAK-006 | Namespace race condition | Critical - data corruption possible |
| P1 | LEAK-003 | Kernel load buffer | High - 2-5 MB per boot |
| P1 | LEAK-004 | PID leak on error | High - exhausts PID space |
| P1 | LEAK-005 | PID leak on namespace error | High - same as above |
| P1 | LEAK-010 | PE loader allocations | High - per process |
| P2 | LEAK-002 | File info buffer | Medium - 512 bytes per boot |

---

## 10. Automated Detection Tools

### Static Analysis
- **Clang Static Analyzer**: Add to CI pipeline
- **Cppcheck**: Already in CI, tune for memory leak checks
- **Custom scripts**: `tools/memory_leak_checker.sh`

### Dynamic Analysis
- **AddressSanitizer (ASAN)**: Build flag `-fsanitize=address`
- **Valgrind**: Run in VM environment
- **Manual testing**: Long-running stress tests with memory monitoring

---

## 11. Prevention Measures

### Code Review Checklist
- [ ] Every `malloc/kmalloc` has a matching `free/kfree` on all paths
- [ ] Every `allocate_pid()` has a `free_pid()` on error paths
- [ ] Every reference count increment has a corresponding decrement
- [ ] All early returns in functions that allocate have cleanup code
- [ ] Error paths are tested with unit tests

### Coding Standards
1. **RAII-style wrappers**: Consider smart pointer equivalents in C
2. **Cleanup labels**: Use `goto cleanup;` pattern for multi-resource functions
3. **Reference counting**: Always use atomic operations for refcounts
4. **Documentation**: Mark which allocations are intentional (passed to caller)

---

## 12. Next Steps

1. **Fix P0 and P1 leaks immediately** (this session)
2. **Add unit tests for all fixed leaks** (verify fixes work)
3. **Run Valgrind on full boot sequence** (find any remaining leaks)
4. **Add ASAN to CI pipeline** (prevent regressions)
5. **Document ownership model** (who owns each allocation)

---

## Conclusion

**Total Confirmed Leaks**: 6 critical/high priority  
**Total Potential Leaks**: 3 requiring further investigation  
**Estimated Impact**: 2-10 MB per boot/runtime cycle  

All leaks are fixable with straightforward patches. Priority is:
1. Fix namespace race condition (data corruption risk)
2. Fix PID leaks (resource exhaustion)
3. Fix boot-time leaks (memory waste)
4. Add comprehensive testing

**Time to fix all**: ~2-3 hours  
**Time to test thoroughly**: ~4-6 hours

---

*Generated by Memory Leak Hunter*  
*Date: 2026-05-26*
