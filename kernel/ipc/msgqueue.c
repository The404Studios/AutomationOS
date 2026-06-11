#include "../include/ipc.h"
#include "../include/kernel.h"
#include "../include/mem.h"
#include "../include/sched.h"
#include "../include/spinlock.h"
#include "../include/syscall.h"
#include "../include/string.h"
#include "../include/drivers.h"   // timer_get_ticks()

// Per-operation tracing. SILENT by default: msgrcv/msgsnd are on the hot path
// (the compositor and every windowed app poll their event queue many times per
// second), so unconditional kprintf here floods the serial with 100k+ lines per
// boot — under TCG that serial I/O dominates wall-clock and starves late-boot
// apps. Build with -DIPC_VERBOSE to re-enable the trace.
#ifdef IPC_VERBOSE
#define MQ_LOG(...) kprintf(__VA_ARGS__)
#else
#define MQ_LOG(...) ((void)0)
#endif

/*
 * Message Queue Implementation
 * ============================
 *
 * Provides System V-style message queues for typed message passing
 * between processes. Useful for compositor control messages and events.
 *
 * Design:
 * - FIFO ordering within message types
 * - Priority via message types (higher type = higher priority)
 * - Non-blocking and blocking modes supported
 * - Size limits to prevent memory exhaustion
 * - Thread-safe with spinlock protection
 *
 * Performance (post-optimisation):
 * ─────────────────────────────────
 *  msg_find_by_id:  O(1) – direct array index on (id - 1)
 *  msg_find_by_key: O(1) avg – open-addressing hash table (power-of-2)
 *  msgsnd tail-insert: O(1) – maintained last pointer
 *  msgrcv type==0: O(1) – dequeue from head
 *  msgrcv type>0 / type<0: O(k) where k = messages before the match
 *    (bounded by max_msgs = 100; compositor uses type==0 in the hot path)
 *
 * Before: all lookups were O(n) linked-list walks on the global queue list.
 */

// ─── Tuning constants ──────────────────────────────────────────────────────

// Direct-index table: one slot per possible queue ID.
// IDs are 1-based; slot = id-1.  Size must be >= MSGMNI (256).
#define MQ_TABLE_SIZE   MSGMNI          // 256

// Key hash table: power-of-2, separate from the ID table.
// Load factor ≤ 0.5 so we double the capacity (collision rare).
#define MQ_HASH_SIZE    512             // must be power of 2
#define MQ_HASH_MASK    (MQ_HASH_SIZE - 1)

// Sentinel for "no queue" in the key hash table
#define MQ_HASH_EMPTY   ((msg_queue_t*)0)
// Sentinel for "slot was deleted" (tombstone for open-addressing)
#define MQ_HASH_DEAD    ((msg_queue_t*)1)

// ─── Global state ─────────────────────────────────────────────────────────

// Flat array: id_table[id-1] == pointer to queue, or NULL if unused.
static msg_queue_t* id_table[MQ_TABLE_SIZE];

// Open-addressing hash table keyed on queue->key.
// Stores pointers (same objects as id_table, just a different index).
static msg_queue_t* key_table[MQ_HASH_SIZE];

static uint32_t next_msg_id = 1;   // Next queue ID to allocate
static spinlock_t msg_lock;        // Protects both tables

// ─── Internal helpers ─────────────────────────────────────────────────────

// Hash a key to a slot in key_table.
static inline uint32_t mq_key_hash(key_t key) {
    uint32_t k = (uint32_t)key;
    // Multiplicative hash (Knuth); good for integer keys.
    k = (k ^ (k >> 16)) * 0x45d9f3b;
    k = (k ^ (k >> 16)) * 0x45d9f3b;
    k ^= (k >> 16);
    return k & MQ_HASH_MASK;
}

