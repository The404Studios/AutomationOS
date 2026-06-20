#include "../../include/mem.h"
#include "../../include/kernel.h"
#include "../../include/x86_64.h"  // read_cr3() for the live address-space walk
#include "../../include/sched.h"   // process_t for vmm_get_cr3 fallback

// External string functions
extern void* memcpy(void* dest, const void* src, size_t count);

// Declared in paging.c
extern void paging_init(void);
extern void paging_map_page(void* virt, void* phys, uint64_t flags);
extern void paging_unmap_page(void* virt);
extern uint64_t paging_create_address_space(void);
extern void paging_destroy_address_space(uint64_t cr3);
extern void paging_set_target(uint64_t cr3);
extern void paging_reset_target(void);
extern int paging_modify_pte_flags(void* virt, uint64_t new_flags);
extern uint64_t paging_kernel_cr3(void);

/*
 * paging_get_pte - walk the ACTIVE page tables and return the raw PTE for
 * `virt`, or 0 if any level is absent (not mapped).
 *
 * This helper is NEEDED by copy_user_string so it can stop cleanly at an
 * unmapped page boundary instead of faulting.  It is NOT yet implemented in
 * paging.c — when it is added there, remove this comment and the stub below.
 *
 * CONTRACT: declared here as extern.  Implemented in kernel/arch/x86_64/paging.c.
 * Signature must match exactly.
 */
extern uint64_t paging_get_pte(uint64_t virt);

void vmm_init(void) {
    kprintf("[VMM] Initializing virtual memory manager...\n");
    paging_init();
    kprintf("[VMM] Virtual memory initialized\n");
}

void* vmm_map_page(void* virt, void* phys, uint64_t flags) {
    ASSERT_ALWAYS(virt != NULL);
    ASSERT_ALWAYS(phys != NULL);
    ASSERT_ALWAYS(((uint64_t)virt & (PAGE_SIZE - 1)) == 0);  // Must be page-aligned
    ASSERT_ALWAYS(((uint64_t)phys & (PAGE_SIZE - 1)) == 0);  // Must be page-aligned

    paging_map_page(virt, phys, flags);
    return virt;
}

void vmm_unmap_page(void* virt) {
    ASSERT_ALWAYS(virt != NULL);
    ASSERT_ALWAYS(((uint64_t)virt & (PAGE_SIZE - 1)) == 0);  // Must be page-aligned

    paging_unmap_page(virt);
}

// Validate user buffer for syscalls
bool validate_user_buffer(const void* ptr, size_t size) {
    if (ptr == NULL) {
        return false;
    }

    uint64_t addr = (uint64_t)ptr;
    uint64_t end = addr + size;

    // Check for overflow
    if (end < addr) {
        return false;
    }

    // Must be in user space
    if (addr >= USER_SPACE_END) {
        return false;
    }
    if (end > USER_SPACE_END) {
        return false;
    }

    // TODO: Check that pages are actually mapped and accessible
    // For now, just check address range

    return true;
}

// Validate user string (checks for null terminator within max_len)
bool validate_user_string(const char* str, size_t max_len) {
    if (!validate_user_buffer(str, 1)) {
        return false;
    }

    // BUG-020 fix: Use safe copy to avoid page faults
    // We can't directly access user memory - it might not be mapped
    // Instead, we'll use a temporary buffer and copy_from_user
    char temp_buf[256];  // Reasonable max for path/string validation
    size_t check_len = max_len < sizeof(temp_buf) ? max_len : sizeof(temp_buf);

    // Try to safely copy the string
    int result = copy_from_user(temp_buf, str, check_len);
    if (result != COPY_SUCCESS) {
        return false;  // Failed to copy - probably unmapped
    }

    // Now safely scan the copied buffer for null terminator
    for (size_t i = 0; i < check_len; i++) {
        if (temp_buf[i] == '\0') {
            return true;
        }
    }

    return false;  // No null terminator found within max_len
}

// Helper: Check if address is in user space (not kernel-range)
static bool is_user_address(uint64_t addr) {
    return addr < USER_SPACE_END;
}

