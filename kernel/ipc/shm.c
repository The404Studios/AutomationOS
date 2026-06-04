#include "../include/ipc.h"
#include "../include/kernel.h"
#include "../include/mem.h"
#include "../include/sched.h"
#include "../include/spinlock.h"
#include "../include/syscall.h"
#include "../include/string.h"
#include "../include/drivers.h"   // timer_get_ticks()

/*
 * Shared Memory Implementation
 * ============================
 *
 * Provides System V-style shared memory for zero-copy data exchange
 * between processes. Critical for compositor↔application communication.
 *
 * Design:
 * - Key-based identification (processes agree on keys)
 * - Physical pages shared across multiple virtual address spaces
 * - Reference counting prevents premature deallocation
 * - Permission checking based on UID/GID
 * - Thread-safe with spinlock protection
 *
 * Performance (post-optimisation):
 * ─────────────────────────────────
 *  shm_find_by_id:  O(1) – direct array index on (id - 1)
 *  shm_find_by_key: O(1) avg – open-addressing hash table (power-of-2)
 *  shmdt reverse-lookup: O(1) – derive id from VA formula instead of
 *    walking the segment list
 *  shmctl IPC_RMID removal: O(1) – clear slot + tombstone hash entry
 *
 * Before: all lookups were O(n) linked-list walks on the global segment list.
 *
 * shmat behaviour preserved: maps into the CALLING process's CR3
 * (current->context.cr3), not the active kernel PML4.
 */

// ─── Tuning constants ──────────────────────────────────────────────────────

// Maximum number of shared memory segments.  Must fit in id_table below.
// Use a generous limit; each slot is only a pointer (8 bytes).
#define SHM_TABLE_SIZE  1024

// Key hash table: power-of-2, load factor ≤ 0.5 → double the capacity.
#define SHM_HASH_SIZE   2048
#define SHM_HASH_MASK   (SHM_HASH_SIZE - 1)

// Sentinel values for open-addressing slots
#define SHM_HASH_EMPTY  ((shm_segment_t*)0)
#define SHM_HASH_DEAD   ((shm_segment_t*)1)

// Virtual address window used by shmat when addr == NULL.
// shmat assigns:  virt = SHM_VA_BASE + (id * SHM_VA_STRIDE)
// shmdt inverts:  id   = (virt - SHM_VA_BASE) / SHM_VA_STRIDE
// These constants match the formula in the original shmat implementation.
#define SHM_VA_BASE     0x60000000ULL
#define SHM_VA_STRIDE   (16ULL * 1024 * 1024)  // 16 MB per slot

// ─── Global state ─────────────────────────────────────────────────────────

// Flat array: id_table[id-1] == pointer to segment, or NULL if unused.
static shm_segment_t* id_table[SHM_TABLE_SIZE];

// Open-addressing hash table keyed on segment->key.
static shm_segment_t* key_table[SHM_HASH_SIZE];

static uint32_t next_shm_id = 1;   // Next segment ID to allocate
static spinlock_t shm_lock;        // Protects both tables

// ─── Per-process attachment tracking ───────────────────────────────────────
// Each process_t has a singly-linked list of shm_attachment_t nodes tracking
// its active SHM attachments. On shmat() we prepend a node; on shmdt() we
// remove it; on process death shm_cleanup_process() walks the list to decrement
// attach_count for each segment. This makes cleanup O(n) where n is the number
// of attachments THIS process holds (typically 1-5) instead of O(128).

// Add an attachment to the process's list. Caller must hold shm_lock.
static void shm_attach_add(process_t* proc, ipc_id_t shm_id, void* virt_addr, size_t size, uint32_t flags) {
    shm_attachment_t* att = (shm_attachment_t*)kmalloc(sizeof(shm_attachment_t));
    if (!att) {
        kprintf("[SHM] WARNING: failed to allocate attachment record for PID %u\n", proc->pid);
        return;
    }
    att->shm_id = shm_id;
    att->virt_addr = virt_addr;
    att->size = size;
    att->flags = flags;
    att->next = proc->shm_attachments;
    proc->shm_attachments = att;
}

// Remove an attachment from the process's list. Caller must hold shm_lock.
// Returns true if found and removed, false otherwise.
static bool shm_attach_remove(process_t* proc, ipc_id_t shm_id) {
    shm_attachment_t** pp = &proc->shm_attachments;
    while (*pp) {
        if ((*pp)->shm_id == shm_id) {
            shm_attachment_t* victim = *pp;
            *pp = victim->next;
            kfree(victim);
            return true;
        }
        pp = &(*pp)->next;
    }
    return false;
}