// Insert a queue into the key hash table.
// Caller must hold msg_lock.
static void key_table_insert(msg_queue_t* q) {
    if (q->key == IPC_PRIVATE) {
        return;  // Private queues are not reachable by key
    }
    uint32_t slot = mq_key_hash(q->key);
    for (uint32_t i = 0; i < MQ_HASH_SIZE; i++) {
        msg_queue_t* e = key_table[(slot + i) & MQ_HASH_MASK];
        if (e == MQ_HASH_EMPTY || e == MQ_HASH_DEAD) {
            key_table[(slot + i) & MQ_HASH_MASK] = q;
            return;
        }
    }
    // Table full — should never happen if load factor is low
}

// Remove a queue from the key hash table (tombstone).
// Caller must hold msg_lock.
static void key_table_remove(msg_queue_t* q) {
    if (q->key == IPC_PRIVATE) {
        return;
    }
    uint32_t slot = mq_key_hash(q->key);
    for (uint32_t i = 0; i < MQ_HASH_SIZE; i++) {
        uint32_t idx = (slot + i) & MQ_HASH_MASK;
        msg_queue_t* e = key_table[idx];
        if (e == MQ_HASH_EMPTY) {
            break;  // Not found — nothing to remove
        }
        if (e == q) {
            key_table[idx] = MQ_HASH_DEAD;
            return;
        }
    }
}

// ─── Public lookup API ────────────────────────────────────────────────────

// Find queue by key — O(1) average via hash table.
msg_queue_t* msg_find_by_key(key_t key) {
    if (key == IPC_PRIVATE) {
        return NULL;
    }
    uint32_t slot = mq_key_hash(key);
    for (uint32_t i = 0; i < MQ_HASH_SIZE; i++) {
        uint32_t idx = (slot + i) & MQ_HASH_MASK;
        msg_queue_t* e = key_table[idx];
        if (e == MQ_HASH_EMPTY) {
            return NULL;
        }
        if (e != MQ_HASH_DEAD && e->key == key) {
            return e;
        }
    }
    return NULL;
}

// Find queue by ID — O(1) direct array index.
msg_queue_t* msg_find_by_id(ipc_id_t id) {
    if (id <= 0 || id > MQ_TABLE_SIZE) {
        return NULL;
    }
    return id_table[id - 1];
}

// ─── Subsystem initialisation ─────────────────────────────────────────────

void msg_init(void) {
    kprintf("[MSG] Initializing message queue subsystem\n");
    for (uint32_t i = 0; i < MQ_TABLE_SIZE; i++) {
        id_table[i] = NULL;
    }
    for (uint32_t i = 0; i < MQ_HASH_SIZE; i++) {
        key_table[i] = MQ_HASH_EMPTY;
    }
    next_msg_id = 1;
    spin_lock_init(&msg_lock);
    kprintf("[MSG] Message queues initialized\n");
}

// ─── Permission helper ────────────────────────────────────────────────────

static bool msg_check_permission(msg_queue_t* q, uint32_t uid, uint32_t gid, bool write) {
    if (uid == 0) {
        return true;
    }
    if (uid == q->creator_uid) {
        return true;
    }
    if (gid == q->creator_gid) {
        /* GROUP bits are mode>>3; the owner bits IPC_W/R would grant a group
         * member the owner's rights (mode 0640 => group can WRITE). 'other' >>6. */
        return write ? (q->mode & (IPC_W >> 3)) != 0 : (q->mode & (IPC_R >> 3)) != 0;
    }
    return write ? (q->mode & (IPC_W >> 6)) != 0 : (q->mode & (IPC_R >> 6)) != 0;
}

// ─── SYS_MSGGET ───────────────────────────────────────────────────────────

/*
 * SYS_MSGGET - Create or get message queue
 *
 * Arguments:
 *   key    - IPC key (IPC_PRIVATE for anonymous)
 *   msgflg - Flags (IPC_CREAT, IPC_EXCL, mode bits)
 *
 * Returns: Queue ID on success, negative error code on failure
 *
 * Lock-order discipline (mirrors shm.c two-phase protocol):
 * ---------------------------------------------------------
 * msg_lock must NEVER be held while calling kmalloc/kfree. The heap has its
 * own internal lock; acquiring it while msg_lock is held inverts the lock
 * order and deadlocks under SMP. We use a three-phase protocol:
 *   Phase 1 (under lock):   key/exist check + reserve an ID slot
 *   Phase 2 (lock dropped): kmalloc the queue struct + initialise
 *   Phase 3 (under lock):   re-validate, then publish to both tables
 */
