#ifndef VMA_H
#define VMA_H

#include "types.h"

// Permission bits for a VMA.
#define VMA_R 0x1u
#define VMA_W 0x2u
#define VMA_X 0x4u

// VMA flags.
#define VMA_FLAG_COW       0x1u
#define VMA_FLAG_GROWSDOWN 0x2u
#define VMA_FLAG_GUARD     0x4u  // Guard page: never faulted in, triggers kill on access

typedef enum { VMA_ANON = 0, VMA_FILE = 1 } vma_backing_t;

// Red-black tree colors
#define VMA_RB_RED   0
#define VMA_RB_BLACK 1

// A virtual memory area: one contiguous, page-aligned region of a process's
// address space with uniform permissions and a single backing source. This is
// the per-process source of truth that the page-fault handler consults; the
// loader installs one per ELF segment (file-backed) plus the stack (anon).
//
// Now organized as a red-black tree for O(log n) lookup, insert, and delete
// operations, replacing the fixed 64-page array limit with dynamic scaling.
typedef struct vma {
    // VMA properties
    uint64_t      vaddr;     // page-aligned start (also the RB-tree key)
    uint64_t      length;    // page-aligned byte length
    uint32_t      perm;      // VMA_R | VMA_W | VMA_X
    uint32_t      flags;     // VMA_FLAG_*
    vma_backing_t backing;   // ANON (zero-fill) or FILE (initrd-backed)
    const void*   file_ptr;  // FILE: base pointer into the resident initrd image
    uint64_t      file_off;  // FILE: byte offset within file_ptr that maps to vaddr
    uint64_t      file_sz;   // FILE: real bytes from vaddr (rest of length is zero)

    // Red-black tree pointers
    struct vma*   left;      // left child (lower addresses)
    struct vma*   right;     // right child (higher addresses)
    struct vma*   parent;    // parent node
    int           color;     // VMA_RB_RED or VMA_RB_BLACK

    // Legacy linked-list pointer (maintained for compatibility)
    struct vma*   next;
} vma_t;

struct process;

// Record a VMA on a process (copies *desc). Does not touch page tables.
// Time complexity: O(log n) with RB-tree implementation
void   vma_add(struct process* proc, const vma_t* desc);

// Return the VMA containing addr, or NULL.
// Time complexity: O(log n) with RB-tree implementation
vma_t* vma_find(struct process* proc, uint64_t addr);

// Free the whole VMA tree (call at process teardown).
// Time complexity: O(n)
void   vma_clear(struct process* proc);

// Count the number of VMAs in the tree (diagnostic)
int    vma_count(struct process* proc);

// Verify RB-tree properties (diagnostic, returns 1 if valid)
int    vma_rb_verify(struct process* proc);

// Recoverable page-fault entry point. Returns 1 if the fault was a legitimate
// access to a known VMA and the page was faulted in (the instruction should be
// retried), or 0 if the fault is unresolved (caller should kill/panic).
//
// NOTE: with the default EAGER load policy every page is pre-mapped, so this is
// effectively dormant for normal apps -- it is the foundation for lazy/CoW
// paging, which become policies over the same VMA records.
int    handle_page_fault(uint64_t fault_addr, uint64_t err_code);

#endif /* VMA_H */
