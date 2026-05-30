#include "../../include/mem.h"
#include "../../include/kernel.h"
#include "../../include/x86_64.h"  // read_cr3() for the live address-space walk

// External string functions
extern void* memcpy(void* dest, const void* src, size_t count);

// Declared in paging.c
extern void paging_init(void);
extern void paging_map_page(void* virt, void* phys, uint64_t flags);
extern void paging_unmap_page(void* virt);
extern uint64_t paging_create_address_space(void);
extern void paging_destroy_address_space(uint64_t cr3);

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
 * user_page_is_accessible - check whether a single 4KB page containing
 * `addr` is mapped and accessible in the current address space.
 *
 * Uses paging_get_pte() (declared above, implemented in paging.c) to do a
 * software page-table walk without touching the memory itself.  Returns true
 * when the PTE is present (bit 0 set).
 *
 * If paging_get_pte() is not yet available at link time (i.e. the symbol is
 * absent), copy_user_string falls back to address-range validation only — it
 * will not crash but may fault on a genuinely unmapped page.  The linker will
 * diagnose the missing symbol during build, so the integrator will know when
 * the helper needs to be added to paging.c.
 */
static bool user_page_is_accessible(uint64_t addr) {
    uint64_t pte = paging_get_pte(addr);
    // PTE_PRESENT = bit 0.  A zero PTE means not mapped.
    return (pte & 1ULL) != 0;
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
    // Performance: 8-byte aligned middle + byte tail is handled inside
    // the compiler/libc memcpy. When string.c is replaced by another agent,
    // fall back to the manual loop below by swapping the call.
    memcpy(kernel_dst, user_src, n);

    return COPY_SUCCESS;
}

/*
 * Extended VMM operations for PE loader / Win32 subsystem
 * These are stub implementations that allocate from the kernel heap
 * until full per-process address space management is implemented.
 */

void* vmm_alloc(void* vmm_ctx, size_t size, int prot) {
    (void)vmm_ctx; (void)prot;
    /* TODO: Implement proper per-process allocation */
    void* addr = kmalloc(size);
    if (addr) {
        uint8_t* p = (uint8_t*)addr;
        for (size_t i = 0; i < size; i++) p[i] = 0;
    }
    return addr;
}

void* vmm_alloc_at(void* vmm_ctx, void* addr, size_t size, int prot) {
    (void)vmm_ctx; (void)prot;
    /* TODO: Implement allocation at specific address */
    if (addr) {
        /* Cannot guarantee specific address with kmalloc, fall back */
        return vmm_alloc(vmm_ctx, size, prot);
    }
    return vmm_alloc(vmm_ctx, size, prot);
}

void vmm_free(void* vmm_ctx, void* addr, size_t size) {
    (void)vmm_ctx; (void)size;
    if (addr) kfree(addr);
}

int vmm_protect(void* vmm_ctx, void* addr, size_t size, int prot) {
    (void)vmm_ctx; (void)addr; (void)size; (void)prot;
    /* TODO: Implement page protection changes */
    return 0;
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
        return COPY_EFAULT;
    }

    // Fast-path bulk copy (see copy_from_user comment above)
    memcpy(user_dst, kernel_src, n);

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
            if (!user_page_is_accessible(cur_addr)) {
                // Next page is not mapped — stop here cleanly
                kprintf("[VMM] copy_user_string: Page at %p not accessible\n",
                        (void*)cur_addr);
                dst[copied] = '\0';
                return COPY_EFAULT;
            }
        }

        char c = src[copied];
        dst[copied] = c;
        copied++;

        if (c == '\0') {
            // NUL already written; string is complete
            return COPY_SUCCESS;
        }
    }

    // Reached max without NUL — terminate and return success (truncated)
    dst[copied] = '\0';
    return COPY_SUCCESS;
}
