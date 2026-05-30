#include "../../include/mem.h"
#include "../../include/kernel.h"

/*
 * Anonymous memory mapping (vmm_mmap_anon / vmm_munmap)
 * =====================================================
 * Gives userspace fresh, zero-cost-to-the-caller private memory — the back
 * buffers, scratch heaps, and staging surfaces a compositor needs.
 *
 * Allocation strategy (M1, eager):
 *   - Each call allocates `len` rounded up to whole 4KiB pages from the PMM
 *     (pmm_alloc_page), one page at a time, and maps each into the target
 *     address space `cr3` with PAGE_USER|PAGE_PRESENT (+PAGE_WRITE if the prot
 *     mask requests write). No demand paging: every page is backed immediately.
 *   - Pages land in the anonymous VA window starting at VMM_ANON_VA_BASE
 *     (4GiB), which sits ABOVE every shared VA window. Because paging_map_page()
 *     marks any user page outside [SHARED_FB_VA_START, SHARED_SHM_VA_END) with
 *     PTE_OWNED, anon pages are automatically owned and therefore reclaimed by
 *     paging_destroy_address_space() on process exit (and eagerly by munmap).
 *
 * VA selection (per address space):
 *   - We keep a small fixed table keyed by CR3 (PCID/flag bits masked off) that
 *     records the next free VA for each address space. The first mmap in an
 *     address space starts at VMM_ANON_VA_BASE; subsequent mmaps bump upward by
 *     the rounded length. This is a monotonically increasing bump allocator.
 *
 *   LIMITATION (M1): the bump never reclaims VA space — vmm_munmap frees the
 *   physical pages and clears the PTEs, but the virtual range is not returned to
 *   the per-AS cursor, so long-lived churn slowly walks the cursor up toward
 *   USER_SPACE_END. For M1 / a single compositor allocating a handful of large
 *   buffers this is fine (the window is ~127 TiB wide). A real implementation
 *   would track free VA ranges (a VMA list/tree) per address space. The table
 *   is also fixed-size; if it fills, we fall back to a shared global cursor
 *   (still correct, just no per-AS isolation of the cursor for extra AS's).
 */

#define VMA_MAX_AS  16  // number of address spaces we track cursors for

typedef struct {
    uint64_t cr3;   // address-space key (PCID/flag bits masked), 0 == free slot
    uint64_t next;  // next free user VA in the anon window
} vma_as_t;

static vma_as_t vma_table[VMA_MAX_AS];
static uint64_t vma_global_next = VMM_ANON_VA_BASE;  // fallback cursor

// Mask CR3 down to the PML4 physical base (drop PCID bits 0-11 and bit 63).
static inline uint64_t vma_key(uint64_t cr3) {
    return cr3 & ~0xFFFULL;
}

// Get (or allocate) the bump cursor slot for an address space.
// Returns a pointer to the cursor to advance, or NULL to use the global cursor.
static uint64_t* vma_cursor_for(uint64_t cr3) {
    uint64_t key = vma_key(cr3);

    // Existing slot?
    for (int i = 0; i < VMA_MAX_AS; i++) {
        if (vma_table[i].cr3 == key) {
            return &vma_table[i].next;
        }
    }
    // First free slot?
    for (int i = 0; i < VMA_MAX_AS; i++) {
        if (vma_table[i].cr3 == 0) {
            vma_table[i].cr3 = key;
            vma_table[i].next = VMM_ANON_VA_BASE;
            return &vma_table[i].next;
        }
    }
    // Table full: fall back to the shared global cursor.
    return &vma_global_next;
}

// Release the bump-cursor slot for an address space at teardown. Without this,
// the slot leaks and — once the freed PML4 physical page is recycled for a new
// process (same CR3 value) — the new process inherits the dead one's stale mmap
// cursor, returning unexpectedly-high VAs or prematurely "exhausting" the anon
// window. Called from process_unref() during address-space destruction.
void vmm_as_release(uint64_t cr3) {
    uint64_t key = vma_key(cr3);
    if (!key) return;
    for (int i = 0; i < VMA_MAX_AS; i++) {
        if (vma_table[i].cr3 == key) {
            vma_table[i].cr3 = 0;
            vma_table[i].next = 0;
            return;
        }
    }
}

void* vmm_mmap_anon(uint64_t cr3, uint64_t len, uint32_t prot) {
    if (!cr3 || len == 0) {
        return NULL;
    }

    uint64_t size = ALIGN_UP(len, PAGE_SIZE);
    uint64_t npages = size / PAGE_SIZE;

    // Reserve the VA range from this address space's cursor.
    uint64_t* cursor = vma_cursor_for(cr3);
    uint64_t base = *cursor;

    // Keep the entire region within user space.
    if (base < VMM_ANON_VA_BASE ||
        base >= USER_SPACE_END ||
        size > USER_SPACE_END - base) {
        kprintf("[VMA] mmap_anon: VA window exhausted (base=%p len=%lu)\n",
                (void*)base, (unsigned long)size);
        return NULL;
    }

    // Page flags (uint64_t to accommodate PAGE_NX in bit 63 for future NX enforcement).
    // EXEC has no separate hardware bit today; NX is not set here.
    uint64_t flags = PAGE_PRESENT | PAGE_USER;
    if (prot & VMM_PROT_WRITE) {
        flags |= PAGE_WRITE;
    }

    // Eagerly allocate and map each page. We reuse vmm_map_phys_into for the
    // mapping mechanics: it retargets cr3, maps the page, and restores the
    // active target (syscall-safe). The phys here is fresh & private, and the
    // anon VA sits above SHARED_SHM_VA_END, so paging_map_page() automatically
    // stamps PTE_OWNED, letting teardown/munmap reclaim it.
    for (uint64_t i = 0; i < npages; i++) {
        void* phys = pmm_alloc_page();
        if (!phys) {
            // Roll back: unmap+free everything mapped so far for this call.
            kprintf("[VMA] mmap_anon: OOM after %lu/%lu pages, rolling back\n",
                    (unsigned long)i, (unsigned long)npages);
            if (i > 0) {
                vmm_unmap_range_into(cr3, base, i * PAGE_SIZE, true);
            }
            return NULL;
        }
        // Map this single private page into cr3. vmm_map_phys_into saves/restores
        // the active target, so each call is self-contained and syscall-safe.
        vmm_map_phys_into(cr3, base + i * PAGE_SIZE, (uint64_t)phys,
                          PAGE_SIZE, flags);
    }

    // Commit the cursor.
    *cursor = base + size;

    return (void*)base;
}

int vmm_munmap(uint64_t cr3, uint64_t addr, uint64_t len) {
    if (!cr3 || addr == 0 || len == 0) {
        return -1;
    }
    // Only the anon window is owned by this allocator. Refuse to free pages
    // outside it (those belong to shm/fb/ELF and must not be touched here).
    if (addr < VMM_ANON_VA_BASE || addr >= USER_SPACE_END) {
        return -1;
    }
    // Unmap and free the private (PTE_OWNED) pages immediately.
    return vmm_unmap_range_into(cr3, addr, len, true);
}