// Free all attachments in a process's list. Called by shm_cleanup_process.
// Caller must hold shm_lock.
static void shm_attach_free_all(process_t* proc) {
    shm_attachment_t* att = proc->shm_attachments;
    while (att) {
        shm_attachment_t* next = att->next;
        kfree(att);
        att = next;
    }
    proc->shm_attachments = NULL;
}

// ─── Internal helpers ─────────────────────────────────────────────────────

static inline uint32_t shm_key_hash(key_t key) {
    uint32_t k = (uint32_t)key;
    k = (k ^ (k >> 16)) * 0x45d9f3b;
    k = (k ^ (k >> 16)) * 0x45d9f3b;
    k ^= (k >> 16);
    return k & SHM_HASH_MASK;
}

static void key_table_insert(shm_segment_t* seg) {
    if (seg->key == IPC_PRIVATE) {
        return;
    }
    uint32_t slot = shm_key_hash(seg->key);
    for (uint32_t i = 0; i < SHM_HASH_SIZE; i++) {
        shm_segment_t* e = key_table[(slot + i) & SHM_HASH_MASK];
        if (e == SHM_HASH_EMPTY || e == SHM_HASH_DEAD) {
            key_table[(slot + i) & SHM_HASH_MASK] = seg;
            return;
        }
    }
}

static void key_table_remove(shm_segment_t* seg) {
    if (seg->key == IPC_PRIVATE) {
        return;
    }
    uint32_t slot = shm_key_hash(seg->key);
    for (uint32_t i = 0; i < SHM_HASH_SIZE; i++) {
        uint32_t idx = (slot + i) & SHM_HASH_MASK;
        shm_segment_t* e = key_table[idx];
        if (e == SHM_HASH_EMPTY) {
            break;
        }
        if (e == seg) {
            key_table[idx] = SHM_HASH_DEAD;
            return;
        }
    }
}

// ─── Public lookup API ────────────────────────────────────────────────────

// Find segment by key — O(1) average via hash table.
shm_segment_t* shm_find_by_key(key_t key) {
    if (key == IPC_PRIVATE) {
        return NULL;
    }
    uint32_t slot = shm_key_hash(key);
    for (uint32_t i = 0; i < SHM_HASH_SIZE; i++) {
        uint32_t idx = (slot + i) & SHM_HASH_MASK;
        shm_segment_t* e = key_table[idx];
        if (e == SHM_HASH_EMPTY) {
            return NULL;
        }
        if (e != SHM_HASH_DEAD && e->key == key) {
            return e;
        }
    }
    return NULL;
}

// Find segment by ID — O(1) direct array index.
shm_segment_t* shm_find_by_id(ipc_id_t id) {
    if (id <= 0 || (uint32_t)id > SHM_TABLE_SIZE) {
        return NULL;
    }
    return id_table[id - 1];
}

// ─── Subsystem initialisation ─────────────────────────────────────────────

void shm_init(void) {
    kprintf("[SHM] Initializing shared memory subsystem\n");
    for (uint32_t i = 0; i < SHM_TABLE_SIZE; i++) {
        id_table[i] = NULL;
    }
    for (uint32_t i = 0; i < SHM_HASH_SIZE; i++) {
        key_table[i] = SHM_HASH_EMPTY;
    }
    next_shm_id = 1;
    spin_lock_init(&shm_lock);
    kprintf("[SHM] Shared memory initialized\n");
}

// ─── Permission helper ────────────────────────────────────────────────────

static bool shm_check_permission(shm_segment_t* seg, uint32_t uid, uint32_t gid, bool write) {
    if (uid == 0) {
        return true;
    }
    if (uid == seg->creator_uid) {
        return true;
    }
    if (gid == seg->creator_gid) {
        /* GROUP bits are mode>>3 (0020/0040); using the owner bits IPC_W/R here
         * would grant a group member the OWNER's rights (mode 0640 => group can
         * WRITE). The 'other' branch below correctly uses >>6. */
        return write ? (seg->mode & (IPC_W >> 3)) != 0 : (seg->mode & (IPC_R >> 3)) != 0;
    }
    return write ? (seg->mode & (IPC_W >> 6)) != 0 : (seg->mode & (IPC_R >> 6)) != 0;
}

// ─── SYS_SHMGET ───────────────────────────────────────────────────────────

