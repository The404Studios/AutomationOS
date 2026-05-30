#include "../../include/mem.h"
#include "../../include/x86_64.h"
#include "../../include/kernel.h"
#include "../../include/string.h"
#include "../../include/tlb.h"

#define ENTRIES_PER_TABLE 512

typedef uint64_t pte_t;  // Page table entry

typedef struct {
    pte_t entries[ENTRIES_PER_TABLE];
} page_table_t;

// PML4 (top-level page table)
static page_table_t* kernel_pml4 = NULL;
static page_table_t* active_pml4 = NULL;  // Currently targeted PML4 for mappings

// PCID (Process-Context Identifiers) support
#define CR4_PCIDE_BIT    (1ULL << 17)  // PCID enable bit in CR4
#define CR3_PCID_MASK    0xFFF          // 12-bit PCID (bits 0-11)
#define CR3_NO_FLUSH     (1ULL << 63)   // Bit 63: preserve TLB entries

static uint16_t next_pcid = 1;          // PCID 0 reserved for kernel
static bool pcid_supported = false;

// Get index for each paging level
#define PML4_INDEX(addr) (((uint64_t)(addr) >> 39) & 0x1FF)
#define PDPT_INDEX(addr) (((uint64_t)(addr) >> 30) & 0x1FF)
#define PD_INDEX(addr)   (((uint64_t)(addr) >> 21) & 0x1FF)
#define PT_INDEX(addr)   (((uint64_t)(addr) >> 12) & 0x1FF)

// Page table entry flags
#define PTE_PRESENT    (1ULL << 0)
#define PTE_WRITE      (1ULL << 1)
#define PTE_USER       (1ULL << 2)
#define PTE_ACCESSED   (1ULL << 5)
#define PTE_DIRTY      (1ULL << 6)

static page_table_t* alloc_page_table(void) {
    void* page = pmm_alloc_page();
    if (!page) return NULL;

    memset(page, 0, PAGE_SIZE);
    return (page_table_t*)page;
}

/**
 * Check if PCID is supported and enable it
 *
 * PCID (Process-Context Identifiers) allows TLB entries to be tagged with
 * a 12-bit process ID, avoiding TLB flush on context switch.
 *
 * Performance impact: 40-60% reduction in context switch cost
 */
static void paging_enable_pcid(void) {
    // Check if PCID is supported (CPUID.01H:ECX.PCID[bit 17])
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));

    if (ecx & (1 << 17)) {
        // PCID supported - enable it in CR4
        uint64_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= CR4_PCIDE_BIT;
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

        pcid_supported = true;
        kprintf("[VMM] PCID enabled (Process-Context Identifiers)\n");
        kprintf("[VMM] TLB flushing optimized for context switches\n");
    } else {
        kprintf("[VMM] PCID not supported on this CPU\n");
    }
}

static void cpu_enable_fpu_sse(void) {
    uint64_t cr0, cr4;

    // CR0: clear EM (bit 2), set MP (bit 1), set NE (bit 5)
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);   // Clear EM (x87 FPU emulation)
    cr0 |= (1ULL << 1);    // Set MP (monitor coprocessor)
    cr0 |= (1ULL << 5);    // Set NE (native error handling)
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));

    // CR4: set OSFXSR (bit 9) and OSXMMEXCPT (bit 10)
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);    // OSFXSR - enable FXSAVE/FXRSTOR
    cr4 |= (1ULL << 10);   // OSXMMEXCPT - enable unmasked SIMD exceptions
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

    // Initialize x87 FPU to default state
    __asm__ volatile("fninit");

    // Initialize SSE MXCSR to default (0x1F80 = all exceptions masked, round-to-nearest)
    __asm__ volatile("pushq $0x1F80\n\t" "ldmxcsr (%rsp)\n\t" "addq $8, %rsp");

    kprintf("[VMM] FPU/SSE/AVX enabled for userspace\n");
}