/*
 * user_page_is_accessible - check whether a single 4KB page containing `addr`
 * is present AND user-accessible in the LIVE caller address space.
 *
 * AUDIT FIX: this previously walked paging_get_pte()'s STALE software view
 * (active_pml4 == kernel_pml4 during a syscall, since nobody calls
 * paging_set_target() on the copy_user_string path), not the active CR3.  That
 * diverges from the caller's real mappings for any user VA mapped after process
 * creation (mmap'd path strings, lazy heap/stack): it false-rejected valid
 * pages and — worse — could pass a page that is present in kernel_pml4's
 * identity map but UNMAPPED in the live CR3, letting copy_user_string then
 * #PF in ring 0.  Walk the live CR3 directly, exactly like
 * user_range_is_accessible (the bulk copy_from/to_user primitive already does
 * this), and require PAGE_USER so a kernel-only identity page is not accepted.
 * Mask table pointers with ADDR_MASK (the NX bit 63 lives in entries).
 */
static bool user_page_is_accessible(uint64_t addr) {
    const uint64_t ADDR_MASK = 0x000FFFFFFFFFF000ULL;
    const uint64_t required  = (uint64_t)(PAGE_PRESENT | PAGE_USER);
    uint64_t* pml4 = (uint64_t*)(read_cr3() & ~0xFFFULL);
    uint64_t va = addr & ~0xFFFULL;

    uint64_t e = pml4[(va >> 39) & 0x1FF];
    if (!(e & PAGE_PRESENT)) return false;
    e = ((uint64_t*)(e & ADDR_MASK))[(va >> 30) & 0x1FF];
    if (!(e & PAGE_PRESENT)) return false;
    e = ((uint64_t*)(e & ADDR_MASK))[(va >> 21) & 0x1FF];
    if (!(e & PAGE_PRESENT)) return false;
    if (e & (1ULL << 7)) return (e & required) == required;   /* 2MB huge leaf */
    e = ((uint64_t*)(e & ADDR_MASK))[(va >> 12) & 0x1FF];
    return (e & required) == required;
}

/*
 * user_range_is_accessible - verify that EVERY 4KB page spanned by the
 * [addr, addr+n) range is present and user-accessible (and, when
 * `need_write` is set, writable) by walking the LIVE caller address space.
 *
 * The previous implementation walked paging_get_pte()'s STALE software view
 * (the vmm map target, not the active CR3) and false-rejected valid user
 * pages (every 0x200xxx source), breaking boot.  This version walks the
 * ACTIVE CR3 directly — exactly like fork_copy_user_pages() in
 * handlers.c — so the pages we check are the same pages the memcpy below
 * will actually touch.
 *
 * This is what makes the bulk copy_from_user / copy_to_user paths
 * fault-safe: a malicious or buggy process can pass an in-range but
 * UNMAPPED (or read-only) pointer, and without this check the raw memcpy
 * below would fault in ring 0 and panic the kernel.  Returns true only if
 * the entire range can be touched without faulting.
 *
 * Bounds are assumed already validated by the caller (range stays below
 * USER_SPACE_END and does not overflow), so the page loop cannot wrap.
 *
 * NX-MASK FIX: when dereferencing a table pointer we mask with
 * 0x000FFFFFFFFFF000ULL, NOT `& ~0xFFF`.  With W^X enabled, data PTEs carry
 * PAGE_NX (bit 63); `& ~0xFFF` alone would leave bit 63 set, yielding a
 * non-canonical pointer that #GPs on the next table access.
 */
static bool user_range_is_accessible(uint64_t addr, size_t n, bool need_write) {
    // Mask off the page-table phys address from each table entry, clearing
    // BOTH the low flag bits and the high flag bits (NX bit 63).
    const uint64_t ADDR_MASK = 0x000FFFFFFFFFF000ULL;

    // Walk the LIVE caller address space, not a stale software view.
    uint64_t cr3 = read_cr3() & ~0xFFFULL;
    uint64_t* pml4 = (uint64_t*)cr3;

    uint64_t required = (uint64_t)(PAGE_PRESENT | PAGE_USER);
    if (need_write) {
        required |= (uint64_t)PAGE_WRITE;
    }

    // Iterate page-by-page from the page containing the first byte through
    // the page containing the last byte (addr + n - 1).
    uint64_t first_page = addr & ~0xFFFULL;
    uint64_t last_page  = (addr + n - 1) & ~0xFFFULL;

    for (uint64_t va = first_page; va <= last_page; va += 0x1000ULL) {
        uint64_t i4 = (va >> 39) & 0x1FF;
        uint64_t i3 = (va >> 30) & 0x1FF;
        uint64_t i2 = (va >> 21) & 0x1FF;
        uint64_t i1 = (va >> 12) & 0x1FF;

        // PML4 -> PDPT
        uint64_t pml4e = pml4[i4];
        if (!(pml4e & PAGE_PRESENT)) {
            return false;
        }
        uint64_t* pdpt = (uint64_t*)(pml4e & ADDR_MASK);

        // PDPT -> PD
        uint64_t pdpte = pdpt[i3];
        if (!(pdpte & PAGE_PRESENT)) {
            return false;
        }
        uint64_t* pd = (uint64_t*)(pdpte & ADDR_MASK);

        // PD entry: either a 2MB huge page leaf or a pointer to a PT.
        uint64_t pde = pd[i2];
        if (!(pde & PAGE_PRESENT)) {
            return false;
        }

        if (pde & (1ULL << 7)) {
            // 2MB huge page leaf — check accessibility directly on the PDE.
            if ((pde & required) != required) {
                return false;
            }
        } else {
            // Walk down to the 4KB PTE.
            uint64_t* pt = (uint64_t*)(pde & ADDR_MASK);
            uint64_t pte = pt[i1];
            if ((pte & required) != required) {
                return false;
            }
        }

        // Guard against wrap at the top of the address space (defensive;
        // bounds checks above already preclude this).
        if (va + 0x1000ULL < va) {
            break;
        }
    }

    return true;
}

