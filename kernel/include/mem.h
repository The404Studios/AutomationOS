#ifndef MEM_H
#define MEM_H

#include "types.h"

// Boot memory map entry (must match boot_enhanced.h layout)
typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;      // Memory type (7 = available for boot_enhanced.h, 1 = usable for legacy)
    uint32_t reserved;  // Padding for alignment, matches bootloader struct
} memory_map_entry_t;

/*
 * Physical Memory Manager (PMM) - Buddy Allocator
 * ===============================================
 */

/* Initialize physical memory manager with boot memory map (processes pages within initial identity-mapped range) */
void pmm_init(memory_map_entry_t* mmap, uint32_t mmap_count);

/* Add remaining pages above the initial identity map limit. Call AFTER paging_init extends mappings. */
void pmm_add_remaining_pages(memory_map_entry_t* mmap, uint32_t mmap_count);

/* Allocate a single 4KB physical page, returns NULL on failure */
void* pmm_alloc_page(void);

/* Free a previously allocated physical page */
void pmm_free_page(void* page);

/*
 * Batch / contiguous physical page allocator
 * ------------------------------------------
 * pmm_alloc_pages: allocate `count` CONTIGUOUS 4KB physical pages.
 *   Uses a bitmap + "next free" hint cursor scanned 64-bits at a time
 *   (word scan with __builtin_ctzll).  Returns physical base address of
 *   the run, or NULL if no contiguous run of that length is available.
 *   O(total_pages/64) worst case; typically O(count/64) with hint.
 *
 * pmm_free_pages: free `count` contiguous pages starting at `base`.
 *   Marks pages free in the bitmap and returns them to the global
 *   free-list for reuse by single-page allocators.  O(count).
 *
 * NOTE: The kernel identity-maps physical == virtual, so callers may
 *       treat the returned pointer as both the physical and virtual address.
 */
void* pmm_alloc_pages(size_t count);
void  pmm_free_pages(void* base, size_t count);

/*
 * Huge Page Allocator (2MB pages)
 * --------------------------------
 * pmm_alloc_huge_page: allocate a 2MB physically contiguous, 2MB-aligned page.
 *   Returns NULL on failure. Reduces TLB pressure by 512x (one TLB entry
 *   covers 2MB instead of 512 separate 4KB entries).
 *
 * pmm_free_huge_page: free a 2MB huge page allocated via pmm_alloc_huge_page.
 *   Base address must be 2MB-aligned.
 *
 * pmm_get_huge_page_stats: retrieve allocation statistics (allocated, freed, failures).
 */
void* pmm_alloc_huge_page(void);
void  pmm_free_huge_page(void* base);
void  pmm_get_huge_page_stats(uint64_t* allocated, uint64_t* freed, uint64_t* failures);

/* Get total physical memory in bytes */
uint64_t pmm_get_total_memory(void);

/* Get used physical memory in bytes */
uint64_t pmm_get_used_memory(void);

/* Get available physical memory in bytes */
uint64_t pmm_get_free_memory(void);

/*
 * Virtual Memory Manager (VMM)
 * ============================
 */

/* Initialize virtual memory manager and paging */
void vmm_init(void);

/* Map virtual address to physical page with flags (PAGE_PRESENT | PAGE_WRITE | PAGE_USER) */
void* vmm_map_page(void* virt, void* phys, uint64_t flags);

/* Unmap virtual address and free page table entry */
void vmm_unmap_page(void* virt);

/* Map a 2MB huge page (reduces TLB pressure by 512x). virt and phys must be 2MB-aligned. */
int paging_map_huge_page(void* virt, void* phys, uint64_t flags);

/* Unmap a 2MB huge page. virt must be 2MB-aligned. */
int paging_unmap_huge_page(void* virt);

/* Get physical address for given virtual address, returns NULL if unmapped */
void* vmm_get_physical(void* virt);

/* Create a new address space (PML4) with kernel mappings, returns CR3 value */
uint64_t paging_create_address_space(void);

/* Destroy an address space and free all user-space page tables */
void paging_destroy_address_space(uint64_t cr3);

/* Target a specific PML4 for subsequent paging_map_page calls */
void paging_set_target(uint64_t cr3);

/* Reset mapping target to kernel PML4 */
void paging_reset_target(void);

/*
 * Map a contiguous SHARED physical range [paddr, paddr+size) at vaddr into the
 * address space `cr3` (e.g. framebuffer MMIO, shm segments). Saves/restores the
 * active mapping target so it is safe to call from the syscall path. Pages are
 * NOT marked PTE_OWNED (caller must use a SHARED_* VA window so teardown skips
 * them). Returns 0 on success, negative on error. `flags` are PAGE_* bits.
 */
int vmm_map_phys_into(uint64_t cr3, uint64_t vaddr, uint64_t paddr,
                      uint64_t size, uint64_t flags);