int64_t sys_msgget(uint64_t key, uint64_t msgflg, uint64_t arg3,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;
    key_t k = (key_t)key;
    int flags = (int)msgflg;
    process_t* current = process_get_current();
    if (!current) {
        return IPC_EINVAL;
    }

    MQ_LOG("[MSGGET] key=%d flags=0x%x (PID %d)\n", k, flags, current->pid);

    /* ── Phase 1: key/exist check + ID reservation (under lock) ─────────── */

    spin_lock(&msg_lock);

    // Check if queue with this key already exists — O(1) hash lookup
    if (k != IPC_PRIVATE) {
        msg_queue_t* existing = msg_find_by_key(k);
        if (existing) {
            if (flags & IPC_EXCL) {
                spin_unlock(&msg_lock);
                MQ_LOG("[MSGGET] Queue exists and IPC_EXCL specified\n");
                return IPC_EEXIST;
            }
            spin_unlock(&msg_lock);
            MQ_LOG("[MSGGET] Returning existing queue %d\n", existing->id);
            return existing->id;
        }

        if (!(flags & IPC_CREAT)) {
            spin_unlock(&msg_lock);
            MQ_LOG("[MSGGET] Queue not found and IPC_CREAT not specified\n");
            return IPC_ENOENT;
        }
    }

    // Find a free ID slot — reserve but do NOT publish yet
    if (next_msg_id > MQ_TABLE_SIZE) {
        next_msg_id = 1;
    }
    uint32_t start = next_msg_id;
    while (id_table[next_msg_id - 1] != NULL) {
        next_msg_id++;
        if (next_msg_id > MQ_TABLE_SIZE) {
            next_msg_id = 1;
        }
        if (next_msg_id == start) {
            spin_unlock(&msg_lock);
            MQ_LOG("[MSGGET] No free queue slots\n");
            return IPC_ENOMEM;
        }
    }
    uint32_t reserved_id = next_msg_id;

    spin_unlock(&msg_lock);

    /* ── Phase 2: heap work outside the lock ────────────────────────────── */

    msg_queue_t* q = (msg_queue_t*)kmalloc(sizeof(msg_queue_t));
    if (!q) {
        MQ_LOG("[MSGGET] Failed to allocate queue structure\n");
        return IPC_ENOMEM;
    }

    // Fully initialise while unlocked — safe because q is not visible yet
    memset(q, 0, sizeof(msg_queue_t));
    q->id = (ipc_id_t)reserved_id;
    q->key = k;
    q->first = NULL;
    q->last = NULL;
    q->msg_count = 0;
    q->total_bytes = 0;
    q->max_msgs = 100;
    q->max_bytes = MSGMNB;
    q->creator_uid = current->uid;
    q->creator_gid = current->gid;
    q->owner_pid = current->pid;
    q->mode = flags & 0777;
    q->create_time = timer_get_ticks();
    q->send_time = 0;
    q->recv_time = 0;
    q->next = NULL;

    /* ── Phase 3: commit under lock (re-validate before publishing) ──────── */

    spin_lock(&msg_lock);

    // Re-check: the slot we reserved might have been taken by a concurrent
    // msgget (possible under SMP between Phase 1 and Phase 3).
    if (id_table[reserved_id - 1] != NULL) {
        spin_unlock(&msg_lock);
        kfree(q);
        MQ_LOG("[MSGGET] ID slot %u raced; retrying\n", reserved_id);
        return IPC_ENOMEM;
    }

    // Re-check: for keyed queues, someone else might have created the same
    // key between Phase 1 and Phase 3.
    if (k != IPC_PRIVATE) {
        msg_queue_t* racing = msg_find_by_key(k);
        if (racing) {
            spin_unlock(&msg_lock);
            kfree(q);
            if (flags & IPC_EXCL) {
                MQ_LOG("[MSGGET] Key raced in; IPC_EXCL -> EEXIST\n");
                return IPC_EEXIST;
            }
            MQ_LOG("[MSGGET] Key raced in; returning existing queue %d\n",
                    racing->id);
            return racing->id;
        }
    }

    // Safe to publish
    id_table[q->id - 1] = q;
    key_table_insert(q);

    next_msg_id = reserved_id + 1;
    if (next_msg_id > MQ_TABLE_SIZE) {
        next_msg_id = 1;
    }

    spin_unlock(&msg_lock);

    MQ_LOG("[MSGGET] Created queue %d: key=%d\n", q->id, k);

    return q->id;
}