/*
 * SYS_SHMGET - Create or get shared memory segment
 *
 * Arguments:
 *   key    - IPC key (IPC_PRIVATE for anonymous)
 *   size   - Segment size in bytes
 *   shmflg - Flags (IPC_CREAT, IPC_EXCL, mode bits)
 *
 * Returns: Segment ID on success, negative error code on failure
 *
 * Lock-order discipline
 * ---------------------
 * shm_lock must NEVER be held while calling into the heap (kmalloc/kfree)
 * or the PMM (pmm_alloc_page/pmm_free_page).  Those subsystems carry their
 * own internal locks; acquiring them while shm_lock is already held would
 * invert the lock order and deadlock once a page-fault or SMP path exists.
 *
 * Two-phase create protocol:
 *   Phase 1 (under lock)  – key/exist check; reserve an ID slot by noting
 *                           which id is free (but do NOT write to id_table yet).
 *   Phase 2 (lock dropped) – kmalloc the struct, kmalloc the phys_pages
 *                            array, pmm_alloc_page() each page (with rollback
 *                            on partial failure), initialise the struct.
 *   Phase 3 (under lock)  – re-validate: confirm the reserved slot is still
 *                            NULL and (for keyed segments) the key still has
 *                            no entry; if either check fails, free everything
 *                            and return the appropriate error.  Otherwise
 *                            publish to id_table + key_table and advance
 *                            next_shm_id.
 */
int64_t sys_shmget(uint64_t key, uint64_t size, uint64_t shmflg,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg4; (void)arg5; (void)arg6;
    key_t k = (key_t)key;
    size_t sz = (size_t)size;
    int flags = (int)shmflg;
    process_t* current = process_get_current();
    if (!current) {
        return IPC_EINVAL;
    }

    kprintf("[SHMGET] key=%d size=%lu flags=0x%x (PID %d)\n",
            k, sz, flags, current->pid);

    if (sz == 0 || sz > 256 * 1024 * 1024) {
        kprintf("[SHMGET] Invalid size: %lu\n", sz);
        return IPC_EINVAL;
    }

    sz = ALIGN_UP(sz, PAGE_SIZE);
    uint32_t pages = sz / PAGE_SIZE;

    /* ── Phase 1: key/exist check + ID reservation (under lock) ─────────── */

    spin_lock(&shm_lock);

    // O(1) key lookup — must happen under the lock so we see a consistent view
    // of key_table against concurrent shmget/shmctl callers.
    if (k != IPC_PRIVATE) {
        shm_segment_t* existing = shm_find_by_key(k);
        if (existing) {
            if (flags & IPC_EXCL) {
                spin_unlock(&shm_lock);
                kprintf("[SHMGET] Segment exists and IPC_EXCL specified\n");
                return IPC_EEXIST;
            }
            spin_unlock(&shm_lock);
            kprintf("[SHMGET] Returning existing segment %d\n", existing->id);
            return existing->id;
        }

        if (!(flags & IPC_CREAT)) {
            spin_unlock(&shm_lock);
            kprintf("[SHMGET] Segment not found and IPC_CREAT not specified\n");
            return IPC_ENOENT;
        }
    }

    // Find a free ID slot.  We only *read* next_shm_id here; the slot is NOT
    // written until Phase 3 so we hold shm_lock for the minimum time.
    if (next_shm_id > SHM_TABLE_SIZE) {
        next_shm_id = 1;
    }
    uint32_t start = next_shm_id;
    while (id_table[next_shm_id - 1] != NULL) {
        next_shm_id++;
        if (next_shm_id > SHM_TABLE_SIZE) {
            next_shm_id = 1;
        }
        if (next_shm_id == start) {
            spin_unlock(&shm_lock);
            kprintf("[SHMGET] No free segment slots\n");
            return IPC_ENOMEM;
        }
    }
    uint32_t reserved_id = next_shm_id;   // remember which slot we found

    spin_unlock(&shm_lock);

    /* ── Phase 2: heap + PMM work outside the lock ───────────────────────── */

    // Allocate segment structure (heap lock taken inside kmalloc, not shm_lock).
    shm_segment_t* seg = (shm_segment_t*)kmalloc(sizeof(shm_segment_t));
    if (!seg) {
        kprintf("[SHMGET] Failed to allocate segment structure\n");
        return IPC_ENOMEM;
    }

    // Allocate per-page physical address array.
    void** phys_pages = (void**)kmalloc(pages * sizeof(void*));
    if (!phys_pages) {
        kfree(seg);
        kprintf("[SHMGET] Failed to allocate phys page array\n");
        return IPC_ENOMEM;
    }

    // Allocate physical pages one by one; roll back on partial failure.
    uint32_t allocated = 0;
    for (uint32_t i = 0; i < pages; i++) {
        phys_pages[i] = pmm_alloc_page();
        if (!phys_pages[i]) {
            for (uint32_t j = 0; j < allocated; j++) {
                pmm_free_page(phys_pages[j]);
            }
            kfree(phys_pages);
            kfree(seg);
            kprintf("[SHMGET] Failed to allocate physical memory (page %u/%u)\n",
                    i, pages);
            return IPC_ENOMEM;
        }
        allocated++;
    }

    // Fully initialise the struct while unlocked — safe because seg is not
    // visible to any other thread yet.
    memset(seg, 0, sizeof(shm_segment_t));
    seg->id           = (ipc_id_t)reserved_id;
    seg->key          = k;
    seg->phys_addr    = (void*)phys_pages;
    seg->size         = sz;
    seg->pages        = pages;
    seg->attach_count = 0;
    seg->creator_uid  = current->uid;
    seg->creator_gid  = current->gid;
    seg->owner_pid    = current->pid;
    seg->mode         = flags & 0777;
    seg->pending_destroy = 0;
    seg->create_time  = timer_get_ticks();
    seg->attach_time  = 0;
    seg->detach_time  = 0;
    seg->next         = NULL;  // Not used for list traversal; kept for struct compat

    /* ── Phase 3: commit under lock (re-validate before publishing) ──────── */

    spin_lock(&shm_lock);

    // Re-check: the slot we reserved might have been taken by a concurrent
    // shmget (possible if another CPU ran between Phase 1 and Phase 3).
    if (id_table[reserved_id - 1] != NULL) {
        // Slot raced away — give up cleanly.
        spin_unlock(&shm_lock);
        for (uint32_t i = 0; i < pages; i++) {
            pmm_free_page(phys_pages[i]);
        }
        kfree(phys_pages);
        kfree(seg);
        kprintf("[SHMGET] ID slot %u raced; no free segment slots\n", reserved_id);
        return IPC_ENOMEM;
    }

    // Re-check: for keyed segments, someone else might have created the same
    // key between Phase 1 and Phase 3.
    if (k != IPC_PRIVATE) {
        shm_segment_t* racing = shm_find_by_key(k);
        if (racing) {
            spin_unlock(&shm_lock);
            for (uint32_t i = 0; i < pages; i++) {
                pmm_free_page(phys_pages[i]);
            }
            kfree(phys_pages);
            kfree(seg);
            if (flags & IPC_EXCL) {
                kprintf("[SHMGET] Key raced in; IPC_EXCL → EEXIST\n");
                return IPC_EEXIST;
            }
            kprintf("[SHMGET] Key raced in; returning existing segment %d\n",
                    racing->id);
            return racing->id;
        }
    }

    // Safe to publish — advance the global counter and insert into both tables.
    id_table[seg->id - 1] = seg;
    key_table_insert(seg);

    next_shm_id = reserved_id + 1;
    if (next_shm_id > SHM_TABLE_SIZE) {
        next_shm_id = 1;
    }

    spin_unlock(&shm_lock);

    kprintf("[SHMGET] Created segment %d: key=%d size=%lu pages=%u phys[0]=%p\n",
            seg->id, k, sz, pages, ((void**)seg->phys_addr)[0]);

    return seg->id;
}

