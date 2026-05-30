#include "../../include/namespace.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/sched.h"
#include "../../include/spinlock.h"

#define MAX_PID_PER_NAMESPACE 1024

// External namespace ID counter
extern uint32_t next_namespace_id;

/**
 * Create a new PID namespace
 *
 * @param parent Parent PID namespace (NULL for root namespace)
 * @return New PID namespace or NULL on error
 */
pid_namespace_t* pid_namespace_create(pid_namespace_t* parent) {
    pid_namespace_t* ns = (pid_namespace_t*)kmalloc(sizeof(pid_namespace_t));
    if (!ns) {
        kprintf("[PID_NS] Failed to allocate PID namespace\n");
        return NULL;
    }

    // Allocate process table
    ns->process_table = (struct process**)kmalloc(sizeof(struct process*) * MAX_PID_PER_NAMESPACE);
    if (!ns->process_table) {
        kprintf("[PID_NS] Failed to allocate process table\n");
        kfree(ns);
        return NULL;
    }

    // Initialize fields
    ns->id = __atomic_fetch_add(&next_namespace_id, 1, __ATOMIC_SEQ_CST);
    ns->next_pid = 1;  // PID 0 is reserved, first process gets PID 1
    ns->process_count = 0;
    ns->parent = parent;
    ns->ref_count = 1;
    ns->level = parent ? parent->level + 1 : 0;

    // RACE-006 fix: Initialize namespace lock
    ns->lock = kmalloc(sizeof(spinlock_t));
    if (ns->lock) {
        spin_lock_init((spinlock_t*)ns->lock);
    }

    // Clear process table
    for (int i = 0; i < MAX_PID_PER_NAMESPACE; i++) {
        ns->process_table[i] = NULL;
    }

    kprintf("[PID_NS] Created PID namespace %d (parent: %d, level: %d)\n",
            ns->id, parent ? parent->id : 0, ns->level);

    return ns;
}

/**
 * Destroy a PID namespace
 * Should only be called when ref_count reaches 0
 */
void pid_namespace_destroy(pid_namespace_t* ns) {
    if (!ns) {
        return;
    }

    kprintf("[PID_NS] Destroying PID namespace %d\n", ns->id);

    // Free process table
    if (ns->process_table) {
        kfree(ns->process_table);
    }

    // Free lock
    if (ns->lock) {
        kfree(ns->lock);
    }

    kfree(ns);
}

/**
 * Allocate a PID in the namespace
 * RACE-006 fix: Protect namespace operations with lock
 *
 * @param ns PID namespace
 * @param proc Process to register with this PID
 * @return Allocated PID or 0 on error
 */
uint32_t pid_namespace_alloc_pid(pid_namespace_t* ns, struct process* proc) {
    if (!ns || !proc) {
        return 0;
    }

    spinlock_t* lock = (spinlock_t*)ns->lock;
    if (lock) spin_lock(lock);

    // Check if we've hit the limit
    if (ns->next_pid >= MAX_PID_PER_NAMESPACE) {
        if (lock) spin_unlock(lock);
        kprintf("[PID_NS] PID namespace %d is full (max %d processes)\n",
                ns->id, MAX_PID_PER_NAMESPACE);
        return 0;
    }

    // Allocate PID
    uint32_t pid = ns->next_pid++;

    // Register process in the table
    ns->process_table[pid] = proc;
    ns->process_count++;

    if (lock) spin_unlock(lock);

    kprintf("[PID_NS] Allocated PID %d in namespace %d (count: %d)\n",
            pid, ns->id, ns->process_count);

    return pid;
}

/**
 * Free a PID in the namespace
 * RACE-006 fix: Protect namespace operations with lock
 *
 * @param ns PID namespace
 * @param pid PID to free
 */
void pid_namespace_free_pid(pid_namespace_t* ns, uint32_t pid) {
    if (!ns || pid == 0 || pid >= MAX_PID_PER_NAMESPACE) {
        return;
    }

    spinlock_t* lock = (spinlock_t*)ns->lock;
    if (lock) spin_lock(lock);

    // Remove from process table
    if (ns->process_table[pid]) {
        ns->process_table[pid] = NULL;
        ns->process_count--;

        if (lock) spin_unlock(lock);

        kprintf("[PID_NS] Freed PID %d in namespace %d (count: %d)\n",
                pid, ns->id, ns->process_count);
        return;
    }

    if (lock) spin_unlock(lock);
}

/**
 * Find a process by PID in the namespace
 * RACE-006 fix: Protect namespace lookup with lock
 *
 * @param ns PID namespace
 * @param pid PID to look up
 * @return Process pointer or NULL if not found
 */
struct process* pid_namespace_find_process(pid_namespace_t* ns, uint32_t pid) {
    if (!ns || pid == 0 || pid >= MAX_PID_PER_NAMESPACE) {
        return NULL;
    }

    spinlock_t* lock = (spinlock_t*)ns->lock;
    if (lock) spin_lock(lock);

    struct process* proc = ns->process_table[pid];

    if (lock) spin_unlock(lock);

    return proc;
}

/**
 * Translate a PID from one namespace to another
 *
 * This is critical for container isolation:
 * - A process in a child namespace with PID 1 might be PID 157 in the parent
 * - Processes can only see PIDs in their namespace and descendant namespaces
 * - Translation fails if target namespace is not an ancestor/descendant
 *
 * @param from Source PID namespace
 * @param to Target PID namespace
 * @param pid PID in the source namespace
 * @return PID in target namespace, or 0 if translation not possible
 */
uint32_t pid_namespace_translate(pid_namespace_t* from, pid_namespace_t* to, uint32_t pid) {
    if (!from || !to || pid == 0) {
        return 0;
    }

    // Same namespace - no translation needed
    if (from == to) {
        return pid;
    }

    // Find the process in the source namespace
    struct process* proc = pid_namespace_find_process(from, pid);
    if (!proc) {
        return 0;
    }

    // TODO: Walk the namespace hierarchy to find the PID in the target namespace
    // For now, we only support translation within the same namespace
    // Full implementation would:
    // 1. Check if 'to' is an ancestor of 'from' (walk up parent chain)
    // 2. If yes, look up the process's PID in that ancestor namespace
    // 3. If 'to' is a descendant of 'from', check if process is visible
    // 4. Otherwise, translation is not allowed (different branches)

    kprintf("[PID_NS] PID translation from NS %d to NS %d not yet implemented\n",
            from->id, to->id);

    return 0;
}