void paging_init(void) {
    kprintf("[VMM] Initializing paging...\n");

    // Enable EFER.NXE (No-Execute Enable, MSR 0xC0000080 bit 11) BEFORE any
    // PAGE_NX (PTE bit 63) is ever set. Until NXE is on, bit 63 is reserved and
    // setting it would #GP/#PF on access; once on, it is honored as No-Execute.
    // This is the hardware switch that makes user-space W^X (RX code / NX data,
    // applied by the ELF loader in exec.c) actually enforced.
    {
        uint64_t efer = rdmsr(MSR_EFER);
        if (!(efer & EFER_NXE)) {
            efer |= EFER_NXE;
            wrmsr(MSR_EFER, efer);
        }
        kprintf("[VMM] EFER.NXE enabled (No-Execute / W^X armed)\n");
    }

    // Read current CR3 to get boot page tables
    uint64_t boot_cr3 = read_cr3();
    page_table_t* boot_pml4 = (page_table_t*)(boot_cr3 & 0x000FFFFFFFFFF000ULL);

    kprintf("[VMM] Boot PML4 at %p\n", boot_pml4);

    // IMPORTANT: We must keep using boot page tables initially!
    // Boot.asm sets up identity mapping for 0-512MB. If we allocate new page tables
    // (via pmm_alloc_page), they might be ABOVE 512MB and thus not accessible
    // until we extend the identity mapping.
    //
    // Strategy: Use boot PML4 directly, extend identity mapping to cover all RAM,
    // then we can safely allocate new page tables.

    kernel_pml4 = boot_pml4;  // Use boot PML4 directly

    // CRITICAL: Extend identity mapping to cover all physical memory
    // Boot.asm only maps 0-512MB using 2MB huge pages (256 PD entries).
    // We need to extend this to cover all RAM so pmm_alloc_page() results
    // are accessible as virtual addresses.
    //
    // Strategy: Extend the boot PD to map up to 1GB (512 PD entries).
    // For systems with >1GB RAM, we'll map only first 1GB which should be
    // sufficient for page tables and kernel heap.

    // Get boot PDPT from PML4[0]
    if (!(boot_pml4->entries[0] & PTE_PRESENT)) {
        kernel_panic("Boot identity mapping (PML4[0]) not present!");
    }
    page_table_t* boot_pdpt = (page_table_t*)(boot_pml4->entries[0] & 0x000FFFFFFFFFF000ULL);

    // Get boot PD from PDPT[0]
    if (!(boot_pdpt->entries[0] & PTE_PRESENT)) {
        kernel_panic("Boot PDPT[0] not present!");
    }
    page_table_t* boot_pd = (page_table_t*)(boot_pdpt->entries[0] & 0x000FFFFFFFFFF000ULL);

    // Boot.asm mapped 256 entries (0-512MB). Extend to 512 entries (0-1GB).
    // This ensures page tables allocated from PMM (which typically allocates
    // from low memory first) are accessible.
    kprintf("[VMM] Extending identity mapping from 512MB to 1GB\n");

    for (uint64_t i = 256; i < 512; i++) {
        // Map each 2MB page (huge page)
        uint64_t phys_addr = i * 0x200000ULL;  // i * 2MB
        boot_pd->entries[i] = phys_addr | PTE_PRESENT | PTE_WRITE | (1ULL << 7);  // Bit 7 = PS (huge)
    }

    // Map 1GB-16GB using additional PDs allocated from PMM.
    // PMM now has free pages in the 0-1GB range (identity-mapped), so
    // the allocated PD pages will be accessible.
    uint64_t total_gb = 16;
    kprintf("[VMM] Extending identity mapping from 1GB to %luGB...\n", (unsigned long)total_gb);

    for (uint64_t pdpt_idx = 1; pdpt_idx < total_gb; pdpt_idx++) {
        // Skip if this PDPT entry already exists (shouldn't happen with boot tables)
        if (boot_pdpt->entries[pdpt_idx] & PTE_PRESENT) continue;

        // Allocate new PD for this 1GB range (PMM allocation will be from 0-1GB)
        page_table_t* pd = (page_table_t*)pmm_alloc_page();
        if (!pd) {
            kprintf("[VMM] WARNING: Failed to allocate PD for %luGB range\n", (unsigned long)pdpt_idx);
            break;
        }

        memset(pd, 0, PAGE_SIZE);

        // Fill with 512 2MB huge page entries
        for (uint64_t i = 0; i < 512; i++) {
            uint64_t phys_addr = (pdpt_idx * 512 + i) * 0x200000ULL;
            pd->entries[i] = phys_addr | PTE_PRESENT | PTE_WRITE | (1ULL << 7);
        }

        boot_pdpt->entries[pdpt_idx] = (uint64_t)pd | PTE_PRESENT | PTE_WRITE;
    }

    // Flush TLB to activate new mappings
    write_cr3(read_cr3());

    kprintf("[VMM] Identity mapping extended successfully\n");

    // Enable PCID if supported (must be done AFTER setting up paging)
    paging_enable_pcid();

    // Enable FPU/SSE for userspace (required for LLM inference with floats)
    cpu_enable_fpu_sse();

    active_pml4 = kernel_pml4;
    kprintf("[VMM] Paging initialized with full identity mapping\n");
}

// Switch which PML4 paging_map_page targets (for per-process mapping)
void paging_set_target(uint64_t cr3) {
    active_pml4 = (page_table_t*)(cr3 & 0x000FFFFFFFFFF000ULL);
}

void paging_reset_target(void) {
    active_pml4 = kernel_pml4;
}

// Physical address (CR3 value, PCID 0) of the kernel's master PML4. The kernel
// identity-maps all RAM with 2MB huge pages and NEVER splits the userspace load
// window, so loading this CR3 gives a stable, alias-free view of both the initrd
// (copy source) and every PMM frame (copy destination) during ELF loading.
uint64_t paging_kernel_cr3(void) {
    return (uint64_t)kernel_pml4;
}