// ─── SYS_SHMAT ────────────────────────────────────────────────────────────

/*
 * SYS_SHMAT - Attach shared memory segment to process address space
 *
 * Arguments:
 *   shmid   - Segment ID (from shmget)
 *   shmaddr - Desired virtual address (NULL = kernel chooses)
 *   shmflg  - Flags (SHM_RDONLY, SHM_RND)
 *
 * Returns: Virtual address on success, negative error code on failure
 *
 * M1 behaviour preserved: maps into current->context.cr3 (the CALLING
 * process), not the active kernel PML4.
 */
int64_t sys_shmat(uint64_t shmid, uint64_t shmaddr, uint64_t shmflg,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg4; (void)arg5; (void)arg6;
    int id = (int)shmid;
    const void* addr = (const void*)shmaddr;
    int flags = (int)shmflg;
    process_t* current = process_get_current();
    if (!current) {
        return IPC_EINVAL;
    }

    kprintf("[SHMAT] shmid=%d addr=%p flags=0x%x (PID %d)\n",
            id, addr, flags, current->pid);

    spin_lock(&shm_lock);

    // O(1) segment lookup
    shm_segment_t* seg = shm_find_by_id(id);
    if (!seg) {
        spin_unlock(&shm_lock);
        kprintf("[SHMAT] Segment %d not found\n", id);
        return IPC_EINVAL;
    }

    bool write = !(flags & SHM_RDONLY);
    if (!shm_check_permission(seg, current->uid, current->gid, write)) {
        spin_unlock(&shm_lock);
        kprintf("[SHMAT] Permission denied\n");
        return IPC_EACCES;
    }

    // Choose virtual address
    uint64_t virt_addr;
    if (addr == NULL) {
        // Deterministic VA derived from segment ID — same formula as before.
        virt_addr = SHM_VA_BASE + ((uint64_t)id * SHM_VA_STRIDE);
    } else {
        virt_addr = (uint64_t)addr;
        if (flags & SHM_RND) {
            virt_addr = ALIGN_DOWN(virt_addr, SHMLBA);
        }
        // Reject if the base OR the end of the mapping leaves the user half.
        // (Overflow-safe: end < base catches wraparound.) The old code only
        // checked the base, so a high shmaddr could map pages into the
        // non-canonical gap above USER_SPACE_END.
        uint64_t end = virt_addr + (uint64_t)seg->size;
        if (virt_addr >= USER_SPACE_END || end > USER_SPACE_END || end < virt_addr) {
            spin_unlock(&shm_lock);
            kprintf("[SHMAT] Invalid address range: %p +%lu\n",
                    (void*)virt_addr, (unsigned long)seg->size);
            return IPC_EINVAL;
        }
    }

    // Map into CALLING process's address space (current->context.cr3).
    uint32_t page_flags = PAGE_PRESENT | PAGE_USER;
    if (write) {
        page_flags |= PAGE_WRITE;
    }

    uint64_t cr3 = current->context.cr3;
    void** phys_pages = (void**)seg->phys_addr;

    for (uint32_t i = 0; i < seg->pages; i++) {
        if (vmm_map_phys_into(cr3, virt_addr + i * PAGE_SIZE,
                              (uint64_t)phys_pages[i], PAGE_SIZE, page_flags) != 0) {
            // Roll back the pages mapped so far (shared pages → free_owned=false)
            // and fail WITHOUT bumping attach_count, so a partial map can't leave
            // an inflated count that blocks the segment from ever being freed.
            if (i > 0) {
                vmm_unmap_range_into(cr3, virt_addr, (uint64_t)i * PAGE_SIZE, false);
            }
            spin_unlock(&shm_lock);
            kprintf("[SHMAT] map failed at page %u; rolled back\n", i);
            return IPC_ENOMEM;
        }
    }

    seg->attach_count++;
    seg->attach_time = timer_get_ticks();
    shm_attach_add(current, seg->id, (void*)virt_addr, seg->size, flags);

    spin_unlock(&shm_lock);

    kprintf("[SHMAT] Attached segment %d at %p (phys=%p, %u pages)\n",
            id, (void*)virt_addr, seg->phys_addr, seg->pages);

    return (int64_t)virt_addr;
}