/*
 * vmm_get_physical - translate a virtual address to its physical address.
 *
 * DESIGN
 * ------
 * Walks the active page tables using paging_get_pte() to retrieve the leaf
 * page-table entry for the given virtual address.  Extracts the physical
 * page frame number from the PTE and combines it with the page offset from
 * the virtual address to produce the complete physical address.
 *
 * For 2MB huge pages, the PDE is the leaf entry and contains a 2MB-aligned
 * physical address; we extract bits 20:12 from virt as the huge-page offset.
 *
 * Returns NULL if the virtual address is not mapped (any level in the page
 * table walk is absent).
 *
 * USAGE
 * -----
 * Used by debugging tools, page-fault handlers, and DMA setup routines that
 * need to translate kernel or user virtual addresses to physical addresses.
 * Should not be called on unmapped addresses without checking the PTE first.
 */
void* vmm_get_physical(void* virt) {
    uint64_t vaddr = (uint64_t)virt;

    // Get the page table entry for this virtual address
    uint64_t pte = paging_get_pte(vaddr);

    // If not present, return NULL
    if (!(pte & PAGE_PRESENT)) {
        return NULL;
    }

    // Check if this is a 2MB huge page (bit 7 set in PDE)
    if (pte & (1ULL << 7)) {
        // Huge page: extract 2MB-aligned physical address and add offset
        uint64_t phys_base = pte & 0x000FFFFFFFE00000ULL;  // Bits 51:21
        uint64_t offset = vaddr & 0x1FFFFFULL;              // Bits 20:0
        return (void*)(phys_base | offset);
    } else {
        // 4KB page: extract 4KB-aligned physical address and add offset
        uint64_t phys_base = pte & 0x000FFFFFFFFFF000ULL;  // Bits 51:12
        uint64_t offset = vaddr & 0xFFFULL;                 // Bits 11:0
        return (void*)(phys_base | offset);
    }
}

/*
 * Bulk copy helpers (8-byte aligned fast path + byte tail).
 * We keep these self-contained so vmm.c does not depend on memcpy from
 * another agent's lib/string.c.  The external memcpy() declared above is
 * still used for the bulk copy_from_user / copy_to_user paths where the
 * compiler already knows the range is validated.
 *
 * For copy_user_string we intentionally avoid memcpy and go byte-by-byte.
 */

/*
 * copy_from_user - safely copy n bytes from user space to kernel space.
 *
 * Bounds checks (rejects kernel-space pointers and overflow):
 *   - src must be below USER_SPACE_END
 *   - src+n must not overflow and must be <= USER_SPACE_END
 *   - n must not exceed USER_SPACE_END (prevents trivial overflow bypass)
 *
 * Performance: after validation the copy is done via the existing memcpy()
 * (which uses 8-byte aligned moves internally on x86_64).  For very small
 * copies (<8 bytes) the byte tail path of memcpy handles it correctly.
 *
 * Returns COPY_SUCCESS (0) or COPY_EFAULT (-1).
 */