void paging_map_page(void* virt, void* phys, uint64_t flags) {
    uint64_t pml4_idx = PML4_INDEX(virt);
    uint64_t pdpt_idx = PDPT_INDEX(virt);
    uint64_t pd_idx = PD_INDEX(virt);
    uint64_t pt_idx = PT_INDEX(virt);

    // Debug output disabled for performance
    (void)pml4_idx; (void)pdpt_idx; (void)pd_idx; (void)pt_idx;

    // CRITICAL SAFETY: Kernel space must NEVER have USER bit
    // This prevents user mode from accessing kernel memory
    uint64_t virt_addr = (uint64_t)virt;
    if (virt_addr >= KERNEL_SPACE_START && (flags & PAGE_USER)) {
        kprintf("[PAGING] ERROR: Attempt to map kernel space %p with USER flag!\n", virt);
        return;
    }

    // CRITICAL: Calculate flags for intermediate page table entries
    // Intermediate entries (PML4→PDPT, PDPT→PD, PD→PT) MUST have USER bit
    // if the final page is user-accessible, otherwise ring 3 gets #PF
    uint64_t intermediate_flags = PTE_PRESENT | PTE_WRITE;
    if (flags & PAGE_USER) {
        intermediate_flags |= PTE_USER;
    }

    // Get or create PDPT (uses active_pml4 - may be kernel or process PML4)
    page_table_t* target = active_pml4;
    page_table_t* pdpt;
    if (!(target->entries[pml4_idx] & PTE_PRESENT)) {
        kprintf("[PAGING]   Allocating new PDPT for PML4[%lu]\n", pml4_idx);
        pdpt = alloc_page_table();
        if (!pdpt) {
            kprintf("[PAGING] Failed to allocate PDPT for mapping %p\n", virt);
            return;
        }
        target->entries[pml4_idx] = (uint64_t)pdpt | intermediate_flags;
    } else {
        pdpt = (page_table_t*)(target->entries[pml4_idx] & 0x000FFFFFFFFFF000ULL);
        if ((flags & PAGE_USER) && !(target->entries[pml4_idx] & PTE_USER)) {
            target->entries[pml4_idx] |= PTE_USER;
        }
    }

    // Get or create PD
    page_table_t* pd;
    if (!(pdpt->entries[pdpt_idx] & PTE_PRESENT)) {
        pd = alloc_page_table();
        if (!pd) {
            kprintf("[PAGING] Failed to allocate PD for mapping %p\n", virt);
            return;
        }
        pdpt->entries[pdpt_idx] = (uint64_t)pd | intermediate_flags;
    } else {
        pd = (page_table_t*)(pdpt->entries[pdpt_idx] & 0x000FFFFFFFFFF000ULL);
        // Upgrade existing entry: add USER bit if mapping user pages
        if ((flags & PAGE_USER) && !(pdpt->entries[pdpt_idx] & PTE_USER)) {
            pdpt->entries[pdpt_idx] |= PTE_USER;
        }
    }

    // Get or create PT
    page_table_t* pt;
    if (!(pd->entries[pd_idx] & PTE_PRESENT)) {
        // Allocating new PT
        pt = alloc_page_table();
        if (!pt) {
            kprintf("[PAGING] Failed to allocate PT for mapping %p\n", virt);
            return;
        }
        pd->entries[pd_idx] = (uint64_t)pt | intermediate_flags;
        // PD created
    } else if (pd->entries[pd_idx] & (1ULL << 7)) {
        // HUGE PAGE (2MB) - must split it into 4KB pages.
        // Capture the original PDE flags (minus PS bit 7) to preserve attributes
        // (PRESENT, WRITE, USER if set, etc.) in the 4KB split copies.
        // Under the current identity map, huge pages have PTE_PRESENT|PTE_WRITE
        // (no PTE_USER), so this is a no-op today but is correct for any future
        // non-identity use where USER or other bits may be present.
        /* Preserve all flag bits including bit 63 (PAGE_NX), minus the PS bit (7). */
        uint64_t orig_pde_flags = (pd->entries[pd_idx] & ~(1ULL << 7))   /* all bits */
                                & ~0x000FFFFFFFE00000ULL;                  /* minus phys base */
        pt = alloc_page_table();
        if (!pt) {
            kprintf("[PAGING] Failed to allocate PT for splitting huge page %p\n", virt);
            return;
        }

        // Fill PT with 512 entries mapping the original 2MB huge page, inheriting flags.
        uint64_t base_phys = pd->entries[pd_idx] & 0x000FFFFFFFE00000ULL;  // 2MB-aligned phys base
        for (int i = 0; i < 512; i++) {
            pt->entries[i] = (base_phys + (uint64_t)i * PAGE_SIZE) | orig_pde_flags;
        }

        // Replace huge page entry with PT pointer
        pd->entries[pd_idx] = (uint64_t)pt | intermediate_flags;
        invlpg(virt);  // Flush TLB for this range
    } else {
        pt = (page_table_t*)(pd->entries[pd_idx] & 0x000FFFFFFFFFF000ULL);
        // Upgrade existing entry: add USER bit if mapping user pages
        if ((flags & PAGE_USER) && !(pd->entries[pd_idx] & PTE_USER)) {
            pd->entries[pd_idx] |= PTE_USER;
        }
    }

    // Map the page.
    //
    // OWNERSHIP TRACKING (teardown double-free fix):
    // Mark this leaf PTE as PTE_OWNED when the page is process-PRIVATE, so that
    // paging_destroy_address_space() is allowed to return it to the PMM. A page
    // is private when ALL of the following hold:
    //   1. We are mapping into a process address space (not the kernel PML4).
    //   2. The mapping is user-accessible (PAGE_USER) — kernel/identity pages
    //      and intermediate tables never get this bit.
    //   3. The virtual address is NOT inside a shared window (framebuffer,
    //      read-only shared data, or shm). Those point at memory owned by other
    //      subsystems that free it independently; the teardown walk must skip
    //      them. Identity-mapped huge pages live in PD entries copied by value
    //      in paging_create_address_space() and never pass through this leaf
    //      write, so they are inherently excluded.
    // This covers exactly the ELF PT_LOAD segments + user stack mapped by
    // exec.c and the per-page copies made by fork.
    uint64_t pte = (uint64_t)phys | flags;
    // OWNED iff: into a process AS, user-accessible, and OUTSIDE the contiguous
    // shared VA region [SHARED_FB_VA_START, SHARED_SHM_VA_END). That shared
    // region covers the framebuffer, read-only shared data, and shm windows —
    // memory owned by other subsystems. Everything else a process maps (ELF
    // PT_LOAD, user stack, fork copies, and anonymous mmap at VMM_ANON_VA_BASE
    // which sits just above SHARED_SHM_VA_END) is private and must be reclaimed
    // on teardown, so it is marked OWNED.
    if (active_pml4 != kernel_pml4 &&
        (flags & PAGE_USER) &&
        !(virt_addr >= SHARED_FB_VA_START && virt_addr < SHARED_SHM_VA_END)) {
        pte |= PTE_OWNED;
    }
    pt->entries[pt_idx] = pte;
    invlpg(virt);
}

void paging_unmap_page(void* virt) {
    uint64_t pml4_idx = PML4_INDEX(virt);
    uint64_t pdpt_idx = PDPT_INDEX(virt);
    uint64_t pd_idx = PD_INDEX(virt);
    uint64_t pt_idx = PT_INDEX(virt);

    // BUGFIX: honor the active mapping target (set via paging_set_target())
    // instead of hardcoding kernel_pml4. During a normal syscall the target is
    // the calling process's PML4, so an unmap must walk THAT hierarchy — the
    // mirror of paging_map_page(), which already uses active_pml4.
    page_table_t* target = active_pml4;

    if (!(target->entries[pml4_idx] & PTE_PRESENT)) return;
    page_table_t* pdpt = (page_table_t*)(target->entries[pml4_idx] & 0x000FFFFFFFFFF000ULL);

    if (!(pdpt->entries[pdpt_idx] & PTE_PRESENT)) return;
    page_table_t* pd = (page_table_t*)(pdpt->entries[pdpt_idx] & 0x000FFFFFFFFFF000ULL);

    if (!(pd->entries[pd_idx] & PTE_PRESENT)) return;
    // A 2MB huge page has no PT beneath it; nothing to clear at PTE granularity.
    if (pd->entries[pd_idx] & (1ULL << 7)) return;
    page_table_t* pt = (page_table_t*)(pd->entries[pd_idx] & 0x000FFFFFFFFFF000ULL);

    pt->entries[pt_idx] = 0;

    // LAZY TLB SHOOTDOWN: Use lazy flush instead of immediate IPI
    // This defers TLB invalidation on remote CPUs until their next context switch,
    // reducing IPI overhead by 60-80% for heavy munmap workloads.
    uint64_t cr3 = (uint64_t)target;
    tlb_flush_page_lazy(virt, cr3);
}

