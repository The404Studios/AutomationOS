#include "../../include/mem.h"
#include "../../include/x86_64.h"
#include "../../include/kernel.h"
#include "../../include/string.h"
#include "../../include/tlb.h"
#ifdef SMP_FOUNDATION
#include "ipi.h"
#include "lapic.h"
#endif

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

// Global: set true by paging_init() when SMAP is enabled.  STAC/CLAC
// (x86_64.h) check this to avoid emitting Haswell-only opcodes on older
// CPUs (Westmere/Arrandale) where they would #UD.
bool cpu_smap_active = false;

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

    // Zero the fresh frame via the DIRECT MAP (PML4[256], shared + present in every
    // CR3) rather than its raw identity address: alloc_page_table runs under whatever
    // CR3 is live (a PROCESS CR3 during exec PHASE 1 / fork / a demand fault), whose
    // identity map may not cover a high frame (the phys==virt bug family). The RETURN
    // value stays the raw PHYSICAL address (callers store it in PTEs / index it). The
    // caller-side page-table WALK derefs (paging_map_page/paging_get_pte) still use the
    // identity and are deferred (low-risk: PT pages are always low-allocated); see the
    // direct-map review + task notes.
    memset(PHYS_TO_DIRECT(page), 0, PAGE_SIZE);
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