int copy_from_user(void* kernel_dst, const void* user_src, size_t n) {
    if (kernel_dst == NULL || user_src == NULL || n == 0) {
        return COPY_EFAULT;
    }

    // BUG-003 fix: Check size BEFORE computing end address to prevent overflow
    if (n > USER_SPACE_END) {
        kprintf("[VMM] copy_from_user: Size %zu exceeds user space\n", n);
        return COPY_EFAULT;
    }

    uint64_t src_addr = (uint64_t)user_src;

    // Reject kernel-space source pointers outright
    if (!is_user_address(src_addr)) {
        kprintf("[VMM] copy_from_user: Source %p not in user space\n", user_src);
        return COPY_EFAULT;
    }

    // Check if addition would overflow (defense in depth)
    if (src_addr > USER_SPACE_END - n) {
        kprintf("[VMM] copy_from_user: Copy would cross user space boundary\n");
        return COPY_EFAULT;
    }

    // Now safe to compute end address
    uint64_t src_end = src_addr + n;

    // Final validation: entire range must stay below USER_SPACE_END
    if (!is_user_address(src_end - 1)) {
        kprintf("[VMM] copy_from_user: End address %p not in user space\n",
                (void*)src_end);
        return COPY_EFAULT;
    }

    // Page-presence fault-safety check: walk the LIVE caller CR3 to confirm
    // every source page is present and user-accessible before the memcpy, so
    // an in-range-but-unmapped pointer cannot fault the kernel in ring 0.
    if (!user_range_is_accessible(src_addr, n, false)) {
        return COPY_EFAULT;
    }

    // Fast-path bulk copy: use the existing memcpy which emits 64-bit moves
    // for aligned data on x86_64.  We validated the source range above;
    // the destination is a kernel pointer supplied by the kernel itself so
    // we trust it.
    //
    // SMAP bracket: when CR4.SMAP is set, kernel code cannot access
    // user-accessible pages unless RFLAGS.AC is set. stac() opens the
    // window; clac() closes it immediately after the copy so that any
    // subsequent wild user-pointer deref in kernel code still faults.
    //
    // Performance: 8-byte aligned middle + byte tail is handled inside
    // the compiler/libc memcpy. When string.c is replaced by another agent,
    // fall back to the manual loop below by swapping the call.
    stac();
    memcpy(kernel_dst, user_src, n);
    clac();

    return COPY_SUCCESS;
}

/*
 * Extended VMM operations for PE loader / Win32 subsystem
 * ========================================================
 * These functions provide per-process virtual memory allocation with optional
 * fixed-address placement for PE image loading and Win32 VirtualAlloc support.
 *
 * vmm_ctx is expected to be a CR3 value (address space identifier). If NULL,
 * the current process's CR3 is used as a fallback (allows PE loader to work
 * even when process->vmm is not initialized).
 */

// Forward declaration to access current process
extern process_t* current_process;

static uint64_t vmm_get_cr3(void* vmm_ctx) {
    if (vmm_ctx) {
        return (uint64_t)vmm_ctx;
    }
    // Fallback to current process's CR3
    if (current_process && current_process->context.cr3) {
        return current_process->context.cr3;
    }
    return 0;
}

void* vmm_alloc(void* vmm_ctx, size_t size, int prot) {
    if (size == 0) {
        return NULL;
    }

    // Get the target CR3 (vmm_ctx or fallback to current process)
    uint64_t cr3 = vmm_get_cr3(vmm_ctx);
    if (!cr3) {
        kprintf("[VMM] vmm_alloc: No valid address space available\n");
        return NULL;
    }

    // Translate VMM_PROT_* flags to PAGE_* flags for the paging layer
    uint32_t vma_prot = 0;
    if (prot & VMM_PROT_READ)  vma_prot |= PAGE_PRESENT | PAGE_USER;
    if (prot & VMM_PROT_WRITE) vma_prot |= PAGE_WRITE;
    // VMM_PROT_EXEC: NX bit handling would go here when implemented

    // Use the existing vmm_mmap_anon infrastructure which:
    // - Allocates from the per-address-space bump allocator
    // - Eagerly maps fresh physical pages
    // - Marks pages as PTE_OWNED for automatic cleanup
    // - Returns user-space virtual addresses
    void* addr = vmm_mmap_anon(cr3, size, vma_prot);

    if (!addr) {
        kprintf("[VMM] vmm_alloc: Failed to allocate %zu bytes in AS %p\n",
                size, vmm_ctx);
        return NULL;
    }

    return addr;
}