// ─── SYS_SHMDT ────────────────────────────────────────────────────────────

/*
 * SYS_SHMDT - Detach shared memory segment from process address space
 *
 * Arguments:
 *   shmaddr - Virtual address of attached segment
 *
 * Returns: 0 on success, negative error code on failure
 *
 * Optimisation: when the address falls within the kernel-chosen window
 * (SHM_VA_BASE … SHM_VA_BASE + SHM_TABLE_SIZE*SHM_VA_STRIDE), we derive
 * the segment ID directly from the address — O(1) without scanning any list.
 * For caller-supplied addresses we fall back to the id_table scan (O(n) but
 * only over present segments, not the whole list).
 */
int64_t sys_shmdt(uint64_t shmaddr, uint64_t arg2, uint64_t arg3,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    const void* addr = (const void*)shmaddr;
    process_t* current = process_get_current();
    if (!current) {
        return IPC_EINVAL;
    }

    if (!addr) {
        return IPC_EINVAL;
    }

    kprintf("[SHMDT] addr=%p (PID %d)\n", addr, current->pid);

    uint64_t virt_addr = (uint64_t)addr;

    spin_lock(&shm_lock);

    shm_segment_t* seg = NULL;

    // Fast path: address is in the kernel-managed VA window.
    // Derive id = (va - base) / stride.  Validate before using.
    if (virt_addr >= SHM_VA_BASE) {
        uint64_t offset = virt_addr - SHM_VA_BASE;
        uint64_t id_guess = offset / SHM_VA_STRIDE;
        if (id_guess >= 1 && id_guess <= SHM_TABLE_SIZE) {
            shm_segment_t* candidate = id_table[id_guess - 1];
            if (candidate != NULL) {
                uint64_t expected_base = SHM_VA_BASE + id_guess * SHM_VA_STRIDE;
                uint64_t expected_end  = expected_base + candidate->size;
                if (virt_addr >= expected_base && virt_addr < expected_end) {
                    seg = candidate;
                }
            }
        }
    }

    // Slow path: caller supplied an explicit non-window address.
    // Scan only the occupied id_table slots (still O(SHM_TABLE_SIZE) worst
    // case, but avoids the pointer-chasing of a linked list and is cache-hot).
    if (!seg) {
        for (uint32_t i = 0; i < SHM_TABLE_SIZE; i++) {
            shm_segment_t* s = id_table[i];
            if (!s) {
                continue;
            }
            uint64_t base = SHM_VA_BASE + ((uint64_t)s->id * SHM_VA_STRIDE);
            if (virt_addr >= base && virt_addr < base + s->size) {
                seg = s;
                break;
            }
        }
    }

    if (!seg) {
        spin_unlock(&shm_lock);
        kprintf("[SHMDT] Address %p not attached\n", addr);
        return IPC_EINVAL;
    }

    // Unmap from calling process (free_owned=false: physical pages belong to
    // the segment, released only when the last reference is dropped).
    vmm_unmap_range_into(current->context.cr3, virt_addr, seg->size, false);

    if (seg->attach_count > 0) {
        seg->attach_count--;
    }
    shm_attach_remove(current, seg->id);   // drop the tracking record
    seg->detach_time = timer_get_ticks();

    ipc_id_t seg_id = seg->id;   // save before potential free

    /*
     * Deferred-destroy check: if IPC_RMID was called while this segment had
     * attachments, the last shmdt() (attach_count just hit 0) is responsible
     * for releasing the physical pages and the segment struct.  id_table was
     * kept pointing at seg so we could reach it here; clear it now.
     *
     * Collect the to-free pointers under the lock, then do the actual
     * kfree / pmm_free_page calls AFTER spin_unlock.  Calling the heap or
     * PMM while shm_lock is held would invert the lock order.
     */
    void** deferred_pages = NULL;
    uint32_t deferred_npages = 0;
    shm_segment_t* deferred_seg = NULL;

    if (seg->pending_destroy && seg->attach_count == 0) {
        id_table[seg->id - 1] = NULL;   // now safe to remove from id_table

        /* Snapshot the to-free data while still under shm_lock. */
        deferred_pages  = (void**)seg->phys_addr;
        deferred_npages = seg->pages;
        deferred_seg    = seg;
        /* seg is invalid after spin_unlock — do not touch it after this. */
    }

    spin_unlock(&shm_lock);

    /* Heap/PMM frees outside the lock. */
    if (deferred_seg) {
        if (deferred_pages) {
            for (uint32_t i = 0; i < deferred_npages; i++) {
                if (deferred_pages[i]) {
                    pmm_free_page(deferred_pages[i]);
                }
            }
            kfree(deferred_pages);
        }
        kprintf("[SHMDT] Deferred free: segment %d fully released\n", seg_id);
        kfree(deferred_seg);
    }

    kprintf("[SHMDT] Detached segment %d from %p\n", seg_id, (void*)shmaddr);

    return IPC_SUCCESS;
}

