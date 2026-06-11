/*
 * kernel/ipc/ipc.c
 * ================
 *
 * IPC subsystem umbrella initialization and shared utilities.
 *
 * This file provides:
 *  1. Top-level ipc_init() that orchestrates initialization of all IPC components:
 *       - Shared memory (shm_init)
 *       - Message queues (msg_init)
 *       - Desktop notifications (notify_init)
 *       - Clipboard (clipboard_init)
 *  2. Generic permission checking (ipc_check_permission) for IPC objects.
 *
 * NOTE: Current subsystems (shm, msgqueue) use inline permission checkers
 * with raw struct fields (creator_uid, mode, etc.) instead of the ipc_perm_t
 * abstraction. The generic ipc_check_permission() is provided for future
 * refactoring or new IPC mechanisms that adopt the standardized permission
 * structure. Both approaches are functionally equivalent.
 *
 * Call ipc_init() once from kernel_main() after heap_init() and
 * before userspace is started.
 */

#include "../include/ipc.h"
#include "../include/notify.h"
#include "../include/clipboard.h"
#include "../include/kernel.h"
#include "../include/mem.h"
#include "../include/spinlock.h"

/* Forward declarations for subsystem initializers */
extern void shm_init(void);
extern void msg_init(void);
extern void notify_init(void);
extern void clipboard_init(void);

/* Forward declarations for cleanup functions. shm_cleanup_process is declared in
 * ipc.h now (takes process_t*); only msg_cleanup_process (by pid) is declared here. */
extern void msg_cleanup_process(uint32_t pid);

/**
 * ipc_check_permission() — Generic IPC permission checker
 *
 * Determines if a process with the given uid/gid has the requested access
 * (read/write) to an IPC object with the specified permission structure.
 *
 * Permission semantics (matches subsystem implementations):
 *   - Root (uid == 0) has full access
 *   - Creator/owner (uid == perm->uid or perm->cuid) has full access
 *   - Group match (gid == perm->gid or perm->cgid) checks owner permission bits
 *   - Others check the "other" permission bits (mode >> 6)
 *
 * NOTE: The group check uses the OWNER permission bits (not group bits >> 3).
 * This matches the existing shm_check_permission() and msg_check_permission()
 * implementations in shm.c:200 and msgqueue.c:167. While this deviates from
 * standard POSIX permission semantics, it is preserved for consistency.
 *
 * @param perm        IPC permission structure
 * @param uid         Requesting process user ID
 * @param gid         Requesting process group ID
 * @param access_mode IPC_R (0400) for read, IPC_W (0200) for write
 * @return            true if access is allowed, false otherwise
 */
bool ipc_check_permission(ipc_perm_t* perm, uint32_t uid, uint32_t gid, int access_mode)
{
    /* Root has unconditional access */
    if (uid == 0) {
        return true;
    }

    /* Owner (original creator or current owner) has full access */
    if (uid == perm->uid || uid == perm->cuid) {
        return true;
    }

    /*
     * Group member: check owner permission bits.
     * DEVIATION: Standard POSIX would check (access_mode >> 3) for group bits,
     * but the existing subsystems check the owner bits directly. This is
     * preserved to match shm_check_permission/msg_check_permission behavior.
     */
    if (gid == perm->gid || gid == perm->cgid) {
        return (perm->mode & access_mode) != 0;
    }

    /* Others: check "other" permission bits (shifted right by 6) */
    return (perm->mode & (access_mode >> 6)) != 0;
}

/**
 * ipc_table_create() — Create a per-namespace IPC table
 *
 * Allocates and initializes an IPC table for use in a namespace.
 * The table maintains separate linked lists of shared memory segments
 * and message queues, with independent ID allocators and thread-safe
 * locking.
 *
 * This is intended for per-namespace IPC isolation (see namespace.h),
 * allowing each namespace to maintain its own IPC identifier space.
 *
 * NOTE: The current global IPC implementation (shm.c, msgqueue.c) uses
 * direct-index tables and key hash tables for O(1) performance. This
 * per-namespace table design uses simpler linked lists since:
 *   1. It matches the ipc_table_t structure defined in ipc.h:181
 *   2. Per-namespace IPC object counts are typically low (< 10)
 *   3. It allows easy iteration for cleanup on namespace destruction
 *
 * When namespace IPC is fully implemented, the global tables should
 * be refactored to use ipc_table_t or vice versa.
 *
 * @return Initialized IPC table, or NULL on allocation failure
 */
ipc_table_t* ipc_table_create(void)
{
    ipc_table_t* table = (ipc_table_t*)kmalloc(sizeof(ipc_table_t));
    if (!table) {
        kprintf("[IPC] Failed to allocate IPC table\n");
        return NULL;
    }

    /* Initialize linked lists to empty */
    table->shm_list = NULL;
    table->msg_list = NULL;

    /* Initialize ID allocators (1-based, matching global convention) */
    table->next_shm_id = 1;
    table->next_msg_id = 1;

    /* Allocate and initialize spinlock for thread safety */
    table->lock = kmalloc(sizeof(spinlock_t));
    if (!table->lock) {
        kprintf("[IPC] Failed to allocate IPC table lock\n");
        kfree(table);
        return NULL;
    }
    spin_lock_init((spinlock_t*)table->lock);

    return table;
}