/*
 * Batch-map `count` consecutive 4KB pages starting at `vaddr` → `paddr` in
 * the CURRENTLY ACTIVE address space (active_pml4 — caller must set target
 * first via paging_set_target() if mapping into a process AS).
 *
 * Upper page-table levels (PML4/PDPT/PD) are walked at most once per
 * 512-GB/1-GB/2-MB boundary, so consecutive-page mapping is O(N + N/512)
 * rather than the O(4N) of N individual paging_map_page() calls.
 *
 * TLB: invlpg is issued per page only when the target address space is the
 * one currently loaded in CR3.  For address spaces being constructed offline
 * (new process, ELF load before first context switch), all TLB invalidations
 * are skipped — they are unnecessary and would flush unrelated entries from
 * the running kernel AS.
 *
 * Ownership: mirrors paging_map_page() — PTE_OWNED is set for user pages
 * outside the shared VA windows so teardown can reclaim them.
 *
 * Returns 0 on success, -1 on PMM allocation failure (partial mapping may
 * have occurred).
 */
int vmm_map_range(uint64_t vaddr, uint64_t paddr, uint64_t count,
                  uint64_t flags);

/*
 * Anonymous mmap: allocate `len` bytes of fresh private physical pages and map
 * them into address space `cr3` (PAGE_USER|PAGE_PRESENT, +PAGE_WRITE if prot
 * has VMM_PROT_WRITE), eagerly. Pages are marked PTE_OWNED so they are freed on
 * address-space teardown. Returns the user virtual address, or NULL on failure.
 * `prot` is a bitmask of VMM_PROT_* (defined below).
 */
void* vmm_mmap_anon(uint64_t cr3, uint64_t len, uint32_t prot);

/* Release an address space's mmap bump-cursor slot at teardown (call from
 * process_unref before paging_destroy_address_space) so a recycled CR3 starts
 * fresh. */
void vmm_as_release(uint64_t cr3);

/* Unmap and free a region previously returned by vmm_mmap_anon. 0 on success. */
int vmm_munmap(uint64_t cr3, uint64_t addr, uint64_t len);

/*
 * Unmap [vaddr, vaddr+size) from address space `cr3`. If free_owned is true,
 * leaf pages carrying PTE_OWNED are returned to the PMM. Internal helper used
 * by vmm_munmap; safe to call from the syscall path. 0 on success.
 */
int vmm_unmap_range_into(uint64_t cr3, uint64_t vaddr, uint64_t size,
                         bool free_owned);

/*
 * Kernel Heap
 * ===========
 */

/* Initialize kernel heap allocator */
void heap_init(void);

/* Allocate kernel memory, returns NULL on failure */
void* kmalloc(size_t size);

/* Free previously allocated kernel memory */
void kfree(void* ptr);

/* Benchmark slab allocator efficiency vs. traditional heap */
void heap_slab_benchmark(void);

/* Memory leak tracking (enabled with MEM_DEBUG) */
#ifdef MEM_DEBUG
void kmalloc_stats_print(void);
void kmalloc_stats_reset(void);
#endif

// Page flags
#define PAGE_PRESENT  0x01
#define PAGE_WRITE    0x02
#define PAGE_USER     0x04
#define PAGE_NX       (1ULL << 63)  // No-Execute (bit 63); inert until EFER.NXE is enabled

// User/kernel address space boundary
#define USER_SPACE_END      0x0000800000000000ULL
#define KERNEL_SPACE_START  0xFFFF800000000000ULL

/*
 * Per-PTE ownership flag (address-space teardown)
 * ===============================================
 * Bit 9 of a leaf PTE is available for OS use (ignored by hardware). We use it
 * to mark physical pages that are PRIVATE to a single process address space and
 * were allocated from the PMM specifically for that process (ELF PT_LOAD
 * segments and the user stack, plus fork's copy-on-write duplicates).
 *
 * paging_destroy_address_space() frees ONLY leaf pages carrying PTE_OWNED. This
 * prevents the destroy walk from returning globally shared physical memory to
 * the PMM free list — identity-mapped RAM (2MB huge pages), the framebuffer,
 * initrd-backed pages, ELF source data, shared-memory (shm) segments, and the
 * read-only shared data window are all left untouched.
 */
#define PTE_OWNED  (1ULL << 9)

/*
 * Copy-on-write flag (fork #20). Bit 10 of a leaf PTE is also OS-available. It
 * marks a page that is SHARED read-only between a parent and child after fork:
 * a write traps and cow_handle_write() gives the writer a private copy. Set
 * only on writable, process-private (PTE_OWNED) pages; read-only code/rodata is
 * shared without it (a write there is a genuine W^X violation).
 */
#define PTE_COW    (1ULL << 10)