// ─── SYS_SHMCTL ───────────────────────────────────────────────────────────

/*
 * SYS_SHMCTL - Shared memory control operations
 *
 * Arguments:
 *   shmid - Segment ID
 *   cmd   - Control command (IPC_RMID, IPC_STAT, IPC_SET)
 *   buf   - Buffer for command-specific data
 *
 * Returns: 0 on success, negative error code on failure
 *
 * IPC_RMID: O(1) removal — clear id_table slot + tombstone key_table.
 */
int64_t sys_shmctl(uint64_t shmid, uint64_t cmd, uint64_t buf,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg4; (void)arg5; (void)arg6;
    int id = (int)shmid;
    int command = (int)cmd;
    void* buffer = (void*)buf;
    (void)buffer;
    process_t* current = process_get_current();
    if (!current) {
        return IPC_EINVAL;
    }

    kprintf("[SHMCTL] shmid=%d cmd=0x%x (PID %d)\n", id, command, current->pid);

    spin_lock(&shm_lock);

    // O(1) segment lookup
    shm_segment_t* seg = shm_find_by_id(id);
    if (!seg) {
        spin_unlock(&shm_lock);
        kprintf("[SHMCTL] Segment %d not found\n", id);
        return IPC_EINVAL;
    }

    switch (command) {
        case IPC_RMID:
            if (current->uid != 0 && current->uid != seg->creator_uid) {
                spin_unlock(&shm_lock);
                return IPC_EACCES;
            }

            if (seg->attach_count == 0) {
                /*
                 * No attachments — remove from both tables under the lock,
                 * then collect the pointers to free.  The actual kfree /
                 * pmm_free_page calls happen AFTER spin_unlock to avoid
                 * holding shm_lock while the heap or PMM take their own
                 * internal locks (lock-order inversion).
                 */
                id_table[seg->id - 1] = NULL;
                key_table_remove(seg);

                /* Snapshot pointers while still under lock. */
                void** to_free_pages = (void**)seg->phys_addr;
                uint32_t npages      = seg->pages;
                shm_segment_t* to_free_seg = seg;

                spin_unlock(&shm_lock);

                /* Heap/PMM work outside the lock. */
                if (to_free_pages) {
                    for (uint32_t i = 0; i < npages; i++) {
                        if (to_free_pages[i]) {
                            pmm_free_page(to_free_pages[i]);
                        }
                    }
                    kfree(to_free_pages);
                }
                kprintf("[SHMCTL] IPC_RMID: freed segment %d (%u pages)\n",
                        id, npages);
                kfree(to_free_seg);
            } else {
                /*
                 * Deferred destroy: one or more processes still have this
                 * segment mapped.  Mark it pending so the last shmdt() frees
                 * the resources.  Remove from key_table so no new shmget()
                 * can attach to it, but KEEP id_table[id-1] pointing to seg
                 * so that in-flight shmdt() calls can still locate it via the
                 * virtual-address reverse-lookup and decrement attach_count.
                 * No heap/PMM calls needed here — just flag manipulation.
                 */
                key_table_remove(seg);
                seg->pending_destroy = 1;
                kprintf("[SHMCTL] IPC_RMID: segment %d pending destroy "
                        "(%u attachments remain)\n",
                        id, seg->attach_count);
                spin_unlock(&shm_lock);
            }
            return IPC_SUCCESS;

        case IPC_STAT:
            spin_unlock(&shm_lock);
            return IPC_SUCCESS;

        case IPC_SET:
            spin_unlock(&shm_lock);
            return IPC_SUCCESS;

        default:
            spin_unlock(&shm_lock);
            return IPC_EINVAL;
    }
}

