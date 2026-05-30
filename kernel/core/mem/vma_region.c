// Per-process virtual-memory-area (VMA) RB-tree + recoverable page-fault handler.
//
// This is the authoritative per-process memory map. The ELF loader installs one
// VMA per PT_LOAD segment (file-backed) and one for the stack (anonymous). The
// page-fault handler consults vma_find() to tell a legitimate access apart from
// a real segfault, and (when a page is absent) faults it in from the VMA.
//
// Implementation: VMAs are organized as a red-black tree keyed by vaddr, providing
// O(log n) lookup, insertion, and deletion. This replaces the old fixed 64-page
// limit with dynamic scaling supporting 1000+ VMAs per process.
//
// Policy: the loader currently EAGER-prefaults every page, so handle_page_fault
// is dormant for normal apps. It is the foundation that makes lazy demand
// paging and copy-on-write future *policies* over the same records, not rewrites.

#include "../../include/vma.h"
#include "../../include/sched.h"
#include "../../include/mem.h"
#include "../../include/kernel.h"
#include "../../include/string.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

// Top of the userspace canonical-low half (matches vmm.c / exec.c checks).
#define VMA_USER_SPACE_END 0x0000800000000000ULL

// NOTE: vma_add(), vma_find(), and vma_clear() are now implemented in vma_rbtree.c
// This file contains only the page-fault handler which uses the RB-tree lookup.

int handle_page_fault(uint64_t fault_addr, uint64_t err_code) {
    process_t* cur = process_get_current();
    if (!cur) {
        return 0;
    }
    // Kernel-range fault is never a user demand page -> let the caller handle it.
    if (fault_addr >= VMA_USER_SPACE_END) {
        return 0;
    }
    // Page-fault error-code bits (x86_64):
    //   bit 0 (P) : 1 = fault on a PRESENT page (a permission/protection
    //               violation), 0 = page not present (a demand-paging fault).
    //   bit 1 (W) : 1 = the access was a WRITE.
    //   bit 4 (I/D): 1 = the access was an instruction FETCH (only meaningful
    //               with EFER.NXE enabled) -> an NX (execute) violation.
    int is_present = (err_code & 0x1) != 0;
    int is_write   = (err_code & 0x2) != 0;
    int is_exec    = (err_code & 0x10) != 0;

    // W^X / permission violations are NOT recoverable here. If the page is
    // already PRESENT, this fault is a protection violation (write to RO code,
    // or execute of NX data), not a missing-page demand fault. Refuse to
    // "resolve" it: returning 0 falls through to the kill path in
    // exception_handler(), terminating the offending user process. Re-mapping
    // the page would either leak a frame or silently grant the very permission
    // W^X is meant to deny.
    if (is_present) {
        // Copy-on-write: a WRITE to a present page that carries PTE_COW is a
        // fork-shared page, not a real violation. cow_handle_write() gives the
        // writer a private copy (or re-grants write if it is the sole owner)
        // and we resume. Any other present-page fault (write to RO code, NX
        // exec, write to a non-CoW RO page) returns 0 and falls through to the
        // kill path, preserving W^X.
        if (is_write && cow_handle_write(fault_addr)) {
            return 1;
        }
        return 0;   // protection violation on a mapped page -> kill
    }

    vma_t* v = vma_find(cur, fault_addr);
    if (!v) {
        return 0;   // no mapping covers this address -> genuine segfault
    }
    if (is_write && !(v->perm & VMA_W)) {
        return 0;   // write to a read-only region -> protection violation
    }
    if (is_exec && !(v->perm & VMA_X)) {
        return 0;   // instruction fetch from a non-executable region -> NX fault
    }

    void* frame = pmm_alloc_page();
    if (!frame) {
        return 0;   // out of memory -> let the caller kill the process
    }

    uint64_t page_base  = fault_addr & ~0xFFFULL;
    uint64_t region_off = page_base - v->vaddr;

    // Populate the frame via its identity-mapped physical address.
    if (v->backing == VMA_FILE && v->file_ptr) {
        uint64_t avail    = (region_off < v->file_sz) ? (v->file_sz - region_off) : 0;
        uint64_t copy_len = (avail < PAGE_SIZE) ? avail : PAGE_SIZE;
        if (copy_len) {
            memcpy(frame, (const uint8_t*)v->file_ptr + v->file_off + region_off, copy_len);
        }
        if (copy_len < PAGE_SIZE) {
            memset((uint8_t*)frame + copy_len, 0, PAGE_SIZE - copy_len);
        }
    } else {
        memset(frame, 0, PAGE_SIZE);   // anonymous demand-zero
    }

    // Mirror exec.c's W^X policy when faulting a page in on demand so the
    // resolved mapping keeps the same protection as the eagerly-mapped pages:
    //   writable VMA   -> PAGE_WRITE
    //   non-exec VMA   -> PAGE_NX  (e.g. the lazy anonymous stack)
    uint64_t flags = PAGE_PRESENT | PAGE_USER;
    if (v->perm & VMA_W) {
        flags |= PAGE_WRITE;
    }
    if (!(v->perm & VMA_X)) {
        flags |= PAGE_NX;
    }

    // Map into the faulting process's address space (its CR3 is live on the CPU).
    paging_set_target(cur->context.cr3);
    vmm_map_page((void*)page_base, frame, flags);   // issues invlpg internally
    paging_reset_target();

    return 1;
}