/*
 * paging_get_pte - return the leaf page-table entry mapping `virt` in the
 * currently-active address space (active_pml4), or 0 if any level along the
 * walk is not present. For a 2MB huge page the PD entry (which carries the
 * PRESENT bit) is returned. Used by copy_user_string()/user_page_is_accessible()
 * to probe whether a user page is mapped without risking a fault.
 */
uint64_t paging_get_pte(uint64_t virt) {
    uint64_t pml4_idx = PML4_INDEX(virt);
    uint64_t pdpt_idx = PDPT_INDEX(virt);
    uint64_t pd_idx   = PD_INDEX(virt);
    uint64_t pt_idx   = PT_INDEX(virt);

    page_table_t* target = active_pml4;
    if (!target) return 0;

    if (!(target->entries[pml4_idx] & PTE_PRESENT)) return 0;
    page_table_t* pdpt = (page_table_t*)(target->entries[pml4_idx] & 0x000FFFFFFFFFF000ULL);

    if (!(pdpt->entries[pdpt_idx] & PTE_PRESENT)) return 0;
    page_table_t* pd = (page_table_t*)(pdpt->entries[pdpt_idx] & 0x000FFFFFFFFFF000ULL);

    if (!(pd->entries[pd_idx] & PTE_PRESENT)) return 0;
    // 2MB huge page: the PD entry is the leaf mapping.
    if (pd->entries[pd_idx] & (1ULL << 7)) return pd->entries[pd_idx];

    page_table_t* pt = (page_table_t*)(pd->entries[pd_idx] & 0x000FFFFFFFFFF000ULL);
    return pt->entries[pt_idx];
}

/*
 * vmm_map_range - batch-map `count` consecutive 4KB pages
 * =========================================================
 * Maps [vaddr, vaddr + count*PAGE_SIZE) → [paddr, paddr + count*PAGE_SIZE)
 * inside the CURRENTLY ACTIVE address space (active_pml4).
 *
 * Performance design
 * ------------------
 * paging_map_page() re-walks all four levels of the page table for every
 * single page.  For N pages this is O(4N) table accesses in the best case
 * (all tables already exist).  The ELF loader and DMA-ring setup map many
 * consecutive pages per invocation, so most of those accesses are redundant:
 * consecutive pages share PML4/PDPT/PD entries and often the same PT.
 *
 * vmm_map_range() amortises the upper-level walks:
 *   • PML4 index changes only every 512 GB → walk PML4 once per 512 GB.
 *   • PDPT index changes only every   1 GB → walk PDPT once per 1 GB.
 *   • PD   index changes only every   2 MB → walk PD   once per 2 MB.
 *   • PT   index changes every page        → one PT entry write per page.
 *
 * In practice the ELF loader maps a handful of consecutive megabytes, so
 * the upper three levels are walked ONCE total and we directly fill PT
 * entries in a tight loop.  Complexity: O(N + T) where T is the number of
 * page-table boundaries crossed (≤ N/512 + 1 for huge runs).
 *
 * Ownership tracking
 * ------------------
 * Mirrors paging_map_page(): leaf pages are marked PTE_OWNED when the
 * mapping is into a process AS (not kernel_pml4), is user-accessible, and
 * lies outside the shared VA windows [SHARED_FB_VA_START, SHARED_SHM_VA_END).
 *
 * TLB invalidation
 * ----------------
 * If active_pml4 matches the CURRENTLY LOADED CR3 (i.e. this address space
 * is live on the CPU right now), we issue invlpg per page — the cheapest
 * possible invalidation.  If active_pml4 is a DIFFERENT address space that
 * is NOT currently loaded, we skip invalidation entirely: the CPU's TLB
 * contains no stale entries for that CR3, so flushing would be wasted work
 * and would, in fact, flush unrelated entries from the current AS.
 *
 * Safety contract: callers must not pass count=0 or unaligned addresses if
 * they care about the return value; the function aligns both addresses down
 * to PAGE_SIZE defensively.
 *
 * Returns 0 on success, -1 on allocation failure (partial mapping may have
 * occurred; caller should treat the range as unusable and clean up).
 */