// ─── Process cleanup ──────────────────────────────────────────────────────

/*
 * shm_cleanup_process - release all SHM resources held by a dying process.
 *
 * Called from process_unref() (in process.c) when the last reference to a
 * process drops to zero, i.e. in the "ref_count == 0" branch, just before
 * paging_destroy_address_space().
 *
 * Two jobs:
 *  1. For every segment the process *owns* (owner_pid == pid): mark it for
 *     destruction via the same deferred-free path used by IPC_RMID.  If no
 *     one is currently attached the segment is freed immediately; otherwise
 *     pending_destroy is set and the last shmdt() will clean up.
 *
 *  2. For every *other* segment that still has the process's virtual mappings
 *     live (attach_count > 0 and we'd normally expect a shmdt): we cannot
 *     unmap them here because paging_destroy_address_space() will tear down
 *     the entire CR3 momentarily, so the mappings are already gone.  All we
 *     need to do is decrement attach_count so the reference counts stay
 *     accurate.  If that makes a pending-destroy segment reach zero we free
 *     it here as well.
 *
 *     NOTE: Without a per-process attachment list we cannot cheaply enumerate
 *     only *this* process's attachments.  The conservative safe action is to
 *     scan the whole id_table (O(SHM_TABLE_SIZE) = O(1024)) which is cheap
 *     at teardown time and avoids both leaks and UAF.  When a per-process
 *     shm_attachment list is added in the future this scan can be replaced.
 */
/*
 * Deferred-free work item: one entry for each segment that must be freed
 * after we drop shm_lock in shm_cleanup_process.
 *
 * SHM_TABLE_SIZE entries × ~20 bytes each = ~20 KB, which is too large to
 * put on the kernel stack.  We use a file-static array instead.
 * shm_cleanup_process is called from process teardown which is inherently
 * single-threaded per process and is serialised through shm_lock, so a
 * static buffer is safe here.
 */
typedef struct {
    void**          phys_pages;
    uint32_t        npages;
    shm_segment_t*  seg;
} shm_deferred_free_t;

/* A process owns at most a handful of segments, so a small fixed work buffer is
 * plenty — sizing this at SHM_TABLE_SIZE (1024) put ~24KB of static into .bss,
 * which (with the IST stack) pushed .bss past where GRUB places the initrd and
 * corrupted it at runtime. Keep it small. */
#define SHM_CLEANUP_MAX 64
static shm_deferred_free_t cleanup_work[SHM_CLEANUP_MAX];