// ─── SYS_MSGSND ───────────────────────────────────────────────────────────

/*
 * SYS_MSGSND - Send message to queue
 *
 * Arguments:
 *   msqid  - Queue ID (from msgget)
 *   msgp   - Pointer to message buffer (struct msgbuf)
 *   msgsz  - Size of message data (bytes)
 *   msgflg - Flags (IPC_NOWAIT)
 *
 * Returns: 0 on success, negative error code on failure
 *
 * Hot path: queue lookup is now O(1); tail-append is O(1) via q->last.
 * The user-space copy is done once directly into the allocated node buffer
 * (no intermediate kernel staging copy).
 */
int64_t sys_msgsnd(uint64_t msqid, uint64_t msgp, uint64_t msgsz,
                   uint64_t msgflg, uint64_t arg5, uint64_t arg6) {
    (void)arg5; (void)arg6;
    int id = (int)msqid;
    const void* msg = (const void*)msgp;
    size_t size = (size_t)msgsz;
    int flags = (int)msgflg;
    process_t* current = process_get_current();
    if (!current) {
        return IPC_EINVAL;
    }

    if (!msg || size > MSGMAX) {
        return IPC_EINVAL;
    }

    MQ_LOG("[MSGSND] msqid=%d size=%lu flags=0x%x (PID %d)\n",
            id, size, flags, current->pid);

    // Copy message type from user space (just the int64_t header field)
    struct msgbuf header;
    if (copy_from_user(&header, msg, sizeof(int64_t)) != COPY_SUCCESS) {
        return IPC_EFAULT;
    }

    if (header.mtype <= 0) {
        MQ_LOG("[MSGSND] Invalid message type: %ld\n", header.mtype);
        return IPC_EINVAL;
    }

    // Build the message node OUTSIDE msg_lock. Both the kmalloc and the
    // (potentially faulting) copy_from_user must not run while holding the lock:
    // a user page fault during the copy with the lock held would deadlock once a
    // #PF handler / SMP exists, and holding a heap lock under the IPC lock
    // inverts the lock order. Allocate + copy first; take msg_lock only to look
    // up the queue, check limits, and link the node.
    // Layout: [msg_message_t header][payload bytes] in one allocation.
    msg_message_t* node = (msg_message_t*)kmalloc(sizeof(msg_message_t) + size);
    if (!node) {
        return IPC_ENOMEM;
    }
    node->mtext = (char*)node + sizeof(msg_message_t);  // payload follows header
    const char* user_data = (const char*)msg + sizeof(int64_t);
    if (copy_from_user(node->mtext, user_data, size) != COPY_SUCCESS) {
        kfree(node);
        return IPC_EFAULT;
    }
    node->mtype = header.mtype;
    node->msize = size;
    node->next  = NULL;

    spin_lock(&msg_lock);

    // O(1) queue lookup
    msg_queue_t* q = msg_find_by_id(id);
    if (!q) {
        spin_unlock(&msg_lock);
        kfree(node);
        MQ_LOG("[MSGSND] Queue %d not found\n", id);
        return IPC_EINVAL;
    }

    if (!msg_check_permission(q, current->uid, current->gid, true)) {
        spin_unlock(&msg_lock);
        kfree(node);
        MQ_LOG("[MSGSND] Permission denied\n");
        return IPC_EACCES;
    }

    if (q->msg_count >= q->max_msgs || q->total_bytes + size > q->max_bytes) {
        spin_unlock(&msg_lock);
        kfree(node);
        if (flags & IPC_NOWAIT) {
            return IPC_EAGAIN;
        }
        MQ_LOG("[MSGSND] Queue full (blocking not implemented)\n");
        return IPC_EAGAIN;
    }

    // O(1) tail-append using q->last
    if (q->last) {
        q->last->next = node;
        q->last = node;
    } else {
        q->first = node;
        q->last = node;
    }

    q->msg_count++;
    q->total_bytes += size;
    q->send_time = timer_get_ticks();

    spin_unlock(&msg_lock);

    MQ_LOG("[MSGSND] Sent message type=%ld size=%lu to queue %d\n",
            header.mtype, size, id);

    return IPC_SUCCESS;
}

