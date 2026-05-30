# User Page Mapping Verification Report

**Date:** 2026-05-26  
**Task:** Verify init's memory is mapped with USER bit set (U/S=1) for ring 3 access  
**Status:** ✅ CRITICAL BUG FIXED

---

## Executive Summary

Found and fixed a **critical page protection bug** that would have prevented user mode from accessing any memory pages, causing immediate page faults (#PF) when jumping to ring 3.

**Root Cause:** Intermediate page table entries (PML4→PDPT, PDPT→PD, PD→PT) were created without the USER bit, blocking ring 3 access even when final PT entries had USER bit set.

**Impact:** This bug would cause:
- Immediate #PF when init executes first instruction in user mode
- Error code indicating "supervisor page accessed in user mode"
- Complete failure of user mode execution

---

## Verification Results

### ✅ Page Flag Definitions (kernel/include/mem.h)

```c
#define PAGE_PRESENT  0x01  // Page is present in memory
#define PAGE_WRITE    0x02  // Page is writable
#define PAGE_USER     0x04  // Page accessible from ring 3
```

**Status:** Correct

---

### ✅ User Memory Mappings

All user space mappings correctly use PAGE_USER flag:

#### 1. ELF Segment Loading (kernel/fs/exec.c:271)
```c
uint32_t page_flags = PAGE_PRESENT | PAGE_USER;
if (phdr[i].p_flags & PF_W) {
    page_flags |= PAGE_WRITE;
}
vmm_map_page((void*)vaddr, phys_page, page_flags);
```
**Status:** ✅ Correct

#### 2. User Stack (kernel/fs/exec.c:338)
```c
vmm_map_page((void*)vaddr, phys_page, 
             PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
```
**Status:** ✅ Correct

#### 3. ELF Loader Helper (kernel/fs/elf_loader.c:83)
```c
static uint32_t elf_pf_to_page_flags(uint32_t p_flags) {
    uint32_t flags = PAGE_PRESENT | PAGE_USER;
    if (p_flags & PF_W) {
        flags |= PAGE_WRITE;
    }
    return flags;
}
```
**Status:** ✅ Correct

---

### ✅ Kernel Memory Mappings

Kernel memory correctly EXCLUDES PAGE_USER flag:

#### Kernel Heap (kernel/core/mem/heap.c:36)
```c
vmm_map_page((void*)(HEAP_START + i * PAGE_SIZE), phys,
             PAGE_PRESENT | PAGE_WRITE);  // No PAGE_USER
```
**Status:** ✅ Correct - prevents user mode from accessing kernel heap

---

### ❌ CRITICAL BUG FIXED: Intermediate Page Table Entries

**Location:** kernel/arch/x86_64/paging.c:138-217

**Problem:**  
When creating intermediate page table structures for user pages, the code hardcoded flags:
```c
// BEFORE (BROKEN):
kernel_pml4->entries[pml4_idx] = (uint64_t)pdpt | PTE_PRESENT | PTE_WRITE;
pdpt->entries[pdpt_idx] = (uint64_t)pd | PTE_PRESENT | PTE_WRITE;
pd->entries[pd_idx] = (uint64_t)pt | PTE_PRESENT | PTE_WRITE;
```

This violates x86_64 paging rules: **ALL levels of page table hierarchy must have USER bit set for ring 3 access.**

**Fix Applied:**
```c
// AFTER (FIXED):
uint64_t intermediate_flags = PTE_PRESENT | PTE_WRITE;
if (flags & PAGE_USER) {
    intermediate_flags |= PTE_USER;  // Propagate USER bit!
}

kernel_pml4->entries[pml4_idx] = (uint64_t)pdpt | intermediate_flags;
pdpt->entries[pdpt_idx] = (uint64_t)pd | intermediate_flags;
pd->entries[pd_idx] = (uint64_t)pt | intermediate_flags;
```

**Additional Safety Check:**
```c
// Prevent kernel space from being mapped with USER bit
uint64_t virt_addr = (uint64_t)virt;
if (virt_addr >= KERNEL_SPACE_START && (flags & PAGE_USER)) {
    kprintf("[PAGING] ERROR: Attempt to map kernel space %p with USER flag!\n", virt);
    return;
}
```

---

## Technical Background: x86_64 Paging Protection

### Page Table Hierarchy
```
PML4 → PDPT → PD → PT → Physical Page
```

### USER Bit Requirement

For a page to be accessible from ring 3 (user mode), **EVERY level** of the page table walk must have the USER bit (bit 2) set:

- **PML4 entry** → USER bit required
- **PDPT entry** → USER bit required  
- **PD entry** → USER bit required
- **PT entry** → USER bit required

If ANY level lacks the USER bit, CPU triggers #PF with error code:
```
Page fault error code = 0x05
  bit 0 (P)   = 1 (page was present)
  bit 1 (W/R) = 0 (read access)
  bit 2 (U/S) = 1 (user mode access)
```

This means: "Supervisor page accessed from user mode"

---

## Memory Layout Verification

### User Space (0x0000000000000000 - 0x00007FFFFFFFFFFF)
- ✅ Init code segment: Mapped with PAGE_USER
- ✅ Init data segment: Mapped with PAGE_USER + PAGE_WRITE
- ✅ Init BSS segment: Mapped with PAGE_USER + PAGE_WRITE
- ✅ User stack (0x00007FFFFFFFE000): Mapped with PAGE_USER + PAGE_WRITE

### Kernel Space (0xFFFF800000000000 - 0xFFFFFFFFFFFFFFFF)
- ✅ Kernel heap (0xFFFFFFFF90000000): Mapped WITHOUT PAGE_USER
- ✅ Kernel code: Mapped WITHOUT PAGE_USER
- ✅ Kernel data: Mapped WITHOUT PAGE_USER

---

## Test Plan

### Manual Verification Steps

1. **Boot kernel with debug output enabled**
   ```
   make qemu-debug
   ```

2. **Watch for page mapping output**
   ```
   [EXEC]   Loading PT_LOAD segment 0:
   [EXEC]     Virtual address: 0x0000000000400000
   [EXEC]     Flags: R-X
   [PAGING]  Mapping 0x400000 → <phys> with flags 0x05 (PRESENT|USER)
   ```

3. **Verify intermediate tables get USER bit**
   ```
   [PAGING]  Allocating new PDPT for PML4[0]
   [PAGING]  PML4[0] flags: 0x07 (PRESENT|WRITE|USER)  ← Should have USER!
   ```

4. **Watch for ring 3 transition**
   ```
   [EXEC] Jumping to user mode: entry=0x0000000000400000 stack=0x00007FFFFFFFE000
   ```

5. **Verify no page faults on first instruction**
   ```
   # Should NOT see:
   [PANIC] Page fault at 0x400000
   ```

### Expected Behavior

- Init process starts executing at entry point
- First instruction fetch succeeds (no #PF)
- Stack access succeeds (no #PF)
- System calls work (ring 3 → ring 0 transitions)

### Failure Modes (Before Fix)

Without the fix, you would see:
```
[EXEC] Jumping to user mode: entry=0x400000 stack=0x7fffffffe000
[PANIC] Page Fault Exception!
  RIP: 0x0000000000400000
  CR2: 0x0000000000400000
  Error code: 0x0005 (present, user-mode, read)
```

This indicates CPU tried to fetch instruction from user space, but page tables lacked USER bit.

---

## Files Modified

### kernel/arch/x86_64/paging.c
- ✅ Added intermediate_flags calculation (line 150-153)
- ✅ Added kernel space protection check (line 144-148)
- ✅ Applied intermediate_flags to PML4, PDPT, PD entries (lines 164, 177, 191, 208)

---

## Success Criteria Met

- [x] Init's code/data/stack mapped with PAGE_USER
- [x] Kernel memory mapped without PAGE_USER
- [x] Intermediate page table entries propagate USER bit
- [x] Kernel space protection enforced (no USER bit allowed)
- [x] Page flags correctly defined (PAGE_USER = 0x04)
- [x] All ELF loading code uses correct flags

---

## Conclusion

The critical page protection bug has been identified and fixed. All user space pages now have the USER bit set at **all levels** of the page table hierarchy, allowing ring 3 (user mode) to access them. Kernel space remains protected from user access.

**Next Steps:**
1. Build and test the kernel
2. Verify init executes without page faults
3. Monitor for any protection violations
4. Add automated tests for page flag propagation

---

## References

- Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 3A, Chapter 4.6 "Access Rights"
- x86_64 Page Table Entry Format (bits 0-11 are flags)
- kernel/include/mem.h - Page flag definitions
- kernel/arch/x86_64/paging.c - Page table management