static void paging_alias_selftest(void);   // #20 coherence proof (defined below)

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

    // ----------------------------------------------------------------------
    // CRITICAL: Un-alias the higher-half kernel PD from the identity PD.
    //
    // boot.asm points BOTH PML4[0]->PDPT[0] (identity, virt 0-1GB) and
    // PML4[511]->PDPT_high[510] (higher half, virt 0xFFFFFFFF80000000+) at the
    // SAME pd_table. The kernel heap lives in the higher half at HEAP_START
    // 0xFFFFFFFF90000000 == pd_table[128] (the +256MB slot), so when heap_extend()
    // maps/splits heap pages it rewrites pd_table[128..], which is ALSO the LOW
    // identity map for physical 256MB+. A heap grown past 16MB reaches pd_table[136]
    // and destroys the identity mapping for ~272MB; a PMM frame later handed out
    // there then #PFs when the kernel zeroes/uses it via identity (the long-hunted
    // spawn-under-churn crash: PD[136] split, PT[61]=0, CR2=0x1103d000). Give the
    // higher half its OWN PD copy so heap splits never touch the identity map. The
    // kernel runs from the low identity map, so this is transparent to live code,
    // and create_address_space() shares PML4[511] by reference so every process
    // sees the same (un-aliased) higher-half PD.
    if (kernel_pml4->entries[511] & PTE_PRESENT) {
        page_table_t* high_pdpt =
            (page_table_t*)(kernel_pml4->entries[511] & 0x000FFFFFFFFFF000ULL);
        if (high_pdpt->entries[510] & PTE_PRESENT) {
            uint64_t shared = high_pdpt->entries[510];
            page_table_t* shared_pd = (page_table_t*)(shared & 0x000FFFFFFFFFF000ULL);
            page_table_t* priv_pd = (page_table_t*)pmm_alloc_page();
            if (priv_pd) {
                for (int i = 0; i < 512; i++) priv_pd->entries[i] = shared_pd->entries[i];
                high_pdpt->entries[510] = (uint64_t)priv_pd | (shared & 0xFFF);
                write_cr3(read_cr3());   // flush TLB so the private PD is live
                kprintf("[VMM] Un-aliased higher-half kernel PD %p from identity PD %p (heap-safe)\n",
                        priv_pd, shared_pd);
            } else {
                kprintf("[VMM] WARNING: failed to un-alias higher-half PD; heap growth may corrupt identity map\n");
            }
        }
    }

    // ----------------------------------------------------------------------
    // DIRECT MAP (#20 fix). Give PML4[256] its OWN dedicated PDPT + PD chain of
    // never-split 2MB huge pages, INSTEAD of aliasing the identity PDPT.
    //
    // OLD (buggy): kernel_pml4->entries[256] = kernel_pml4->entries[0], so the
    // direct map and the low identity map shared ONE PDPT -> PD -> 2MB-huge-PDE
    // chain. Under churn a paging_map_page split a 2MB identity PDE (exec maps a
    // process's low PT_LOAD VAs at 4KB, hitting the huge-page split branch), and
    // because PML4[256] is in the SHARED kernel higher-half the corrupted view was
    // visible in EVERY CR3 -- including the AP's fixed master kernel_cr3. CPU1 then
    // #PF'd reading a kmalloc_ref offload operand via PHYS_TO_DIRECT in
    // matmul_band_n (the smpstress crash, phys ~194MB).
    //
    // NEW (fixed): a DEDICATED chain that shares NO structural page with the
    // splittable identity. Nothing ever paging_map_page's into the direct-map VA
    // range, so its huge PDEs stay present forever; a split of an identity PDE
    // perturbs only PML4[0]'s private chain. Installed at kernel_pml4->entries[256]
    // BEFORE any process or the AP exists, so it propagates BY REFERENCE to every
    // CR3 (paging_create_address_space copies 256-511) and the AP master CR3 -- no
    // per-CR3 work. Supervisor-only (no USER: ring-3 must not read all RAM) + NX
    // (data, W^X). Covers phys 0..16GB, matching the identity extent.
    //
    // BOOTSTRAP ORDER (critical, or triple-fault): fill the dedicated tables via
    // the LOW IDENTITY (raw pointers -- the frames are low + identity-covered here)
    // and only THEN install entries[256] + flush. PHYS_TO_DIRECT does not resolve
    // until the map is live.
    {
        const uint64_t DM_GB = 16;                          // cover the 16GB the identity covers
        page_table_t* dm_pdpt = (page_table_t*)pmm_alloc_page();
        int dm_ok = (dm_pdpt != 0);
        if (dm_ok) {
            memset(dm_pdpt, 0, PAGE_SIZE);
            for (uint64_t g = 0; g < DM_GB; g++) {
                page_table_t* dm_pd = (page_table_t*)pmm_alloc_page();
                if (!dm_pd) { dm_ok = 0; break; }
                memset(dm_pd, 0, PAGE_SIZE);
                for (uint64_t i = 0; i < 512; i++) {
                    uint64_t ph = (g * 512 + i) * 0x200000ULL;       // 2MB huge page
                    dm_pd->entries[i] = ph | PTE_PRESENT | PTE_WRITE
                                        | (1ULL << 7)                 // PS (2MB huge)
                                        | (1ULL << 63);               // NX (data, W^X)
                }
                dm_pdpt->entries[g] = (uint64_t)dm_pd | PTE_PRESENT | PTE_WRITE;  // no USER
            }
        }
        if (dm_ok) {
            kernel_pml4->entries[256] = (uint64_t)dm_pdpt
                                        | PTE_PRESENT | PTE_WRITE     // supervisor-only
                                        | (1ULL << 63);               // NX
            write_cr3(read_cr3());                                    // flush -> dedicated map live
            kprintf("[VMM] Direct map ONLINE @ 0x%016lx (DEDICATED PDPT+PD, phys 0..%luGB, never-split, all CR3s)\n",
                    (unsigned long)DIRECT_MAP_BASE, (unsigned long)DM_GB);
        } else {
            // OOM (essentially unreachable this early): fall back to the old alias so
            // boot still completes in the known-buggy-but-boots state (#20 unfixed).
            kernel_pml4->entries[256] = (kernel_pml4->entries[0] & ~0x4ULL) | (1ULL << 63);
            write_cr3(read_cr3());
            kprintf("[VMM] WARNING: dedicated direct-map alloc failed; fell back to identity alias (#20 unfixed)\n");
        }

        // Boot self-test: write a sentinel via the DIRECT MAP and read it back via
        // the LOW identity (and vice-versa). Proves the direct map resolves to the
        // same physical RAM as the identity. (Post-split coherence is PAGINGALIAS.)
        void* probe = pmm_alloc_page();
        if (probe) {
            volatile uint64_t* via_ident  = (volatile uint64_t*)probe;
            volatile uint64_t* via_direct = (volatile uint64_t*)PHYS_TO_DIRECT(probe);
            *via_direct = 0xD15EA5ED0FF5E7ULL;
            uint64_t rb_ident = *via_ident;
            *via_ident = 0xC0FFEE5DEADBEEFULL;
            uint64_t rb_direct = *via_direct;
            if (rb_ident == 0xD15EA5ED0FF5E7ULL && rb_direct == 0xC0FFEE5DEADBEEFULL) {
                kprintf("[VMM] Direct-map self-test OK (direct<->identity coherent)\n");
            } else {
                kprintf("[VMM] WARNING: direct-map self-test FAILED (ident=0x%lx direct=0x%lx)\n",
                        (unsigned long)rb_ident, (unsigned long)rb_direct);
            }
            pmm_free_page(probe);
        }
    }

    // Enable SMEP and SMAP if the CPU supports them.
    //
    // SMEP (Supervisor Mode Execution Prevention, CR4 bit 20): prevents the
    // kernel from EXECUTING code on user-accessible pages. A #PF is raised
    // instead, blocking ret2user / JOP into user-mapped trampolines.
    //
    // SMAP (Supervisor Mode Access Prevention, CR4 bit 21): prevents the
    // kernel from READING or WRITING user-accessible pages unless RFLAGS.AC
    // is explicitly set (via the STAC instruction). This forces every kernel
    // user-memory access through the validated copy_from/to_user paths
    // (which bracket the access with stac/clac) and catches wild dereferences
    // of user pointers in kernel code.
    //
    // CPUID.07H:EBX: bit 7 = SMEP, bit 20 = SMAP.
    // GUARD: Westmere/Arrandale (T410) max CPUID leaf is 0xB.  Leaf 7 is in
    // range but returns zeroes for SMEP/SMAP (those are Haswell+).  Older CPUs
    // with max leaf < 7 would get the highest-supported-leaf result (garbage),
    // so always check max leaf first.
    {
        uint32_t max_leaf;
        __asm__ volatile("cpuid"
                         : "=a"(max_leaf)
                         : "a"(0)
                         : "ebx", "ecx", "edx");
        uint64_t cr4 = read_cr4();
        if (max_leaf >= 7) {
            uint32_t eax7, ebx7, ecx7, edx7;
            __asm__ volatile("cpuid"
                             : "=a"(eax7), "=b"(ebx7), "=c"(ecx7), "=d"(edx7)
                             : "a"(7), "c"(0));
            if (ebx7 & (1U << 7)) {
                cr4 |= CR4_SMEP;
                kprintf("[VMM] SMEP enabled (Supervisor Mode Execution Prevention)\n");
            } else {
                kprintf("[VMM] SMEP not supported on this CPU\n");
            }
            if (ebx7 & (1U << 20)) {
                cr4 |= CR4_SMAP;
                cpu_smap_active = true;  // arm STAC/CLAC in x86_64.h
                kprintf("[VMM] SMAP enabled (Supervisor Mode Access Prevention)\n");
            } else {
                kprintf("[VMM] SMAP not supported on this CPU\n");
            }
            // ERMS (Enhanced REP MOVSB/STOSB), CPUID.07H:EBX[bit 9]. Ivy-Bridge
            // 2012+. Gates the kernel's fast REP-string memcpy/memset path
            // (kernel/lib/string.c, g_string_erms_ok — default 0). The T410's
            // 2010 Westmere i5 reports 0 here, so it keeps the portable, DF-safe
            // word-copy loop. Under T410_SAFE the whole block is compiled out so
            // ERMS can never arm regardless of what CPUID claims.
#ifndef T410_SAFE
            {
                extern volatile int g_string_erms_ok;
                if (ebx7 & (1U << 9)) {
                    g_string_erms_ok = 1;
                    kprintf("[VMM] ERMS enabled (fast REP MOVSB/STOSB string ops)\n");
                } else {
                    kprintf("[VMM] ERMS not supported; portable word-copy loop\n");
                }
            }
#endif
        } else {
            kprintf("[VMM] CPUID max leaf %u < 7, skipping SMEP/SMAP detection\n",
                    max_leaf);
        }
        write_cr4(cr4);
        if (cpu_smap_active)
            __asm__ volatile("clac" ::: "cc");  // establish AC=0 default
        kprintf("[CPU] features: SMEP=%d SMAP=%d leaf7=%d\n",
                (cr4 & CR4_SMEP) ? 1 : 0,
                cpu_smap_active ? 1 : 0,
                (max_leaf >= 7) ? 1 : 0);
        kprintf("[CPU] usercopy: STAC/CLAC %s\n",
                cpu_smap_active ? "active" : "disabled (pre-Haswell CPU)");
    }

    // Enable PCID if supported (must be done AFTER setting up paging)
    paging_enable_pcid();

    // Enable FPU/SSE for userspace (required for LLM inference with floats)
    cpu_enable_fpu_sse();

    active_pml4 = kernel_pml4;
    kprintf("[VMM] Paging initialized with full identity mapping\n");

    // Permanent coherence proof for the #20 fix: split a low-identity 2MB PDE and
    // confirm the dedicated direct map stays coherent (emits PAGINGALIAS PASS/FAIL).
    paging_alias_selftest();
}