// ─── SYS_MSGRCV ───────────────────────────────────────────────────────────

/*
 * SYS_MSGRCV - Receive message from queue
 *
 * Arguments:
 *   msqid  - Queue ID
 *   msgp   - Pointer to receive buffer (struct msgbuf)
 *   msgsz  - Maximum message size
 *   msgtyp - Message type to receive:
 *            0 = first message in queue  (O(1) – head dequeue)
 *            >0 = first message of type msgtyp  (O(k))
 *            <0 = first message with lowest type <= |msgtyp|  (O(k))
 *   msgflg - Flags (IPC_NOWAIT, MSG_NOERROR)
 *
 * Returns: Number of bytes received, negative error code on failure
 *
 * Semantics preserved exactly including IPC_NOWAIT → IPC_ENOMSG.
 */
int64_t sys_msgrcv(uint64_t msqid, uint64_t msgp, uint64_t msgsz,
                   uint64_t msgtyp, uint64_t msgflg, uint64_t arg6) {
    (void)arg6;
    int id = (int)msqid;
    void* msg = (void*)msgp;
    size_t size = (size_t)msgsz;
    int64_t type = (int64_t)msgtyp;
    int flags = (int)msgflg;
    process_t* current = process_get_current();
    if (!current) {
        return IPC_EINVAL;
    }

    if (!msg) {
        return IPC_EINVAL;
    }

    MQ_LOG("[MSGRCV] msqid=%d size=%lu type=%ld flags=0x%x (PID %d)\n",
            id, size, type, flags, current->pid);

    // Pre-allocate a kernel bounce buffer for the payload BEFORE taking the
    // lock, so neither the heap alloc nor the user delivery runs under msg_lock
    // (a fault on the user buffer with the lock held would deadlock once a #PF
    // handler / SMP exists). cap = what the caller will accept, bounded by
    // MSGMAX (messages can never exceed that).
    size_t cap = (size < (size_t)MSGMAX) ? size : (size_t)MSGMAX;
    char* tmp = NULL;
    if (cap > 0) {
        tmp = (char*)kmalloc(cap);
        if (!tmp) return IPC_ENOMEM;
    }

    spin_lock(&msg_lock);

    // O(1) queue lookup
    msg_queue_t* q = msg_find_by_id(id);
    if (!q) {
        spin_unlock(&msg_lock);
        if (tmp) kfree(tmp);
        MQ_LOG("[MSGRCV] Queue %d not found\n", id);
        return IPC_EINVAL;
    }

    if (!msg_check_permission(q, current->uid, current->gid, false)) {
        spin_unlock(&msg_lock);
        if (tmp) kfree(tmp);
        MQ_LOG("[MSGRCV] Permission denied\n");
        return IPC_EACCES;
    }

    // Find matching message in queue
    msg_message_t* m = q->first;
    msg_message_t* prev = NULL;

    while (m) {
        bool match = false;

        if (type == 0) {
            match = true;         // First message — O(1) head dequeue
        } else if (type > 0) {
            match = (m->mtype == type);
        } else {
            match = (m->mtype <= -type);
        }

        if (match) {
            break;
        }

        prev = m;
        m = m->next;
    }

    if (!m) {
        spin_unlock(&msg_lock);
        if (tmp) kfree(tmp);
        if (flags & IPC_NOWAIT) {
            return IPC_ENOMSG;
        }
        MQ_LOG("[MSGRCV] No matching message (blocking not implemented)\n");
        return IPC_ENOMSG;
    }

    // Check if message fits in buffer
    size_t copy_size = m->msize;
    if (copy_size > size) {
        if (flags & MSG_NOERROR) {
            copy_size = size;
        } else {
            spin_unlock(&msg_lock);
            if (tmp) kfree(tmp);
            return IPC_E2BIG;
        }
    }

    // Snapshot the message into the kernel bounce buffer, unlink it from the
    // queue, and record the pointer for deferred free AFTER unlock. The kfree
    // must NOT run under msg_lock (the heap has its own lock; holding msg_lock
    // across it inverts the lock order and will deadlock under SMP). User-space
    // delivery also happens after unlock.
    int64_t mtype_snapshot = m->mtype;
    if (copy_size > 0) {
        memcpy(tmp, m->mtext, copy_size);
    }

    if (prev) {
        prev->next = m->next;
    } else {
        q->first = m->next;
    }
    if (q->last == m) {
        q->last = prev;
    }
    q->msg_count--;
    q->total_bytes -= m->msize;
    q->recv_time = timer_get_ticks();
    msg_message_t* deferred_msg = m;   // save for post-unlock free

    spin_unlock(&msg_lock);

    kfree(deferred_msg);               // free outside the lock

    // Deliver to user space OUTSIDE the lock. If the user buffer is bad the
    // message is already consumed (acceptable — the caller passed a bad ptr).
    struct msgbuf header;
    header.mtype = mtype_snapshot;
    if (copy_to_user(msg, &header, sizeof(int64_t)) != COPY_SUCCESS) {
        if (tmp) kfree(tmp);
        return IPC_EFAULT;
    }
    if (copy_size > 0 &&
        copy_to_user((char*)msg + sizeof(int64_t), tmp, copy_size) != COPY_SUCCESS) {
        if (tmp) kfree(tmp);
        return IPC_EFAULT;
    }
    if (tmp) kfree(tmp);

    MQ_LOG("[MSGRCV] Received message type=%ld size=%lu from queue %d\n",
            mtype_snapshot, copy_size, id);

    return (int64_t)copy_size;
}