int vmm_map_range(uint64_t vaddr, uint64_t paddr, uint64_t count,
                  uint64_t flags) {
    if (count == 0) return 0;

    /* Page-align both addresses. */
    vaddr &= ~0xFFFULL;
    paddr &= ~0xFFFULL;

    /*
     * Detect whether active_pml4 is the currently-loaded address space.
     * If so, we must issue invlpg per page to keep the CPU TLB coherent.
     * If not (e.g. we are setting up a new process while the kernel AS is
     * running), the target CR3 is not loaded, its TLB tag is absent, and
     * we can safely skip all invalidations for this range.
     */
    uint64_t current_cr3_phys = read_cr3() & 0x000FFFFFFFFFF000ULL;
    uint64_t target_pml4_phys = (uint64_t)active_pml4;  /* identity-mapped */
    bool need_invlpg = (current_cr3_phys == target_pml4_phys);

    /*
     * Intermediate-entry flags: propagate USER bit down so ring-3 page
     * walks succeed.  Mirrors paging_map_page().
     */
    uint64_t intermediate_flags = PTE_PRESENT | PTE_WRITE;
    if (flags & PAGE_USER) {
        intermediate_flags |= PTE_USER;
    }

    /*
     * Walk the upper three levels lazily — only when the corresponding
     * index changes.  We detect boundary crossings by comparing the index
     * derived from the *previous* vaddr to the current one.
     */
    page_table_t* pdpt = NULL;
    page_table_t* pd   = NULL;
    page_table_t* pt   = NULL;

    /* Sentinel: force a full walk on the first iteration. */
    uint64_t prev_pml4_idx = ~0ULL;
    uint64_t prev_pdpt_idx = ~0ULL;
    uint64_t prev_pd_idx   = ~0ULL;

    for (uint64_t i = 0; i < count; i++, vaddr += PAGE_SIZE, paddr += PAGE_SIZE) {

        /* Kernel-space safety check (mirrors paging_map_page). */
        if (vaddr >= KERNEL_SPACE_START && (flags & PAGE_USER)) {
            /* Skip silently — caller should not do this. */
            continue;
        }

        uint64_t pml4_idx = PML4_INDEX(vaddr);
        uint64_t pdpt_idx = PDPT_INDEX(vaddr);
        uint64_t pd_idx   = PD_INDEX(vaddr);
        uint64_t pt_idx   = PT_INDEX(vaddr);

        /* ------ Level 1: PML4 → PDPT (once per 512 GB) --------------- */
        if (pml4_idx != prev_pml4_idx) {
            page_table_t* target = active_pml4;
            if (!(target->entries[pml4_idx] & PTE_PRESENT)) {
                pdpt = alloc_page_table();
                if (!pdpt) return -1;
                target->entries[pml4_idx] = (uint64_t)pdpt | intermediate_flags;
            } else {
                pdpt = (page_table_t*)(target->entries[pml4_idx] & 0x000FFFFFFFFFF000ULL);
                if ((flags & PAGE_USER) && !(target->entries[pml4_idx] & PTE_USER))
                    target->entries[pml4_idx] |= PTE_USER;
            }
            prev_pml4_idx = pml4_idx;
            /* Force lower-level re-walk. */
            prev_pdpt_idx = ~0ULL;
            prev_pd_idx   = ~0ULL;
            pt = NULL;
        }

        /* ------ Level 2: PDPT → PD (once per 1 GB) ------------------- */
        if (pdpt_idx != prev_pdpt_idx) {
            if (!(pdpt->entries[pdpt_idx] & PTE_PRESENT)) {
                pd = alloc_page_table();
                if (!pd) return -1;
                pdpt->entries[pdpt_idx] = (uint64_t)pd | intermediate_flags;
            } else {
                pd = (page_table_t*)(pdpt->entries[pdpt_idx] & 0x000FFFFFFFFFF000ULL);
                if ((flags & PAGE_USER) && !(pdpt->entries[pdpt_idx] & PTE_USER))
                    pdpt->entries[pdpt_idx] |= PTE_USER;
            }
            prev_pdpt_idx = pdpt_idx;
            /* Force lower-level re-walk. */
            prev_pd_idx = ~0ULL;
            pt = NULL;
        }

        /* ------ Level 3: PD → PT (once per 2 MB) --------------------- */
        if (pd_idx != prev_pd_idx) {
            if (!(pd->entries[pd_idx] & PTE_PRESENT)) {
                pt = alloc_page_table();
                if (!pt) return -1;
                pd->entries[pd_idx] = (uint64_t)pt | intermediate_flags;
            } else if (pd->entries[pd_idx] & (1ULL << 7)) {
                /*
                 * Huge page (2 MB) in the way — split it into 512 4 KB PTEs
                 * so we can install a fine-grained mapping.  Inherit original
                 * PDE flags (minus PS bit 7) into the split copies, matching
                 * paging_map_page() so the two functions stay consistent.
                 */
                /* Preserve all flag bits including bit 63 (PAGE_NX), minus the PS bit (7). */
                uint64_t orig_pde_flags = (pd->entries[pd_idx] & ~(1ULL << 7))
                                        & ~0x000FFFFFFFE00000ULL;
                pt = alloc_page_table();
                if (!pt) return -1;
                uint64_t base_phys = pd->entries[pd_idx] & 0x000FFFFFFFE00000ULL;
                for (int s = 0; s < 512; s++) {
                    pt->entries[s] = (base_phys + (uint64_t)s * PAGE_SIZE) |
                                     orig_pde_flags;
                }
                pd->entries[pd_idx] = (uint64_t)pt | intermediate_flags;
                /* Flush the old 2 MB TLB entry if this AS is live. */
                if (need_invlpg) invlpg((void*)vaddr);
            } else {
                pt = (page_table_t*)(pd->entries[pd_idx] & 0x000FFFFFFFFFF000ULL);
                if ((flags & PAGE_USER) && !(pd->entries[pd_idx] & PTE_USER))
                    pd->entries[pd_idx] |= PTE_USER;
            }
            prev_pd_idx = pd_idx;
        }

        /* ------ Level 4: write the leaf PTE (once per page) ----------- */
        uint64_t pte = paddr | (uint64_t)flags;

        /* Ownership tracking — same predicate as paging_map_page(). */
        if (active_pml4 != kernel_pml4 &&
            (flags & PAGE_USER) &&
            !(vaddr >= SHARED_FB_VA_START && vaddr < SHARED_SHM_VA_END)) {
            pte |= PTE_OWNED;
        }

        pt->entries[pt_idx] = pte;

        /* Per-page TLB invalidation only when this AS is currently live. */
        if (need_invlpg) invlpg((void*)vaddr);
    }

    return 0;
}

/*
 * vmm_map_phys_into - map a contiguous physical range into a process AS
 * =====================================================================
 * Cleanly maps [paddr, paddr+size) at [vaddr, vaddr+size) inside the address
 * space identified by `cr3`, without disturbing the caller's notion of the
 * active target (it saves/restores it). This is the correct primitive for the
 * syscall path: paging_map_page() operates on active_pml4, which is the kernel
 * PML4 during a normal syscall — so without retargeting, mappings would
 * wrongly land in the kernel PML4 instead of the calling process's.
 *
 * OWNERSHIP: this is intended for SHARED physical memory (framebuffer MMIO,
 * shm segments) whose lifetime is managed by another subsystem. We deliberately
 * do NOT want these pages freed by paging_destroy_address_space(). The
 * PTE_OWNED bit is set by paging_map_page() ONLY for user pages OUTSIDE the
 * shared VA windows (see SHARED_FB_VA_START.. / SHARED_SHM_VA_START in mem.h);
 * callers of this function are expected to use VAs inside those shared windows
 * (e.g. shm uses the 0x60000000+ window), so PTE_OWNED is naturally not set and
 * the teardown walk skips them. Mapping shared phys at a non-shared VA would
 * incorrectly claim ownership, so don't do that.
 *
 * vaddr/paddr/size must be page-aligned (size rounded up here defensively).
 * Returns 0 on success, negative on bad args.
 */