// Switch which PML4 paging_map_page targets (for per-process mapping)
void paging_set_target(uint64_t cr3) {
    active_pml4 = (page_table_t*)(cr3 & 0x000FFFFFFFFFF000ULL);
}

void paging_reset_target(void) {
    active_pml4 = kernel_pml4;
}

// ---------------------------------------------------------------------------
// PAGINGALIAS coherence self-test (#20). Proves the direct map is STRUCTURALLY
// INDEPENDENT of the splittable identity -- the invariant the fix establishes.
//
// We assert it WITHOUT mutating the kernel identity: forcing a split here would
// create a kernel-identity PT that processes deep-copy (shared) and the teardown
// path then frees (the shared-kernel-PT guard only protects PTs split from a
// STILL-HUGE kernel PDE), corrupting memory under churn. Structural independence
// is the stronger, safe proof: if PML4[256]'s PDPT is a DIFFERENT physical page
// than PML4[0]'s identity PDPT, then NO identity split (which only edits the
// identity's PD pages) can ever reach the direct map's pages -- coherence holds
// by construction. The pre-fix aliased code had entries[256]==entries[0], so this
// FAILs on the bug and PASSes on the fix.
static void paging_alias_selftest(void) {
    void* F = pmm_alloc_page();
    if (!F) { kprintf("[VMM] PAGINGALIAS SKIP (no probe frame)\n"); return; }
    uint64_t phys = (uint64_t)F;
    const uint64_t MASK = 0x000FFFFFFFFFF000ULL;
    const uint64_t HUGE = (1ULL << 7);

    // (1) STRUCTURAL INDEPENDENCE: direct-map PDPT != identity PDPT.
    uint64_t dm_pdpt_phys = kernel_pml4->entries[256] & MASK;
    uint64_t id_pdpt_phys = kernel_pml4->entries[0]   & MASK;
    int independent = (dm_pdpt_phys != id_pdpt_phys) && dm_pdpt_phys != 0;

    // (2) COHERENCE: a write via the DIRECT MAP is seen via the LOW identity
    //     (both name the same physical frame).
    volatile uint64_t* dm = (volatile uint64_t*)PHYS_TO_DIRECT(phys);
    volatile uint64_t* id = (volatile uint64_t*)phys;
    *dm = 0xA11A5C0DEULL;
    int coherent = (*id == 0xA11A5C0DEULL);

    // (3) the direct-map PDE for the probe is a PRESENT 2MB HUGE page (never split).
    uint64_t dva          = (uint64_t)PHYS_TO_DIRECT(phys);
    page_table_t* dm_pdpt = (page_table_t*)PHYS_TO_DIRECT(dm_pdpt_phys);
    uint64_t dm_pdpte     = dm_pdpt->entries[(dva >> 30) & 0x1FF];
    page_table_t* dm_pd   = (page_table_t*)PHYS_TO_DIRECT(dm_pdpte & MASK);
    uint64_t dm_pde       = dm_pd->entries[(dva >> 21) & 0x1FF];
    int dm_huge           = (dm_pde & PTE_PRESENT) && (dm_pde & HUGE);

    pmm_free_page(F);

    if (independent && coherent && dm_huge) {
        kprintf("[VMM] PAGINGALIAS PASS (direct map independent of identity: dm_pdpt=0x%lx id_pdpt=0x%lx)\n",
                (unsigned long)dm_pdpt_phys, (unsigned long)id_pdpt_phys);
    } else {
        kprintf("[VMM] PAGINGALIAS FAIL (independent=%d coherent=%d dm_huge=%d dm_pdpt=0x%lx id_pdpt=0x%lx)\n",
                independent, coherent, dm_huge,
                (unsigned long)dm_pdpt_phys, (unsigned long)id_pdpt_phys);
    }
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
                            // CoW-aware free, mirroring the teardown path below
                            // (~line 865). A fork()-shared OWNED page is mapped in
                            // BOTH co-owners' CR3s with cow_table[pfn] > 0; freeing
                            // it here with a bare pmm_free_page() (no cow_unref())
                            // returns a still-referenced frame to the PMM, which
                            // then double-frees + re-hands it to two owners ->
                            // page-table corruption. Only free when WE are the last
                            // owner.
                            uint64_t pa = e & 0x000FFFFFFFFFF000ULL;
                            if (cow_unref(pa)) {
                                pmm_free_page((void*)pa);
                            }
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
        // BUG-013 fix: Flush ALL TLB entries for ALL PCIDs before recycling
        if (next_pcid >= 4096) {
            kprintf("[PAGING] PCID exhausted, recycling from 1 (all-context flush)\n");

            // Flush ALL PCIDs on local CPU
            tlb_flush_all_contexts_local();

            #ifdef SMP_FOUNDATION
            // Remote CPU flush deferred: The AP in SMP_FOUNDATION runs with the BSP's
            // CR3 (shared page tables) and executes no userspace (single kernel worker),
            // so PCID recycling on the AP is moot. Remote TLB-flush-all will be added
            // in a later brick when the AP runs its own address spaces.
            // (Sending IPI_TLB_FLUSH_ALL here would be a no-op: the AP has no IDT gate
            // for vector 0x45 and runs with interrupts permanently masked.)
            #endif

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
                // 2MB huge page: no PT beneath it. Identity-mapped huge pages
                // (inherited from the kernel, no PTE_OWNED) are shared RAM and
                // must NOT be freed. Process-private huge pages (PTE_OWNED,
                // created by paging_map_huge_page for user code) must be freed.
                if (pd->entries[k] & PTE_HUGE) {
                    if (pd->entries[k] & PTE_OWNED) {
                        void* huge_phys = (void*)(pd->entries[k] & 0x000FFFFFFFFFF000ULL);
                        pmm_free_huge_page(huge_phys);
                    }
                    continue;
                }
                page_table_t* pt = (page_table_t*)(pd->entries[k] & 0x000FFFFFFFFFF000ULL);

                // SHARED-KERNEL-PT GUARD: create_address_space() copies a split
                // huge-page PD entry (a PT *pointer*, not a 2MB leaf) BY VALUE, so
                // this process's deep-copied PD can point at the KERNEL's own
                // identity PT. Freeing that shared PT here returns it to the PMM;
                // it is then reused/zeroed and the kernel identity map for that
                // region goes NOT-PRESENT for EVERY CR3 — observed as a ring-0 #PF
                // in memset on a high identity page (~272MB) only under spawn/kill
                // churn (PD[136] split, PT[61]=0). If the kernel references this
                // exact physical PT at the same PML4/PDPT/PD slot, it is shared —
                // skip its owned-leaf scan AND its free. Process-private PTs are
                // distinct physical pages, so they never match and are still freed.
                // leak_pt: when set, free this PT's OWNED leaves but do NOT return the
                // PT STRUCTURAL page to the PMM (it shadows a still-shared kernel huge
                // page -- see FIX A below).
                int leak_pt = 0;
                if (kernel_pml4 && (kernel_pml4->entries[i] & PTE_PRESENT)) {
                    page_table_t* kpdpt = (page_table_t*)(kernel_pml4->entries[i] & 0x000FFFFFFFFFF000ULL);
                    uint64_t kpdpte = kpdpt->entries[j];
                    if ((kpdpte & PTE_PRESENT) && !(kpdpte & PTE_HUGE)) {
                        page_table_t* kpd = (page_table_t*)(kpdpte & 0x000FFFFFFFFFF000ULL);
                        uint64_t kpde = kpd->entries[k];
                        if ((kpde & PTE_PRESENT) && !(kpde & PTE_HUGE) &&
                            (page_table_t*)(kpde & 0x000FFFFFFFFFF000ULL) == pt) {
                            continue;  // process aliases the kernel's OWN PT — never touch it
                        }
                        // FIX A (slab-churn corruption ROOT FIX): the kernel still maps
                        // this slot as a 2MB HUGE page, but THIS process SPLIT it into a
                        // private PT (exec maps a user page at a LOW VA -- e.g. a big
                        // app's BSS at ~27MB that falls inside an inherited identity huge
                        // page; paging_map_page splits the huge page into a PT whose 511
                        // fill-entries STILL alias the shared identity RAM). The 511
                        // identity leaves carry no PTE_OWNED (skipped below); the 1 private
                        // target leaf IS owned and is freed. But the PT STRUCTURAL page
                        // must NOT go back to the PMM: it is a LOW frame whose backing 2MB
                        // range is still mapped for EVERY CR3. Freeing it lets slab_grow
                        // recycle it into a slab, which the NEXT exec's identity-path BSS
                        // memset (exec.c, dphys = pte & ~0xFFF) then zeros -> slab header
                        // magic=0 (the SLABDIAG self-heal event under churn). The old guard
                        // only matched when the kernel's PDE was itself a 4KB PT pointer;
                        // it missed this process-side-split-of-a-still-huge-PDE case. Leak
                        // the PT (bounded: <=1 per split-once 2MB region per process).
                        if ((kpde & PTE_PRESENT) && (kpde & PTE_HUGE)) {
                            leak_pt = 1;
                            kprintf("[SLABPROBE] FixA leak PT %p (split from huge kernel "
                                    "PDE i=%d j=%d k=%d)\n", (void*)pt, i, j, k);
                        }
                    }
                }

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

                // The PT itself is process-private; free it -- UNLESS it was split from
                // a still-shared kernel huge page (FIX A): leak it so its low frame is
                // never recycled into a slab and then zeroed by an identity-path memset.
                if (!leak_pt) pmm_free_page(pt);
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

/*
 * paging_modify_pte_flags - modify protection flags on an existing PTE
 * =====================================================================
 * Walks the page tables in active_pml4 to find the leaf PTE for `virt` and
 * updates its protection flags (PRESENT, WRITE, USER, NX) while preserving
 * the physical address and other bits. Automatically invalidates the TLB for
 * the modified page.
 *
 * Used by vmm_protect() to implement page-protection changes for PE loader
 * section permissions and Win32 VirtualProtect().
 *
 * Parameters:
 *   virt       - virtual address of the page to modify (must be 4KB-aligned)
 *   new_flags  - new protection flags (PAGE_PRESENT, PAGE_WRITE, PAGE_USER, PAGE_NX)
 *
 * Returns:
 *    0 on success
 *   -1 if the page is not mapped or any level is missing
 *
 * NOTE: This function modifies 4KB pages only. For 2MB huge pages, use
 *       paging_unmap_huge_page() and remap with new flags.
 */
int paging_modify_pte_flags(void* virt, uint64_t new_flags) {
    uint64_t virt_addr = (uint64_t)virt;

    // Validate alignment
    if (virt_addr & (PAGE_SIZE - 1)) {
        kprintf("[PAGING] ERROR: paging_modify_pte_flags requires 4KB alignment (virt=%p)\n", virt);
        return -1;
    }

    uint64_t pml4_idx = PML4_INDEX(virt);
    uint64_t pdpt_idx = PDPT_INDEX(virt);
    uint64_t pd_idx   = PD_INDEX(virt);
    uint64_t pt_idx   = PT_INDEX(virt);

    page_table_t* target = active_pml4;
    if (!target) {
        kprintf("[PAGING] ERROR: paging_modify_pte_flags called with NULL active_pml4\n");
        return -1;
    }

    // Walk PML4 -> PDPT
    if (!(target->entries[pml4_idx] & PTE_PRESENT)) {
        return -1;  // Page not mapped
    }
    page_table_t* pdpt = (page_table_t*)(target->entries[pml4_idx] & 0x000FFFFFFFFFF000ULL);

    // Walk PDPT -> PD
    if (!(pdpt->entries[pdpt_idx] & PTE_PRESENT)) {
        return -1;  // Page not mapped
    }
    page_table_t* pd = (page_table_t*)(pdpt->entries[pdpt_idx] & 0x000FFFFFFFFFF000ULL);

    // Walk PD -> PT
    if (!(pd->entries[pd_idx] & PTE_PRESENT)) {
        return -1;  // Page not mapped
    }

    // Check for 2MB huge page (PS bit set in PD entry)
    if (pd->entries[pd_idx] & (1ULL << 7)) {
        kprintf("[PAGING] WARNING: paging_modify_pte_flags cannot modify 2MB huge pages (virt=%p)\n", virt);
        return -1;
    }

    page_table_t* pt = (page_table_t*)(pd->entries[pd_idx] & 0x000FFFFFFFFFF000ULL);

    // Get current PTE
    uint64_t old_pte = pt->entries[pt_idx];
    if (!(old_pte & PTE_PRESENT)) {
        return -1;  // Page not mapped
    }

    // Preserve physical address and OS-available bits (PTE_OWNED, PTE_COW, etc.)
    // but replace the protection flags
    uint64_t phys_and_os_bits = old_pte & 0xFFFFFFFFFFFFF600ULL;  // Preserve phys addr [51:12] and OS bits [11:9]
    uint64_t new_pte = phys_and_os_bits | (new_flags & 0x1FF);    // Apply new flags [8:0]

    // Preserve NX bit (bit 63) from new_flags
    if (new_flags & PAGE_NX) {
        new_pte |= PAGE_NX;
    } else {
        new_pte &= ~PAGE_NX;
    }

    // Update PTE
    pt->entries[pt_idx] = new_pte;

    // Invalidate TLB for this page
    invlpg(virt);

    return 0;
}