// ─── SYS_MSGCTL ───────────────────────────────────────────────────────────

/*
 * SYS_MSGCTL - Message queue control operations
 *
 * Arguments:
 *   msqid - Queue ID
 *   cmd   - Control command (IPC_RMID, IPC_STAT, IPC_SET)
 *   buf   - Buffer for command-specific data
 *
 * Returns: 0 on success, negative error code on failure
 *
 * IPC_RMID removal is now O(1): clear id_table[id-1] and tombstone the
 * key hash entry, then free — no predecessor-list walk needed.
 */
int64_t sys_msgctl(uint64_t msqid, uint64_t cmd, uint64_t buf,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg4; (void)arg5; (void)arg6;
    int id = (int)msqid;
    int command = (int)cmd;
    void* buffer = (void*)buf;
    (void)buffer;
    process_t* current = process_get_current();
    if (!current) {
        return IPC_EINVAL;
    }

    MQ_LOG("[MSGCTL] msqid=%d cmd=0x%x (PID %d)\n", id, command, current->pid);

    spin_lock(&msg_lock);

    // O(1) queue lookup
    msg_queue_t* q = msg_find_by_id(id);
    if (!q) {
        spin_unlock(&msg_lock);
        MQ_LOG("[MSGCTL] Queue %d not found\n", id);
        return IPC_EINVAL;
    }

    switch (command) {
        case IPC_RMID:
            if (current->uid != 0 && current->uid != q->creator_uid) {
                spin_unlock(&msg_lock);
                return IPC_EACCES;
            }

            // O(1) removal from both tables
            id_table[q->id - 1] = NULL;
            key_table_remove(q);

            // Snapshot the queue and its message chain for post-unlock free.
            // kfree must NOT run under msg_lock (heap has its own lock;
            // holding msg_lock across it inverts the lock order).
            {
                msg_message_t* to_free_msgs = q->first;
                msg_queue_t*   to_free_q    = q;

                spin_unlock(&msg_lock);

                // Free all pending messages outside the lock
                msg_message_t* msg = to_free_msgs;
                while (msg) {
                    msg_message_t* next = msg->next;
                    kfree(msg);   // payload is embedded; one free suffices
                    msg = next;
                }
                kfree(to_free_q);
            }

            MQ_LOG("[MSGCTL] Deleted queue %d\n", id);
            return IPC_SUCCESS;

        case IPC_STAT:
            spin_unlock(&msg_lock);
            return IPC_SUCCESS;

        case IPC_SET:
            spin_unlock(&msg_lock);
            return IPC_SUCCESS;

        default:
            spin_unlock(&msg_lock);
            return IPC_EINVAL;
    }
}