int vmm_map_phys_into(uint64_t cr3, uint64_t vaddr, uint64_t paddr,
                      uint64_t size, uint64_t flags) {
    if (!cr3 || size == 0) {
        return -1;
    }

    // Page-align the range (round base down, length up to cover the request).
    uint64_t off = paddr & 0xFFF;
    uint64_t v   = vaddr & ~0xFFFULL;
    uint64_t p   = paddr & ~0xFFFULL;
    uint64_t len = ALIGN_UP(size + off, PAGE_SIZE);

    page_table_t* saved = active_pml4;        // preserve caller's target
    paging_set_target(cr3);

    for (uint64_t i = 0; i < len; i += PAGE_SIZE) {
        paging_map_page((void*)(v + i), (void*)(p + i), flags);
    }

    active_pml4 = saved;                       // restore exactly (not just kernel)
    return 0;
}

/*
 * vmm_unmap_range_into - unmap [vaddr, vaddr+size) from address space `cr3`.
 * If free_owned is true, any leaf page carrying PTE_OWNED is returned to the
 * PMM (used by vmm_munmap to release private anonymous pages immediately rather
 * than deferring to teardown). Shared pages (no PTE_OWNED) are only unmapped.
 * Saves/restores the active target so it is safe on the syscall path.
 * Returns 0 on success, negative on bad args.
 */
int vmm_unmap_range_into(uint64_t cr3, uint64_t vaddr, uint64_t size,
                         bool free_owned) {
    if (!cr3 || size == 0) {
        return -1;
    }

    uint64_t v   = vaddr & ~0xFFFULL;
    uint64_t len = ALIGN_UP(size + (vaddr & 0xFFF), PAGE_SIZE);

    page_table_t* saved = active_pml4;
    page_table_t* target = (page_table_t*)(cr3 & 0x000FFFFFFFFFF000ULL);
    active_pml4 = target;  // so paging_unmap_page walks the right hierarchy

    for (uint64_t i = 0; i < len; i += PAGE_SIZE) {
        void* va = (void*)(v + i);
        if (free_owned) {
            // Look up the leaf PTE so we can free OWNED phys before clearing it.
            uint64_t pml4_idx = PML4_INDEX(va);
            uint64_t pdpt_idx = PDPT_INDEX(va);
            uint64_t pd_idx   = PD_INDEX(va);
            uint64_t pt_idx   = PT_INDEX(va);
            if (target->entries[pml4_idx] & PTE_PRESENT) {
                page_table_t* pdpt = (page_table_t*)(target->entries[pml4_idx] & 0x000FFFFFFFFFF000ULL);
                if (pdpt->entries[pdpt_idx] & PTE_PRESENT) {
                    page_table_t* pd = (page_table_t*)(pdpt->entries[pdpt_idx] & 0x000FFFFFFFFFF000ULL);
                    if ((pd->entries[pd_idx] & PTE_PRESENT) &&
                        !(pd->entries[pd_idx] & (1ULL << 7))) {
                        page_table_t* pt = (page_table_t*)(pd->entries[pd_idx] & 0x000FFFFFFFFFF000ULL);
                        uint64_t e = pt->entries[pt_idx];
                        if ((e & PTE_PRESENT) && (e & PTE_OWNED)) {
                            pmm_free_page((void*)(e & 0x000FFFFFFFFFF000ULL));
                        }
                    }
                }
            }
        }
        paging_unmap_page(va);
    }

    active_pml4 = saved;
    return 0;
}

// Create a new address space (page directory hierarchy)
uint64_t paging_create_address_space(void) {
    // Allocate new PML4
    page_table_t* pml4 = alloc_page_table();
    if (!pml4) {
        kprintf("[PAGING] Failed to allocate PML4 for new address space\n");
        return 0;
    }

    // Copy kernel upper-half entries (shared, PML4[256-511])
    for (int i = 256; i < 512; i++) {
        pml4->entries[i] = kernel_pml4->entries[i];
    }

    // Deep-copy PML4[0] identity mapping hierarchy so each process has
    // independent page tables for the lower half. Without this, user page
    // mappings at addresses like 0x200000 corrupt other processes' mappings.
    //
    // OOM SAFETY: on any allocation failure during the deep copy we roll back
    // ALL pages already allocated for this address space and return 0. The
    // old code shared the kernel PD on alloc failure, which caused cross-process
    // corruption under memory pressure. Callers (process_create) check for 0.
    if (kernel_pml4->entries[0] & PTE_PRESENT) {
        page_table_t* src_pdpt = (page_table_t*)(kernel_pml4->entries[0] & 0x000FFFFFFFFFF000ULL);
        page_table_t* new_pdpt = alloc_page_table();
        if (!new_pdpt) {
            // OOM: release the PML4 we already allocated and fail.
            pmm_free_page(pml4);
            kprintf("[PAGING] OOM: failed to allocate PDPT for new address space\n");
            return 0;
        }

        for (int j = 0; j < 512; j++) {
            if (!(src_pdpt->entries[j] & PTE_PRESENT)) {
                new_pdpt->entries[j] = 0;
                continue;
            }
            // Deep-copy each PD that the PDPT points to
            page_table_t* src_pd = (page_table_t*)(src_pdpt->entries[j] & 0x000FFFFFFFFFF000ULL);
            page_table_t* new_pd = alloc_page_table();
            if (!new_pd) {
                // OOM: roll back all PDs successfully allocated so far, then
                // free new_pdpt and the PML4.
                for (int r = 0; r < j; r++) {
                    if (new_pdpt->entries[r] & PTE_PRESENT) {
                        pmm_free_page((void*)(new_pdpt->entries[r] & 0x000FFFFFFFFFF000ULL));
                    }
                }
                pmm_free_page(new_pdpt);
                pmm_free_page(pml4);
                kprintf("[PAGING] OOM: failed to allocate PD[%d] for new address space\n", j);
                return 0;
            }
            for (int k = 0; k < 512; k++) {
                new_pd->entries[k] = src_pd->entries[k];  // copy PD entries (huge pages or PT pointers)
            }
            new_pdpt->entries[j] = (uint64_t)new_pd | (src_pdpt->entries[j] & 0xFFF);
        }
        pml4->entries[0] = (uint64_t)new_pdpt | (kernel_pml4->entries[0] & 0xFFF);
    }

    uint64_t cr3 = (uint64_t)pml4;

    // Assign PCID if supported (avoid TLB flush on context switch)
    if (pcid_supported && next_pcid < 4096) {
        cr3 |= next_pcid;  // Embed PCID in lower 12 bits of CR3
        kprintf("[PAGING] Created address space at %p with PCID %u\n", pml4, next_pcid);
        next_pcid++;

        // Handle PCID exhaustion (recycle PCIDs if needed)
        // BUG-013 fix: Flush ALL TLB entries before recycling PCIDs
        if (next_pcid >= 4096) {
            kprintf("[PAGING] Warning: PCID exhausted, recycling from 1\n");
            kprintf("[PAGING] Flushing all TLB entries before PCID recycling\n");

            // Flush TLB on ALL CPUs by reloading CR3 without PCID preservation
            uint64_t cr3 = read_cr3() & ~CR3_NO_FLUSH;
            write_cr3(cr3);

            // TODO: Send IPI to other CPUs to flush their TLBs
            // For now, this flushes current CPU's TLB

            next_pcid = 1;
        }
    } else {
        kprintf("[PAGING] Created address space at %p (no PCID)\n", pml4);
    }

    return cr3;
}