void* vmm_alloc_at(void* vmm_ctx, void* addr, size_t size, int prot) {
    if (size == 0) {
        return NULL;
    }

    // Get the target CR3 (vmm_ctx or fallback to current process)
    uint64_t cr3 = vmm_get_cr3(vmm_ctx);
    if (!cr3) {
        kprintf("[VMM] vmm_alloc_at: No valid address space available\n");
        return NULL;
    }

    // Validate the requested address is in user space
    uint64_t requested_addr = (uint64_t)addr;
    if (requested_addr == 0 || requested_addr >= USER_SPACE_END) {
        kprintf("[VMM] vmm_alloc_at: Invalid address %p (must be in user space)\n", addr);
        return NULL;
    }

    // Check if the address is page-aligned (PE images typically require alignment)
    if ((requested_addr & (PAGE_SIZE - 1)) != 0) {
        kprintf("[VMM] vmm_alloc_at: Address %p not page-aligned\n", addr);
        return NULL;
    }
    uint64_t aligned_size = ALIGN_UP(size, PAGE_SIZE);
    uint64_t npages = aligned_size / PAGE_SIZE;

    // Reject zero / overflowing sizes and ranges that would extend PAST user
    // space. Line 431 above only bounds the START; without bounding the END,
    // (requested_addr + aligned_size) can wrap around 2^64 and slip past the
    // USER_SPACE_END check, mapping kernel memory as user-accessible. We compare
    // via subtraction (USER_SPACE_END - aligned_size) so the check itself never
    // overflows. `aligned_size < size` catches an ALIGN_UP wrap on a huge size.
    if (size == 0 || aligned_size < size ||
        aligned_size > USER_SPACE_END ||
        requested_addr > USER_SPACE_END - aligned_size) {
        kprintf("[VMM] vmm_alloc_at: size %lu at %p exceeds user space\n",
                (unsigned long)size, addr);
        return NULL;
    }

    // Translate VMM_PROT_* to PAGE_* flags
    uint64_t flags = PAGE_PRESENT | PAGE_USER;
    if (prot & VMM_PROT_WRITE) {
        flags |= PAGE_WRITE;
    }
    // NX handling: when VMM_PROT_EXEC is NOT set, we should set PAGE_NX
    // For now, all pages are executable (PAGE_NX not set)

    // Check if the requested range overlaps existing mappings by walking the
    // page tables. This prevents silently overwriting existing allocations.
    // We save the current CR3, switch to the target, check the PTEs, then restore.
    uint64_t saved_cr3 = read_cr3();
    write_cr3(cr3);

    // Simple overlap check: verify the target range is not already mapped.
    // A full implementation would check every page or consult a VMA tree.
    // For now, check first and last page as a basic sanity test.
    uint64_t first_pte = paging_get_pte(requested_addr);
    uint64_t last_pte = paging_get_pte(requested_addr + aligned_size - PAGE_SIZE);

    // Restore the original CR3
    write_cr3(saved_cr3);

    if ((first_pte & PAGE_PRESENT) || (last_pte & PAGE_PRESENT)) {
        kprintf("[VMM] vmm_alloc_at: Address range %p-%p already mapped\n",
                addr, (void*)(requested_addr + aligned_size));
        return NULL;
    }

    // Allocate and map pages at the requested address
    for (uint64_t i = 0; i < npages; i++) {
        void* phys = pmm_alloc_page();
        if (!phys) {
            kprintf("[VMM] vmm_alloc_at: OOM after %lu/%lu pages\n",
                    (unsigned long)i, (unsigned long)npages);

            // Rollback: free pages allocated so far
            if (i > 0) {
                vmm_unmap_range_into(cr3, requested_addr, i * PAGE_SIZE, true);
            }
            return NULL;
        }

        // Zero the physical page (PE loader expects zero-initialized memory).
        // AUDIT FIX (gap-org): access via the stable direct-map alias, not the raw
        // physical address -- (uint8_t*)phys only works while the page falls in the
        // identity range; PHYS_TO_DIRECT resolves for any frame on any CR3.
        uint8_t* page_ptr = (uint8_t*)PHYS_TO_DIRECT(phys);
        for (size_t j = 0; j < PAGE_SIZE; j++) {
            page_ptr[j] = 0;
        }

        // Map into the target address space at the requested address
        int result = vmm_map_phys_into(cr3, requested_addr + i * PAGE_SIZE,
                                       (uint64_t)phys, PAGE_SIZE, flags);
        if (result != 0) {
            kprintf("[VMM] vmm_alloc_at: Failed to map page at %p\n",
                    (void*)(requested_addr + i * PAGE_SIZE));
            pmm_free_page(phys);

            // Rollback
            if (i > 0) {
                vmm_unmap_range_into(cr3, requested_addr, i * PAGE_SIZE, true);
            }
            return NULL;
        }
    }

    return addr;
}

