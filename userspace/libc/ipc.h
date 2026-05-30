/*
 * Userspace IPC Header
 * ====================
 *
 * POSIX-compatible API for System V IPC
 */

#ifndef _IPC_H
#define _IPC_H

// Type definitions
typedef int key_t;
typedef unsigned long size_t;
typedef long ssize_t;

// IPC Key constants
#define IPC_PRIVATE  0      // Private IPC object

// IPC command flags
#define IPC_CREAT   0x0200  // Create if doesn't exist
#define IPC_EXCL    0x0400  // Fail if exists (with IPC_CREAT)
#define IPC_RMID    0x1000  // Remove identifier
#define IPC_STAT    0x2000  // Get status
#define IPC_SET     0x3000  // Set options

// Permission bits
#define IPC_R   0400        // Read permission
#define IPC_W   0200        // Write permission

/*
 * Shared Memory
 */

// Shared memory flags for shmat()
#define SHM_RDONLY  0x1000  // Read-only attach
#define SHM_RND     0x2000  // Round address to SHMLBA

// Shared memory alignment
#define SHMLBA      4096    // Page size

// Shared memory statistics (for shmctl)
struct shmid_ds {
    unsigned int shm_perm_uid;      // Owner UID
    unsigned int shm_perm_gid;      // Owner GID
    unsigned int shm_perm_mode;     // Permissions
    unsigned long shm_segsz;        // Size of segment
    unsigned long shm_atime;        // Last attach time
    unsigned long shm_dtime;        // Last detach time
    unsigned long shm_ctime;        // Last change time
    unsigned int shm_cpid;          // Creator PID
    unsigned int shm_lpid;          // Last operator PID
    unsigned int shm_nattch;        // Number of attachments
};

// Shared memory functions
int shmget(key_t key, size_t size, int shmflg);
void* shmat(int shmid, const void* shmaddr, int shmflg);
int shmdt(const void* shmaddr);
int shmctl(int shmid, int cmd, struct shmid_ds* buf);

/*
 * Message Queues
 */

// Message buffer structure
struct msgbuf {
    long mtype;         // Message type (must be > 0)
    char mtext[1];      // Message data (variable length)
};

// Message queue flags
#define MSG_NOERROR 0x1000  // Truncate if too long
#define MSG_COPY    0x2000  // Copy without removing

// Message queue limits
#define MSGMAX      8192    // Max message size
#define MSGMNB      16384   // Max queue size

// Message queue statistics (for msgctl)
struct msqid_ds {
    unsigned int msg_perm_uid;      // Owner UID
    unsigned int msg_perm_gid;      // Owner GID
    unsigned int msg_perm_mode;     // Permissions
    unsigned long msg_stime;        // Last send time
    unsigned long msg_rtime;        // Last receive time
    unsigned long msg_ctime;        // Last change time
    unsigned int msg_qnum;          // Number of messages
    unsigned long msg_qbytes;       // Total bytes in queue
    unsigned int msg_lspid;         // Last send PID
    unsigned int msg_lrpid;         // Last receive PID
};

// Message queue functions
int msgget(key_t key, int msgflg);
int msgsnd(int msqid, const void* msgp, size_t msgsz, int msgflg);
ssize_t msgrcv(int msqid, void* msgp, size_t msgsz, long msgtyp, int msgflg);
int msgctl(int msqid, int cmd, struct msqid_ds* buf);

/*
 * Helper Functions
 */

// Generate IPC key from pathname and project ID
key_t ftok(const char* pathname, int proj_id);

#endif // _IPC_H