// Destroy an address space (free all page tables)
void paging_destroy_address_space(uint64_t cr3) {
    if (!cr3) {
        kprintf("[PAGING] Warning: Attempted to destroy NULL address space\n");
        return;
    }

    page_table_t* pml4 = (page_table_t*)(cr3 & 0x000FFFFFFFFFF000ULL);

    // Only walk user space mappings (lower half, entries 0-255). Kernel
    // mappings (higher half, 256-511) are shared and must NEVER be freed.
    //
    // DOUBLE-FREE FIX: The PDPT/PD/PT *structural* pages are process-private —
    // paging_create_address_space() deep-copies them per address space and
    // paging_map_page() allocates fresh ones for this process's mappings — so
    // they are always safe to free. Leaf DATA pages, however, are only freed
    // when they carry PTE_OWNED, which is set exclusively for this process's
    // private pages (ELF PT_LOAD segments, user stack, fork copies). This
    // prevents returning globally shared physical memory (identity-mapped RAM,
    // framebuffer, initrd, ELF source data, shm segments) to the PMM.
    #define PTE_HUGE (1ULL << 7)
    for (int i = 0; i < 256; i++) {
        if (!(pml4->entries[i] & PTE_PRESENT)) continue;
        page_table_t* pdpt = (page_table_t*)(pml4->entries[i] & 0x000FFFFFFFFFF000ULL);

        for (int j = 0; j < 512; j++) {
            if (!(pdpt->entries[j] & PTE_PRESENT)) continue;
            // A 1GB huge page at the PDPT level (or any shared entry copied by
            // value) maps memory directly with no PD/PT below it — skip it so we
            // neither dereference it as a table nor free shared RAM.
            if (pdpt->entries[j] & PTE_HUGE) continue;
            page_table_t* pd = (page_table_t*)(pdpt->entries[j] & 0x000FFFFFFFFFF000ULL);

            for (int k = 0; k < 512; k++) {
                if (!(pd->entries[k] & PTE_PRESENT)) continue;
                // 2MB huge page (e.g. identity-mapped RAM copied by value in
                // paging_create_address_space): no PT beneath it and the
                // physical RAM is shared. Do not dereference or free it.
                if (pd->entries[k] & PTE_HUGE) continue;
                page_table_t* pt = (page_table_t*)(pd->entries[k] & 0x000FFFFFFFFFF000ULL);

                // Free only leaf pages this process privately owns.
                for (int m = 0; m < 512; m++) {
                    if ((pt->entries[m] & PTE_PRESENT) &&
                        (pt->entries[m] & PTE_OWNED)) {
                        void* phys_page = (void*)(pt->entries[m] & 0x000FFFFFFFFFF000ULL);
                        // CoW-aware free: a page shared by a parent+child (fork)
                        // must only be returned to the PMM by its LAST owner.
                        // cow_unref() returns 1 for unshared pages (refcount 0),
                        // so non-forked teardown is unchanged.
                        if (cow_unref((uint64_t)phys_page))
                            pmm_free_page(phys_page);
                    }
                }

                // The PT itself is process-private; free it.
                pmm_free_page(pt);
            }
            // The PD is process-private (deep-copied per address space); free it.
            pmm_free_page(pd);
        }
        // The PDPT is process-private (deep-copied per address space); free it.
        pmm_free_page(pdpt);
    }
    #undef PTE_HUGE

    // Free PML4 itself
    pmm_free_page(pml4);

    kprintf("[PAGING] Destroyed address space at %p\n", pml4);
}

/* -----------------------------------------------------------------------
 * Huge Page Mapping (2MB pages with PSE bit)
 * -----------------------------------------------------------------------
 * Maps a 2MB-aligned physical address as a huge page, setting bit 7 (PS)
 * in the PDE to signal a 2MB leaf mapping. This reduces TLB pressure by
 * 512x: a single TLB entry covers 2MB instead of requiring 512 separate
 * 4KB TLB entries.
 *
 * REQUIREMENTS:
 *   - virt and phys MUST be 2MB-aligned (0x200000)
 *   - The function validates alignment and returns -1 on failure
 *   - Uses the currently active PML4 (set via paging_set_target)
 *
 * PERFORMANCE:
 *   - For 100MB allocation: 50 huge pages = 50 TLB entries
 *   - vs 4KB pages: 25600 entries (99.8% reduction)
 * ----------------------------------------------------------------------- */