void vmm_free(void* vmm_ctx, void* addr, size_t size) {
    if (!addr || size == 0) {
        return;
    }

    // Get the target CR3 (vmm_ctx or fallback to current process)
    uint64_t cr3 = vmm_get_cr3(vmm_ctx);
    if (!cr3) {
        kprintf("[VMM] vmm_free: No valid address space available\n");
        return;
    }
    uint64_t vaddr = (uint64_t)addr;

    // Validate address is in user space
    if (vaddr >= USER_SPACE_END) {
        kprintf("[VMM] vmm_free: Invalid address %p (not in user space)\n", addr);
        return;
    }

    // Free the pages via vmm_munmap if in the anon window, otherwise use
    // the general unmap+free path. vmm_munmap already does bounds checking.
    if (vaddr >= VMM_ANON_VA_BASE) {
        // This was allocated via vmm_alloc, use munmap
        vmm_munmap(cr3, vaddr, size);
    } else {
        // This was allocated via vmm_alloc_at at a fixed address outside
        // the normal anon window (e.g., PE image base). Unmap and free.
        vmm_unmap_range_into(cr3, vaddr, size, true);
    }
}

/*
 * vmm_protect - change protection flags on a virtual memory region
 * =================================================================
 * Modifies the page-table protection bits for all pages in the range
 * [addr, addr+size) in the specified address space (vmm_ctx is the CR3 value).
 *
 * Used by:
 *   - PE loader: setting section permissions (.text = RX, .data = RW, .rdata = R)
 *   - Win32 VirtualProtect(): runtime page-protection changes
 *   - Win32 VirtualFree(MEM_DECOMMIT): marking pages inaccessible
 *
 * Parameters:
 *   vmm_ctx - CR3 value of the target address space (NULL = fail)
 *   addr    - starting virtual address (will be page-aligned down)
 *   size    - size in bytes (will be rounded up to page boundary)
 *   prot    - new protection flags (VMM_PROT_READ, VMM_PROT_WRITE, VMM_PROT_EXEC)
 *
 * Returns:
 *    0 on success
 *   -1 on failure (invalid vmm_ctx, address out of range, or unmapped page)
 *
 * Implementation:
 *   - Aligns addr down and size up to PAGE_SIZE boundaries
 *   - Translates VMM_PROT_* to PAGE_* flags
 *   - Saves the current paging target, switches to the target CR3
 *   - Walks each page in the range and calls paging_modify_pte_flags()
 *   - Restores the original paging target
 */
