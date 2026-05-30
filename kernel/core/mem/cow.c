// Copy-on-write page sharing for fork() (#20).
// =============================================================================
// fork() used to eagerly deep-copy every user page (handlers.c). CoW instead
// SHARES the parent's physical pages with the child read-only and marks both
// PTEs with PTE_COW; the first WRITE to such a page traps into the page-fault
// handler, which hands the writer a private copy (or, if it is the sole
// remaining owner, simply re-grants write access in place).
//
// Ownership accounting
// --------------------
// A per-physical-frame refcount tracks the number of EXTRA owners beyond the
// first (so an unshared page has refcount 0 and behaves exactly as before —
// this is what keeps non-forking workloads byte-for-byte unchanged):
//     refcount[pfn] == 0  -> exactly one owner (or untracked)
//     refcount[pfn] == N  -> N+1 owners share the frame copy-on-write
//   cow_incref()  : +1 owner (fork shares a page).         0 on overflow.
//   cow_unref()   : -1 owner. Returns 1 iff the CALLER was the LAST owner and
//                   should therefore pmm_free_page() the frame; 0 if others
//                   still hold it. Used by BOTH the CoW write path and
//                   paging_destroy_address_space() teardown.
//
// The table is allocated from the PMM at boot (NOT a static array — the kernel
// .bss has only a few KB of headroom before the GRUB-placed initrd, so a 1 MB
// static table would corrupt it). cow_init() must run after the PMM + heap are
// up. If the allocation fails CoW disables itself and fork falls back to eager
// copying, so the system still works (just without the memory savings).

#include "../../include/mem.h"
#include "../../include/kernel.h"
#include "../../include/string.h"
#include "../../include/spinlock.h"
#include "../../include/x86_64.h"   // read_cr3

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define COW_PHYS_MASK 0x000FFFFFFFFFF000ULL
#define COW_MAX_OWNERS 255            // uint8_t saturation

// One byte per 4KB frame, covering the same 4 GB PFN space as the PMM bitmap.
#define COW_TABLE_ENTRIES (1UL << 20) // 2^20 frames = 4 GB / 4 KB

static uint8_t*  cow_table = NULL;     // refcount[pfn] (extra owners)
static uint64_t  cow_table_len = 0;    // number of entries (0 => CoW disabled)
static spinlock_t cow_lock;

static inline void cow_invlpg(uint64_t va) {
    __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
}

void cow_init(void) {
    spin_lock_init(&cow_lock);
    uint64_t entries = COW_TABLE_ENTRIES;
    size_t bytes = (size_t)entries * sizeof(uint8_t);          // 1 MB
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;        // 256
    cow_table = (uint8_t*)pmm_alloc_pages(pages);
    if (!cow_table) {
        cow_table_len = 0;
        kprintf("[COW] disabled: refcount table alloc (%lu pages) failed\n",
                (unsigned long)pages);
        return;
    }
    memset(cow_table, 0, bytes);
    cow_table_len = entries;
    kprintf("[COW] refcount table: %lu entries (%lu KB) at %p\n",
            (unsigned long)entries, (unsigned long)(bytes / 1024), cow_table);
}

int cow_enabled(void) {
    return cow_table != NULL && cow_table_len != 0;
}

// Register one ADDITIONAL owner of `phys`. Returns 1 on success, 0 if the frame
// cannot be tracked (table disabled / out of range / refcount saturated) — in
// which case the caller must fall back to making a private copy.
int cow_incref(uint64_t phys) {
    uint64_t pfn = (phys & COW_PHYS_MASK) >> 12;
    if (!cow_table || pfn >= cow_table_len) return 0;
    int ok;
    spin_lock(&cow_lock);
    if (cow_table[pfn] >= COW_MAX_OWNERS) {
        ok = 0;
    } else {
        cow_table[pfn]++;
        ok = 1;
    }
    spin_unlock(&cow_lock);
    return ok;
}

// Drop one owner of `phys`. Returns 1 iff the caller was the LAST owner and is
// responsible for freeing the frame; 0 if other owners remain. Untracked frames
// (table disabled / out of range / refcount already 0) return 1 so existing
// free paths behave exactly as before.
int cow_unref(uint64_t phys) {
    uint64_t pfn = (phys & COW_PHYS_MASK) >> 12;
    if (!cow_table || pfn >= cow_table_len) return 1;
    int last;
    spin_lock(&cow_lock);
    if (cow_table[pfn] > 0) {
        cow_table[pfn]--;
        last = 0;
    } else {
        last = 1;
    }
    spin_unlock(&cow_lock);
    return last;
}

// Walk the LIVE address space (current CR3) to the leaf PTE for `va`, returning
// a writable pointer to it, or NULL if there is no 4KB leaf mapping.
static uint64_t* cow_walk_pte(uint64_t va) {
    uint64_t* pml4 = (uint64_t*)(read_cr3() & COW_PHYS_MASK);
    uint64_t i4 = (va >> 39) & 0x1FF, i3 = (va >> 30) & 0x1FF;
    uint64_t i2 = (va >> 21) & 0x1FF, i1 = (va >> 12) & 0x1FF;
    if (!(pml4[i4] & PAGE_PRESENT)) return NULL;
    uint64_t* pdpt = (uint64_t*)(pml4[i4] & COW_PHYS_MASK);
    if (!(pdpt[i3] & PAGE_PRESENT) || (pdpt[i3] & (1ULL << 7))) return NULL;
    uint64_t* pd = (uint64_t*)(pdpt[i3] & COW_PHYS_MASK);
    if (!(pd[i2] & PAGE_PRESENT) || (pd[i2] & (1ULL << 7))) return NULL;
    uint64_t* pt = (uint64_t*)(pd[i2] & COW_PHYS_MASK);
    if (!(pt[i1] & PAGE_PRESENT)) return NULL;
    return &pt[i1];
}

// Resolve a write fault at `fault_addr`. Returns 1 if it was a CoW page and is
// now writable (caller resumes the process), 0 if it was NOT a CoW page (caller
// treats it as a genuine protection violation -> kill). Runs on the faulting
// process's live CR3.
int cow_handle_write(uint64_t fault_addr) {
    if (!cow_enabled()) return 0;
    uint64_t va = fault_addr & ~0xFFFULL;
    uint64_t* pte = cow_walk_pte(va);
    if (!pte) return 0;
    uint64_t e = *pte;
    if (!(e & PTE_COW)) return 0;           // not copy-on-write -> real fault

    uint64_t phys = e & COW_PHYS_MASK;
    uint64_t pfn = phys >> 12;

    // Sole owner? Just re-grant write in place; no copy needed.
    spin_lock(&cow_lock);
    uint32_t extra = (pfn < cow_table_len) ? cow_table[pfn] : 0;
    if (extra == 0) {
        *pte = (e & ~PTE_COW) | PAGE_WRITE;
        spin_unlock(&cow_lock);
        cow_invlpg(va);
        return 1;
    }
    spin_unlock(&cow_lock);

    // Shared: hand this writer a private, writable copy.
    void* nf = pmm_alloc_page();
    if (!nf) return 0;                        // OOM -> let caller kill the process
    memcpy(nf, (void*)phys, PAGE_SIZE);

    uint64_t flags = (e & ~COW_PHYS_MASK & ~PTE_COW) | PAGE_WRITE | PTE_OWNED;
    *pte = ((uint64_t)(uintptr_t)nf & COW_PHYS_MASK) | flags;

    cow_unref(phys);                          // drop this writer's share of the old frame
    cow_invlpg(va);
    return 1;
}