// ─── Process cleanup ──────────────────────────────────────────────────────

/*
 * msg_cleanup_process - destroy all message queues owned by a dying process.
 *
 * Called from process_unref() (in process.c) when the last reference to a
 * process drops to zero, i.e. in the "ref_count == 0" branch, before
 * paging_destroy_address_space().
 *
 * Message queues have no "attached" state (unlike SHM), so the cleanup is
 * straightforward: iterate id_table, and for every queue whose owner_pid
 * matches, perform a full IPC_RMID: drain all pending messages, remove from
 * both lookup tables, free the queue struct.  This mirrors exactly what
 * sys_msgctl(IPC_RMID) does, but without the permission check (kernel path).
 */
/*
 * Deferred-free work item for msg_cleanup_process: one entry per queue to free
 * after we drop msg_lock. A process typically owns 1-3 queues, so a small fixed
 * buffer suffices.
 */
#define MSG_CLEANUP_MAX 32
typedef struct {
    msg_queue_t*    queue;
    msg_message_t*  first_msg;   /* head of the queued-message chain */
} msg_deferred_free_t;
static msg_deferred_free_t msg_cleanup_work[MSG_CLEANUP_MAX];

void msg_cleanup_process(uint32_t pid) {
    MQ_LOG("[MSG] Cleanup for PID %d\n", pid);

    uint32_t nfree = 0;

    spin_lock(&msg_lock);

    for (uint32_t i = 0; i < MQ_TABLE_SIZE; i++) {
        msg_queue_t* q = id_table[i];
        if (!q || q->owner_pid != pid) {
            continue;
        }

        /* Remove from both lookup tables first */
        id_table[i] = NULL;
        key_table_remove(q);

        /* Snapshot the queue and its message chain for post-unlock free.
         * The kfree calls must NOT run under msg_lock (the heap has its own
         * lock; holding msg_lock across it inverts the lock order). */
        if (nfree < MSG_CLEANUP_MAX) {
            msg_cleanup_work[nfree].queue     = q;
            msg_cleanup_work[nfree].first_msg = q->first;
            nfree++;
        }

        MQ_LOG("[MSG] Cleanup PID %d: queuing queue %d for deferred free\n", pid, q->id);
    }

    spin_unlock(&msg_lock);

    /* Heap frees outside the lock */
    for (uint32_t i = 0; i < nfree; i++) {
        msg_message_t* m = msg_cleanup_work[i].first_msg;
        while (m) {
            msg_message_t* next = m->next;
            kfree(m);
            m = next;
        }
        MQ_LOG("[MSG] Cleanup PID %d: freed queue\n", pid);
        kfree(msg_cleanup_work[i].queue);
    }
}
