// VMA management -- singly-linked-list implementation.
// =====================================================
//
// HISTORY / WHY THIS IS A LIST, NOT A TREE:
// An earlier red-black-tree version overloaded proc->vma_list as BOTH the tree
// root AND a "legacy" ->next list head, and maintained the list with
// `z->next = *root;`. Since *root IS proc->vma_list, the first VMA inserted got
// z->next = z -- a SELF-CYCLE. Every consumer that walks proc->vma_list via
// ->next (procapi's vma_count_list / vma_page_count, fork's VMA copy, process
// teardown) then looped forever, hanging the cooperative scheduler mid-boot.
//
// The two structures cannot share the single proc->vma_list field (tree
// rotations move the root without updating the ->next chain). Processes carry
// only a handful of VMAs (one per ELF segment + stack), so an O(n) linked list
// is entirely adequate and is exactly what every existing ->next consumer
// expects. This file therefore maintains a single, correct, NULL-terminated
// singly-linked list. (A proper O(log n) tree could return later as a SEPARATE
// index that does not alias the list head.)

#include "../../include/vma.h"
#include "../../include/sched.h"
#include "../../include/mem.h"
#include "../../include/kernel.h"
#include "../../include/string.h"

// Record a VMA on a process (copies *desc). O(1) prepend. Does not touch page
// tables. The list is NULL-terminated; order is reverse-insertion (irrelevant
// to all consumers, which either search by address or visit every node).
int vma_add(struct process* proc, const vma_t* desc) {
    if (!proc || !desc) {
        return -1;
    }
    vma_t* z = (vma_t*)kmalloc(sizeof(vma_t));
    if (!z) {
        kprintf("[VMA] WARN: kmalloc failed; region 0x%lx not tracked\n",
                (unsigned long)desc->vaddr);
        return -1;
    }
    *z = *desc;
    z->left = NULL;
    z->right = NULL;
    z->parent = NULL;
    z->color = VMA_RB_BLACK;
    z->next = proc->vma_list;   // prepend: new head's next = old head
    proc->vma_list = z;          // new head (proper NULL-terminated list)
    return 0;
}

// Return the VMA containing addr, or NULL. O(n) over the (short) list.
vma_t* vma_find(struct process* proc, uint64_t addr) {
    if (!proc) {
        return NULL;
    }
    for (vma_t* v = proc->vma_list; v != NULL; v = v->next) {
        if (addr >= v->vaddr && addr < v->vaddr + v->length) {
            return v;
        }
    }
    return NULL;
}

// Free every VMA (process teardown). Walks + frees the list, then clears head.
void vma_clear(struct process* proc) {
    if (!proc) {
        return;
    }
    vma_t* v = proc->vma_list;
    while (v != NULL) {
        vma_t* next = v->next;
        kfree(v);
        v = next;
    }
    proc->vma_list = NULL;
}

// EXECVE-INPLACE-0: PCB-free prepend onto a detached list head. Mirrors vma_add()
// exactly (same node init + O(1) prepend) but operates on *head, not a process.
int vma_add_to_list(struct vma** head, const vma_t* desc) {
    if (!head || !desc) {
        return -1;
    }
    vma_t* z = (vma_t*)kmalloc(sizeof(vma_t));
    if (!z) {
        kprintf("[VMA] WARN: kmalloc failed; staged region 0x%lx not tracked\n",
                (unsigned long)desc->vaddr);
        return -1;
    }
    *z = *desc;
    z->left = NULL;
    z->right = NULL;
    z->parent = NULL;
    z->color = VMA_RB_BLACK;
    z->next = *head;   // prepend onto the detached list
    *head = z;
    return 0;
}

// EXECVE-INPLACE-0: free a detached list passed BY VALUE (mirrors vma_clear's
// walk-and-free). Used to discard a STAGED list on a failed exec, and to free the
// OLD list after a successful transplant. Safe on a NULL/empty list. Does NOT
// touch page tables (the caller frees the staged CR3 separately).
void vma_free_list(struct vma* head) {
    vma_t* v = head;
    while (v != NULL) {
        vma_t* next = v->next;
        kfree(v);
        v = next;
    }
}

// Count VMAs (diagnostic).
int vma_count(struct process* proc) {
    if (!proc) {
        return 0;
    }
    int n = 0;
    for (vma_t* v = proc->vma_list; v != NULL; v = v->next) {
        n++;
    }
    return n;
}

// Structural verification (diagnostic). A singly-linked list is always valid;
// also sanity-checks it is finite / not cyclic within a generous bound.
int vma_rb_verify(struct process* proc) {
    if (!proc) {
        return 1;
    }
    int guard = 0;
    for (vma_t* v = proc->vma_list; v != NULL; v = v->next) {
        if (++guard > 100000) {
            return 0;   // cycle / corruption
        }
    }
    return 1;
}
