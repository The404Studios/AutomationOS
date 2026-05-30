/*
 * Userspace IPC Library
 * =====================
 *
 * POSIX-compatible wrappers for System V IPC (shared memory, message queues)
 * Provides familiar API for application developers.
 */

#include "ipc.h"
#include "syscall.h"

// System call numbers for IPC (must be registered in kernel)
#define SYS_SHMGET  18
#define SYS_SHMAT   19
#define SYS_SHMDT   20
#define SYS_SHMCTL  21
#define SYS_MSGGET  22
#define SYS_MSGSND  23
#define SYS_MSGRCV  24
#define SYS_MSGCTL  25

// Generic syscall function (defined in syscall.c)
extern long syscall(long number, ...);

/*
 * Shared Memory Functions
 * =======================
 */

// Get shared memory segment ID
int shmget(key_t key, size_t size, int shmflg) {
    return (int)syscall(SYS_SHMGET, key, size, shmflg);
}

// Attach shared memory segment
void* shmat(int shmid, const void* shmaddr, int shmflg) {
    long result = syscall(SYS_SHMAT, shmid, shmaddr, shmflg);

    // Negative values are errors, return NULL
    if (result < 0) {
        return (void*)0;
    }

    // Positive values are virtual addresses
    return (void*)result;
}

// Detach shared memory segment
int shmdt(const void* shmaddr) {
    return (int)syscall(SYS_SHMDT, shmaddr);
}

// Shared memory control operations
int shmctl(int shmid, int cmd, struct shmid_ds* buf) {
    return (int)syscall(SYS_SHMCTL, shmid, cmd, buf);
}

/*
 * Message Queue Functions
 * =======================
 */

// Get message queue ID
int msgget(key_t key, int msgflg) {
    return (int)syscall(SYS_MSGGET, key, msgflg);
}

// Send message to queue
int msgsnd(int msqid, const void* msgp, size_t msgsz, int msgflg) {
    return (int)syscall(SYS_MSGSND, msqid, msgp, msgsz, msgflg);
}

// Receive message from queue
ssize_t msgrcv(int msqid, void* msgp, size_t msgsz, long msgtyp, int msgflg) {
    return (ssize_t)syscall(SYS_MSGRCV, msqid, msgp, msgsz, msgtyp, msgflg);
}

// Message queue control operations
int msgctl(int msqid, int cmd, struct msqid_ds* buf) {
    return (int)syscall(SYS_MSGCTL, msqid, cmd, buf);
}

/*
 * Helper Functions
 * ================
 */

// Simple ftok implementation (key generation from path + id)
key_t ftok(const char* pathname, int proj_id) {
    // Simple hash-based key generation
    // In real implementation, would use inode + device ID
    key_t key = (key_t)proj_id;

    const char* p = pathname;
    while (*p) {
        key = key * 31 + *p;
        p++;
    }

    return key;
}