void shm_cleanup_process(process_t* proc) {
    if (!proc) return;
    uint32_t pid = proc->pid;
    kprintf("[SHM] Cleanup for PID %d\n", pid);

    /*
     * We collect work items to free under the lock, then do the actual
     * kfree / pmm_free_page calls after releasing shm_lock, so we never
     * hold shm_lock while the heap or PMM take their own internal locks
     * (lock-order inversion → deadlock).
     *
     * cleanup_work[] is file-static (see above); it is safe to reuse because
     * shm_lock serialises concurrent cleanup calls and the array is fully
     * repopulated on every entry.
     */
    uint32_t nfree = 0;

    // proc is passed in by the caller (process_unref teardown). Do NOT re-look it
    // up via process_get_by_pid(): by the time this runs the PCB has ALREADY been
    // removed from process_table, so the lookup returned NULL and the entire
    // cleanup became a silent no-op — every owned shm segment and attachment node
    // leaked. (And taking a ref during ref_count==0 teardown would recurse on
    // unref.) The caller owns the sole reference; use proc directly.
    spin_lock(&shm_lock);

    // Pass 0: walk the process's attachment list and decrement attach_count for
    // each segment. This is O(n) where n is the number of attachments THIS
    // process holds (typically 1-5) instead of O(128) scanning the global array.
    // Fixes the leak where a non-owner attacher's death left an IPC_RMID'd
    // segment pinned forever.
    shm_attachment_t* att = proc->shm_attachments;
    while (att) {
        shm_segment_t* seg = shm_find_by_id(att->shm_id);
        if (seg && seg->attach_count > 0) {
            seg->attach_count--;
            kprintf("[SHM] Cleanup PID %d: decremented attach_count for segment %d\n",
                    pid, att->shm_id);
        }
        att = att->next;
    }
    // Free the entire attachment list now that we've processed all entries.
    shm_attach_free_all(proc);

    for (uint32_t i = 0; i < SHM_TABLE_SIZE; i++) {
        shm_segment_t* seg = id_table[i];
        if (!seg) {
            continue;
        }

        if (seg->owner_pid == pid) {
            /*
             * This process created the segment.  Treat it as an IPC_RMID:
             * remove it from the key_table so new callers cannot attach, then
             * either queue for immediate free (no attachments) or defer to the
             * last shmdt() by setting pending_destroy.
             *
             * Skip if pending_destroy is already set — another thread already
             * called IPC_RMID before we got here.
             */
            if (!seg->pending_destroy) {
                key_table_remove(seg);

                /* Only free now if there is room in the work buffer; otherwise
                 * leave the segment in place (it will be reclaimed on a later
                 * pass / by the last shmdt) rather than overflow the buffer. */
                if (seg->attach_count == 0 && nfree < SHM_CLEANUP_MAX) {
                    id_table[i] = NULL;
                    /* Queue for post-unlock free. */
                    cleanup_work[nfree].phys_pages = (void**)seg->phys_addr;
                    cleanup_work[nfree].npages     = seg->pages;
                    cleanup_work[nfree].seg        = seg;
                    nfree++;
                    kprintf("[SHM] Cleanup PID %d: queuing owned segment %d for free\n",
                            pid, seg->id);
                    /* seg pointer is now stale from callers' perspective —
                     * id_table[i] is NULL; we own it for the post-unlock free. */
                } else {
                    seg->pending_destroy = 1;
                    kprintf("[SHM] Cleanup PID %d: segment %d pending destroy "
                            "(%u attachments)\n",
                            pid, seg->id, seg->attach_count);
                }
            }
        }
        // Non-owned segments: attach_count was already decremented in Pass 0
        // from this pid's attachment records; the freeing sweep below reclaims
        // any that are now IPC_RMID'd with zero attachers.
    }

    // Pass 2: a segment that was IPC_RMID'd (pending_destroy) whose last attacher
    // was this dying process now has attach_count 0 -> reclaim it. (Owned
    // segments already freed/queued above set id_table[i]=NULL and are skipped.)
    for (uint32_t i = 0; i < SHM_TABLE_SIZE && nfree < SHM_CLEANUP_MAX; i++) {
        shm_segment_t* seg = id_table[i];
        if (!seg) continue;
        if (seg->pending_destroy && seg->attach_count == 0) {
            id_table[i] = NULL;
            key_table_remove(seg);
            cleanup_work[nfree].phys_pages = (void**)seg->phys_addr;
            cleanup_work[nfree].npages     = seg->pages;
            cleanup_work[nfree].seg        = seg;
            nfree++;
        }
    }

    spin_unlock(&shm_lock);

    /* ── Post-unlock: heap and PMM frees (no lock held) ─────────────────── */
    for (uint32_t i = 0; i < nfree; i++) {
        void** pp    = cleanup_work[i].phys_pages;
        uint32_t np  = cleanup_work[i].npages;
        shm_segment_t* s = cleanup_work[i].seg;
        if (pp) {
            for (uint32_t p = 0; p < np; p++) {
                if (pp[p]) {
                    pmm_free_page(pp[p]);
                }
            }
            kfree(pp);
        }
        kprintf("[SHM] Cleanup PID %d: freed owned segment %d\n", pid, s->id);
        kfree(s);
    }
}