/* ---- Copy-on-write page accounting (kernel/core/mem/cow.c) ---- */
void cow_init(void);                 /* allocate the refcount table (after PMM) */
int  cow_enabled(void);              /* 1 if CoW is available */
int  cow_incref(uint64_t phys);      /* +1 owner; 0 if untrackable -> eager copy */
int  cow_unref(uint64_t phys);       /* -1 owner; 1 if caller must free the frame */
int  cow_handle_write(uint64_t fault_addr); /* resolve a CoW write fault; 1=handled */

/*
 * Shared user VA windows that must NOT be marked PTE_OWNED.
 * Mappings placed here point at memory owned by another subsystem (which frees
 * it independently), so paging must never claim ownership of them:
 *   - Framebuffer MMIO mapped for userspace.
 *   - Read-only shared data exported to processes.
 *   - System V shared-memory (shm) attach window (freed by shmctl).
 */
#define SHARED_FB_VA_START    0x40000000ULL   // framebuffer window base
#define SHARED_FB_VA_END      0x50000000ULL
#define SHARED_DATA_VA_START  0x50000000ULL   // read-only shared data window base
#define SHARED_DATA_VA_END    0x60000000ULL
#define SHARED_SHM_VA_START   0x60000000ULL   // shm attach window base
#define SHARED_SHM_VA_END     0x100000000ULL  // shm window end (== anon base below)

/*
 * Anonymous-mmap user VA window (vmm_mmap_anon).
 * Lives ABOVE all shared windows so that anon pages — which ARE process-private
 * and must be reclaimed on teardown — are marked PTE_OWNED by paging_map_page().
 * Conversely the shared windows [SHARED_FB_VA_START, SHARED_SHM_VA_END) are
 * never owned. Base 4GiB; user space extends to USER_SPACE_END.
 */
#define VMM_ANON_VA_BASE      0x100000000ULL

/*
 * User Memory Validation
 * ======================
 */

/* Validate user buffer is within user space and accessible */
bool validate_user_buffer(const void* ptr, size_t size);

/* Validate user string is null-terminated within max_len bytes */
bool validate_user_string(const char* str, size_t max_len);

/*
 * Safe User-Kernel Memory Copy
 * =============================
 */

/* Safely copy from user space to kernel space, returns COPY_SUCCESS or COPY_EFAULT */
int copy_from_user(void* kernel_dst, const void* user_src, size_t n);

/* Safely copy from kernel space to user space, returns COPY_SUCCESS or COPY_EFAULT */
int copy_to_user(void* user_dst, const void* kernel_src, size_t n);

/*
 * copy_user_string - copy a NUL-terminated string from user space to kernel.
 *
 * Copies byte-by-byte from user_src into kernel_dst, stopping at the first
 * NUL terminator or after max-1 non-NUL bytes (always writes a NUL at
 * kernel_dst[copied]).  Before reading the first byte of each new 4KB page
 * it checks via a page-table walk (paging_get_pte) that the page is mapped,
 * stopping with COPY_EFAULT instead of generating a fault if it is not.
 *
 * Path handlers SHOULD use this instead of copy_from_user(buf, ptr, MAX_PATH)
 * to avoid reading past the end of a short path that sits near a page boundary.
 *
 * Parameters:
 *   kernel_dst - kernel buffer (caller guarantees >= max bytes)
 *   user_src   - user-space NUL-terminated string pointer
 *   max        - buffer size including the NUL terminator (strlcpy semantics)
 *
 * Returns COPY_SUCCESS (0) on success (string fits and NUL found, or
 * truncated to max-1 chars); COPY_EFAULT (-1) on bad pointer or
 * unmapped page encountered before NUL.
 */
int copy_user_string(void* kernel_dst, const void* user_src, size_t max);

// Copy error codes
#define COPY_SUCCESS 0
#define COPY_EFAULT -1

/*
 * Extended VMM operations for PE loader / Win32 subsystem
 * =======================================================
 * These functions operate on a per-process virtual memory context.
 * The 'vmm' argument is the process vmm context pointer (may be NULL for kernel context).
 */

/* Allocate virtual memory in a process address space */
void* vmm_alloc(void* vmm_ctx, size_t size, int prot);

/* Allocate virtual memory at a specific address */
void* vmm_alloc_at(void* vmm_ctx, void* addr, size_t size, int prot);

/* Free virtual memory */
void vmm_free(void* vmm_ctx, void* addr, size_t size);

/* Change protection flags on a virtual memory region */
int vmm_protect(void* vmm_ctx, void* addr, size_t size, int prot);

/* Protection flags for vmm_alloc/vmm_protect */
#define VMM_PROT_READ    0x01
#define VMM_PROT_WRITE   0x02
#define VMM_PROT_EXEC    0x04

#endif