/**
 * ipc_table_destroy() — Destroy a per-namespace IPC table
 *
 * Cleans up all IPC objects (shared memory segments, message queues)
 * in the table and frees the table structure itself.
 *
 * This is called when a namespace is destroyed and must:
 *  1. Free all shared memory segments
 *     - Segments may still be attached to processes if not explicitly
 *       removed with shmctl(IPC_RMID). These must be forcibly freed.
 *  2. Free all message queues
 *     - Queued messages must be freed to prevent memory leaks
 *  3. Free the lock and table structure
 *
 * Cleanup order matches the logic in shm_cleanup_process() (shm.c:830)
 * and msg_cleanup_process() (msgqueue.c:648):
 *   - For SHM: free physical page array, then segment structure
 *   - For MSG: free all queued messages (fused allocation), then queue
 *
 * @param table  IPC table to destroy (may be NULL)
 */
void ipc_table_destroy(ipc_table_t* table)
{
    if (!table) {
        return;
    }

    uint32_t shm_freed = 0;
    uint32_t mq_freed = 0;
    uint32_t msg_freed = 0;

    /* Acquire lock to prevent concurrent access during teardown.
     * NOTE: If any other code path attempts to access this table during
     * destruction, that's a use-after-free bug in the caller. This lock
     * is defensive only. */
    if (table->lock) {
        spin_lock((spinlock_t*)table->lock);
    }

    /* ── Cleanup shared memory segments ──────────────────────────────────
     *
     * Walk table->shm_list and for each segment:
     *  1. Free physical pages backing the segment (array of page pointers)
     *  2. Free the shm_segment_t structure
     *
     * NOTE: We do NOT attempt to unmap the segment from attached processes
     * because namespace destruction implies all processes in the namespace
     * are already dead or are dying. Their page tables will be torn down
     * by paging_destroy_address_space() in process cleanup.
     *
     * Matches cleanup logic in shm_cleanup_process() (shm.c:936-949):
     *   - phys_addr points to an array of physical page pointers
     *   - Each element is freed with pmm_free_page()
     *   - Then free the array itself with kfree()
     */
    shm_segment_t* seg = table->shm_list;
    while (seg) {
        shm_segment_t* next = seg->next;

        /* Free physical pages if allocated */
        if (seg->phys_addr && seg->pages > 0) {
            void** page_array = (void**)seg->phys_addr;
            for (uint32_t i = 0; i < seg->pages; i++) {
                if (page_array[i]) {
                    pmm_free_page(page_array[i]);
                }
            }
            kfree(page_array);
        }

        /* Free the segment structure */
        kfree(seg);
        shm_freed++;
        seg = next;
    }

    /* ── Cleanup message queues ───────────────────────────────────────────
     *
     * Walk table->msg_list and for each queue:
     *  1. Walk queue->first message list and free each message
     *  2. Free the msg_queue_t structure
     *
     * NOTE: In msgqueue.c, messages use fused allocation where mtext is
     * allocated inline with msg_message_t (see msgqueue.c:441-447).
     * Therefore, kfree(msg) releases both the node and the payload.
     *
     * Matches cleanup logic in msg_cleanup_process() (msgqueue.c:664-669).
     */
    msg_queue_t* q = table->msg_list;
    while (q) {
        msg_queue_t* next_q = q->next;

        /* Free all queued messages */
        msg_message_t* msg = q->first;
        while (msg) {
            msg_message_t* next_msg = msg->next;
            /* Fused allocation: kfree(msg) releases both node and mtext */
            kfree(msg);
            msg_freed++;
            msg = next_msg;
        }

        /* Free the queue structure */
        kfree(q);
        mq_freed++;
        q = next_q;
    }

    /* Unlock before freeing the lock itself */
    if (table->lock) {
        spin_unlock((spinlock_t*)table->lock);
        kfree(table->lock);
    }

    /* Free the table structure itself */
    kfree(table);

    kprintf("[IPC] Destroyed IPC table: %u SHM segments, %u message queues "
            "(%u messages)\n", shm_freed, mq_freed, msg_freed);
}

/**
 * ipc_init() — Initialize all IPC subsystems
 *
 * This umbrella function calls each IPC component initializer in the
 * correct order. Safe to call once during kernel bootstrap.
 */
void ipc_init(void)
{
    kprintf("[IPC] Initializing IPC subsystems...\n");

    /* Shared memory: System V-style shmget/shmat/shmdt */
    shm_init();

    /* Message queues: System V-style msgget/msgsnd/msgrcv */
    msg_init();

    /* Desktop notifications: SYS_NOTIFY/SYS_NOTIFY_POLL */
    notify_init();

    /* Kernel clipboard: SYS_CLIP_SET/SYS_CLIP_GET */
    clipboard_init();

    kprintf("[IPC] IPC subsystem initialization complete\n");
}