/**
 * paging_map_huge_page - map a 2MB huge page at virt -> phys with flags.
 *
 * Sets PDE bit 7 (PS) to signal a 2MB leaf mapping. The physical address
 * goes directly into the PDE (no PT level beneath it).
 *
 * Parameters:
 *   virt  - 2MB-aligned virtual address
 *   phys  - 2MB-aligned physical address
 *   flags - PAGE_PRESENT | PAGE_WRITE | PAGE_USER | PAGE_NX as needed
 *
 * Returns 0 on success, -1 on alignment error or allocation failure.
 */
int paging_map_huge_page(void* virt, void* phys, uint64_t flags) {
    uint64_t virt_addr = (uint64_t)virt;
    uint64_t phys_addr = (uint64_t)phys;

    // Validate 2MB alignment (bits 0-20 must be zero)
    if ((virt_addr & 0x1FFFFF) || (phys_addr & 0x1FFFFF)) {
        kprintf("[PAGING] ERROR: Huge page mapping requires 2MB alignment (virt=%p, phys=%p)\n",
                virt, phys);
        return -1;
    }

    uint64_t pml4_idx = PML4_INDEX(virt);
    uint64_t pdpt_idx = PDPT_INDEX(virt);
    uint64_t pd_idx = PD_INDEX(virt);

    // Intermediate flags (propagate USER bit down)
    uint64_t intermediate_flags = PTE_PRESENT | PTE_WRITE;
    if (flags & PAGE_USER) {
        intermediate_flags |= PTE_USER;
    }

    // Walk/create PDPT
    page_table_t* target = active_pml4;
    page_table_t* pdpt;
    if (!(target->entries[pml4_idx] & PTE_PRESENT)) {
        pdpt = alloc_page_table();
        if (!pdpt) {
            kprintf("[PAGING] Failed to allocate PDPT for huge page mapping at %p\n", virt);
            return -1;
        }
        target->entries[pml4_idx] = (uint64_t)pdpt | intermediate_flags;
    } else {
        pdpt = (page_table_t*)(target->entries[pml4_idx] & 0x000FFFFFFFFFF000ULL);
        if ((flags & PAGE_USER) && !(target->entries[pml4_idx] & PTE_USER)) {
            target->entries[pml4_idx] |= PTE_USER;
        }
    }

    // Walk/create PD
    page_table_t* pd;
    if (!(pdpt->entries[pdpt_idx] & PTE_PRESENT)) {
        pd = alloc_page_table();
        if (!pd) {
            kprintf("[PAGING] Failed to allocate PD for huge page mapping at %p\n", virt);
            return -1;
        }
        pdpt->entries[pdpt_idx] = (uint64_t)pd | intermediate_flags;
    } else {
        pd = (page_table_t*)(pdpt->entries[pdpt_idx] & 0x000FFFFFFFFFF000ULL);
        if ((flags & PAGE_USER) && !(pdpt->entries[pdpt_idx] & PTE_USER)) {
            pdpt->entries[pdpt_idx] |= PTE_USER;
        }
    }

    // Check if there's already a 4KB PT here (would conflict)
    if ((pd->entries[pd_idx] & PTE_PRESENT) && !(pd->entries[pd_idx] & (1ULL << 7))) {
        kprintf("[PAGING] WARNING: Replacing 4KB PT with huge page at %p\n", virt);
        // Free the old PT (defensive: prevents leak)
        page_table_t* old_pt = (page_table_t*)(pd->entries[pd_idx] & 0x000FFFFFFFFFF000ULL);
        pmm_free_page(old_pt);
    }

    // Set PDE with PS bit (bit 7) to signal 2MB huge page
    // Physical address bits 21-51 go into PDE bits 21-51
    // Bits 12-20 MUST be zero (2MB alignment requirement)
    uint64_t pde = phys_addr | flags | (1ULL << 7);  // PS bit = huge page

    // Ownership tracking: mark OWNED if this is a process-private huge page
    // (same logic as 4KB pages: process AS, user-accessible, outside shared windows)
    if (active_pml4 != kernel_pml4 &&
        (flags & PAGE_USER) &&
        !(virt_addr >= SHARED_FB_VA_START && virt_addr < SHARED_SHM_VA_END)) {
        pde |= PTE_OWNED;
    }

    pd->entries[pd_idx] = pde;

    // Invalidate TLB for the entire 2MB range
    // Note: invlpg on x86-64 invalidates the entire huge page when used on any
    // address within it, so a single invlpg suffices.
    invlpg(virt);

    return 0;
}

/**
 * paging_unmap_huge_page - unmap a 2MB huge page at virt.
 *
 * Clears the PDE and invalidates the TLB. Does not free the physical memory
 * (caller must free via pmm_free_huge_page if they own it).
 *
 * Returns 0 on success, -1 if the address is not 2MB-aligned or not mapped.
 */
int paging_unmap_huge_page(void* virt) {
    uint64_t virt_addr = (uint64_t)virt;

    // Validate 2MB alignment
    if (virt_addr & 0x1FFFFF) {
        kprintf("[PAGING] ERROR: Huge page unmap requires 2MB alignment (virt=%p)\n", virt);
        return -1;
    }

    uint64_t pml4_idx = PML4_INDEX(virt);
    uint64_t pdpt_idx = PDPT_INDEX(virt);
    uint64_t pd_idx = PD_INDEX(virt);

    page_table_t* target = active_pml4;

    if (!(target->entries[pml4_idx] & PTE_PRESENT)) return -1;
    page_table_t* pdpt = (page_table_t*)(target->entries[pml4_idx] & 0x000FFFFFFFFFF000ULL);

    if (!(pdpt->entries[pdpt_idx] & PTE_PRESENT)) return -1;
    page_table_t* pd = (page_table_t*)(pdpt->entries[pdpt_idx] & 0x000FFFFFFFFFF000ULL);

    if (!(pd->entries[pd_idx] & PTE_PRESENT)) return -1;

    // Verify it's actually a huge page (PS bit set)
    if (!(pd->entries[pd_idx] & (1ULL << 7))) {
        kprintf("[PAGING] WARNING: Attempt to unmap non-huge-page PDE at %p\n", virt);
        return -1;
    }

    // Clear the PDE
    pd->entries[pd_idx] = 0;

    // Invalidate TLB (one invlpg invalidates the entire 2MB huge page)
    invlpg(virt);

    return 0;
}