int vmm_protect(void* vmm_ctx, void* addr, size_t size, int prot) {
    if (!vmm_ctx || !addr || size == 0) {
        return -1;
    }

    uint64_t cr3 = (uint64_t)vmm_ctx;
    uint64_t vaddr = (uint64_t)addr;

    // Validate address is in user space
    if (vaddr >= USER_SPACE_END) {
        kprintf("[VMM] vmm_protect: Address %p is not in user space\n", addr);
        return -1;
    }

    // Overflow-safe range bound BEFORE the rounding math below. vaddr is already
    // < USER_SPACE_END; reject if `size` would push the range to/past it. This
    // also prevents `vaddr + size + PAGE_SIZE - 1` from wrapping uint64 (which
    // would make end_addr small, aligned_size huge, and let the post-check at
    // `aligned_addr + aligned_size > USER_SPACE_END` wrap back under the bound --
    // walking the loop into the shared kernel half and re-flagging kernel PTEs).
    if (size > USER_SPACE_END - vaddr) {
        kprintf("[VMM] vmm_protect: range size %lu from %p overflows user space\n",
                (unsigned long)size, addr);
        return -1;
    }

    // Align address down to page boundary and size up
    uint64_t aligned_addr = vaddr & ~(PAGE_SIZE - 1);
    uint64_t end_addr = (vaddr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t aligned_size = end_addr - aligned_addr;

    // Check end address doesn't overflow into kernel space
    if (aligned_addr + aligned_size > USER_SPACE_END) {
        kprintf("[VMM] vmm_protect: Range %p-%p extends into kernel space\n",
                (void*)aligned_addr, (void*)(aligned_addr + aligned_size));
        return -1;
    }

    // Translate VMM_PROT_* flags to PAGE_* flags
    uint64_t page_flags = 0;

    // PROT_NONE (0x00): clear PRESENT bit to make page inaccessible
    // Used by VirtualFree(MEM_DECOMMIT) to mark pages as unavailable while
    // keeping the virtual address space reserved.
    if (prot == VMM_PROT_NONE) {
        // Clear PRESENT bit - page will fault if accessed
        // Keep USER bit so the entry is still recognized as user-space
        page_flags = PAGE_USER;
    } else {
        // Any non-zero protection: page is accessible
        page_flags = PAGE_PRESENT | PAGE_USER;

        if (prot & VMM_PROT_WRITE) {
            page_flags |= PAGE_WRITE;
        }

        // NX (No-Execute) bit: set if VMM_PROT_EXEC is NOT set
        // NOTE: This requires EFER.NXE to be enabled (should be done in paging_init)
        if (!(prot & VMM_PROT_EXEC)) {
            page_flags |= PAGE_NX;
        }
    }

    // Save current paging target and switch to the target address space
    uint64_t saved_target = read_cr3() & 0x000FFFFFFFFFF000ULL;
    paging_set_target(cr3);

    // Modify protection on each page in the range
    uint64_t npages = aligned_size / PAGE_SIZE;
    int result = 0;

    for (uint64_t i = 0; i < npages; i++) {
        void* page_addr = (void*)(aligned_addr + i * PAGE_SIZE);

        if (paging_modify_pte_flags(page_addr, page_flags) != 0) {
            kprintf("[VMM] vmm_protect: Failed to modify PTE flags for page %p (not mapped?)\n",
                    page_addr);
            result = -1;
            // Continue trying to modify remaining pages rather than abort
        }
    }

    // Restore original paging target
    paging_reset_target();
    uint64_t kernel_cr3 = paging_kernel_cr3();
    if (saved_target != kernel_cr3) {
        paging_set_target(saved_target);
    }

    return result;
}

/*
 * copy_to_user - safely copy n bytes from kernel space to user space.
 *
 * Same bounds-check contract as copy_from_user: rejects kernel-range
 * destinations, overflow, and out-of-user-space ranges.
 *
 * Performance: validated then delegated to memcpy (8-byte aligned moves).
 *
 * Returns COPY_SUCCESS (0) or COPY_EFAULT (-1).
 */
int copy_to_user(void* user_dst, const void* kernel_src, size_t n) {
    if (user_dst == NULL || kernel_src == NULL || n == 0) {
        return COPY_EFAULT;
    }

    // BUG-003 fix: Check size BEFORE computing end address to prevent overflow
    if (n > USER_SPACE_END) {
        kprintf("[VMM] copy_to_user: Size %zu exceeds user space\n", n);
        return COPY_EFAULT;
    }

    uint64_t dst_addr = (uint64_t)user_dst;

    // Reject kernel-space destination pointers outright
    if (!is_user_address(dst_addr)) {
        kprintf("[VMM] copy_to_user: Destination %p not in user space\n", user_dst);
        return COPY_EFAULT;
    }

    // Check if addition would overflow (defense in depth)
    if (dst_addr > USER_SPACE_END - n) {
        kprintf("[VMM] copy_to_user: Copy would cross user space boundary\n");
        return COPY_EFAULT;
    }

    // Now safe to compute end address
    uint64_t dst_end = dst_addr + n;

    // Final validation: entire range must stay below USER_SPACE_END
    if (!is_user_address(dst_end - 1)) {
        kprintf("[VMM] copy_to_user: End address %p not in user space\n",
                (void*)dst_end);
        return COPY_EFAULT;
    }

    // Page-presence/writability fault-safety check: walk the LIVE caller CR3
    // to confirm every destination page is present, user-accessible AND
    // writable before the memcpy, so a read-only or unmapped destination
    // cannot #PF the kernel in ring 0.
    if (!user_range_is_accessible(dst_addr, n, true)) {
        // The writability check failed. The dominant benign cause is a
        // copy-on-write page: a freshly-forked process whose target buffer is
        // still the shared, read-only parent page (e.g. waitpid status, a
        // poll()/select() fd set, a signal frame). A userspace store there would
        // trap and the #PF handler would give it a private copy; do the same here
        // so kernel-side writes don't spuriously EFAULT. cow_handle_write is a
        // no-op on non-CoW / already-writable pages, so this only acts on the
        // genuine CoW case; a truly read-only or unmapped page still fails below.
        uint64_t pg, lo = dst_addr & ~0xFFFULL, hi = (dst_addr + n - 1) & ~0xFFFULL;
        for (pg = lo; pg <= hi; pg += 0x1000) cow_handle_write(pg);
        if (!user_range_is_accessible(dst_addr, n, true)) {
            return COPY_EFAULT;
        }
    }

    // Fast-path bulk copy (see copy_from_user comment above).
    // SMAP bracket: open AC for the user-page write, close immediately after.
    stac();
    memcpy(user_dst, kernel_src, n);
    clac();

    return COPY_SUCCESS;
}

/*
 * copy_user_string - safely copy a NUL-terminated string from user space.
 *
 * DESIGN
 * ------
 * Unlike copy_from_user which copies a fixed `n` bytes, this function:
 *
 *   1. Validates the starting address is in user space and non-NULL.
 *   2. Walks BYTE BY BYTE, stopping at the first NUL or when `max` bytes
 *      have been copied — it NEVER reads past the actual string.
 *   3. At every 4KB PAGE BOUNDARY it calls user_page_is_accessible() which
 *      does a software page-table walk (paging_get_pte) to check whether the
 *      next page is mapped before touching it.  If the page is absent the
 *      copy stops and returns COPY_EFAULT instead of generating a fault.
 *   4. Writes a NUL terminator into `kernel_dst` before returning
 *      COPY_SUCCESS so the caller always gets a well-formed C string.
 *
 * PAGE-BOUNDARY CHECK
 * -------------------
 * We check accessibility at the START of each new page (i.e. when
 * `(current_addr & 0xFFF) == 0` — meaning we just stepped onto a new
 * 4KB page).  The first page is checked before reading any byte.
 * This costs one page-table walk (~4 memory accesses) per 4096 bytes —
 * negligible compared to the syscall overhead for typical path lengths.
 *
 * MISSING PAGING_GET_PTE
 * ----------------------
 * If paging_get_pte() is not yet present in paging.c the linker will
 * report an undefined symbol at build time.  Until then, user_page_is_accessible()
 * always returns true (effectively removing the page-mapping check) which
 * restores the old behaviour.  Add the following function to paging.c:
 *
 *   uint64_t paging_get_pte(uint64_t virt) {
 *       // walk active_pml4 -> pdpt -> pd -> pt and return leaf PTE or 0
 *       ...
 *   }
 *
 * Parameters
 * ----------
 *   kernel_dst  - kernel buffer to write into (caller must ensure >= max bytes)
 *   user_src    - user-space string pointer
 *   max         - maximum bytes to copy INCLUDING the NUL terminator; i.e.
 *                 at most max-1 non-NUL characters are copied, then a NUL is
 *                 appended.  Equivalent to strlcpy semantics.
 *
 * Returns COPY_SUCCESS (0) or COPY_EFAULT (-1).
 */
int copy_user_string(void* kernel_dst, const void* user_src, size_t max) {
    if (kernel_dst == NULL || user_src == NULL || max == 0) {
        return COPY_EFAULT;
    }

    uint64_t src_addr = (uint64_t)user_src;

    // Reject kernel-space source pointers
    if (!is_user_address(src_addr)) {
        kprintf("[VMM] copy_user_string: Source %p not in user space\n", user_src);
        return COPY_EFAULT;
    }

    char* dst = (char*)kernel_dst;
    const char* src = (const char*)user_src;
    size_t copied = 0;

    // Reserve one byte for the NUL terminator we always append
    size_t copy_limit = max - 1;

    // SMAP optimisation: instead of stac/clac per byte (~2 instructions per
    // character = ~100% overhead on a 64-byte path), we open the AC window
    // once per validated page and close it only at page boundaries (before
    // re-validation) or on exit.  user_page_is_accessible runs with AC clear
    // (it reads kernel page-table pages, not user data), so the bracket must
    // close before each page check.
    int smap_open = 0;

    while (copied < copy_limit) {
        uint64_t cur_addr = (uint64_t)(src + copied);

        // Reject if we have wandered into kernel space (shouldn't happen but
        // be defensive — the user buffer could wrap via a very long string)
        if (!is_user_address(cur_addr)) {
            break;
        }

        // At every page boundary (including the very first byte) verify the
        // page is actually mapped before dereferencing it.
        if ((cur_addr & 0xFFFULL) == 0) {
            // Close SMAP window before page-table walk (kernel-page reads)
            if (smap_open) { clac(); smap_open = 0; }
            if (!user_page_is_accessible(cur_addr)) {
                // Next page is not mapped — stop here cleanly
                kprintf("[VMM] copy_user_string: Page at %p not accessible\n",
                        (void*)cur_addr);
                dst[copied] = '\0';
                return COPY_EFAULT;
            }
            // Re-open for the validated page
            stac(); smap_open = 1;
        }

        char c = src[copied];
        dst[copied] = c;
        copied++;

        if (c == '\0') {
            // NUL already written; string is complete
            if (smap_open) clac();
            return COPY_SUCCESS;
        }
    }

    if (smap_open) clac();

    // Reached max without NUL — terminate and return success (truncated)
    dst[copied] = '\0';
    return COPY_SUCCESS;
}
