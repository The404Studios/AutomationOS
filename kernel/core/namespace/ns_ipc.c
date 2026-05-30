#include "../../include/namespace.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"

// External namespace ID counter
extern uint32_t next_namespace_id;

/**
 * Create a new IPC namespace
 *
 * IPC namespaces isolate System V IPC objects:
 * - Shared memory segments (shmget, shmat, shmdt, shmctl)
 * - Semaphore arrays (semget, semop, semctl)
 * - Message queues (msgget, msgsnd, msgrcv, msgctl)
 *
 * Each namespace has its own IPC identifier space.
 * IPC objects in one namespace are invisible to processes in other namespaces.
 *
 * @return New IPC namespace or NULL on error
 */
ipc_namespace_t* ipc_namespace_create(void) {
    ipc_namespace_t* ns = (ipc_namespace_t*)kmalloc(sizeof(ipc_namespace_t));
    if (!ns) {
        kprintf("[IPC_NS] Failed to allocate IPC namespace\n");
        return NULL;
    }

    // Initialize fields
    ns->id = __atomic_fetch_add(&next_namespace_id, 1, __ATOMIC_SEQ_CST);
    ns->table = NULL;  // Will be initialized by IPC subsystem
    ns->ref_count = 1;

    kprintf("[IPC_NS] Created IPC namespace %d\n", ns->id);

    // TODO: Initialize IPC tables for this namespace
    // When IPC subsystem is implemented, this should:
    // 1. Initialize shared memory segment table
    // 2. Initialize semaphore array table
    // 3. Initialize message queue table
    //
    // Each namespace maintains separate IPC identifier spaces:
    // - Key to ID mappings (ftok keys -> IPC IDs)
    // - ID to object mappings (IPC IDs -> actual objects)
    // - Permission checks (IPC_CREAT, IPC_EXCL, access modes)

    return ns;
}

/**
 * Destroy an IPC namespace
 * Should only be called when ref_count reaches 0
 */
void ipc_namespace_destroy(ipc_namespace_t* ns) {
    if (!ns) {
        return;
    }

    kprintf("[IPC_NS] Destroying IPC namespace %d\n", ns->id);

    // TODO: Cleanup IPC objects in this namespace
    // When IPC subsystem is implemented, this should:
    // 1. Destroy all shared memory segments
    //    - Detach all attached processes
    //    - Free physical memory backing the segments
    // 2. Destroy all semaphore arrays
    //    - Wake up any processes waiting on semaphores
    //    - Free semaphore structures
    // 3. Destroy all message queues
    //    - Free queued messages
    //    - Wake up blocked senders/receivers
    //
    // Note: IPC objects can outlive the processes that created them
    // (unless created with IPC_RMID), so we must explicitly clean up

    kfree(ns);
}
