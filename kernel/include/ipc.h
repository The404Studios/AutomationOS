#ifndef IPC_H
#define IPC_H

#include "types.h"

/*
 * Inter-Process Communication (IPC) API
 * =====================================
 *
 * Provides System V-style IPC mechanisms:
 * 1. Shared Memory - Zero-copy data sharing between processes
 * 2. Message Queues - Typed message passing with priorities
 *
 * Key-based identification (similar to ftok) allows processes to
 * find shared IPC objects by agreed-upon keys.
 */

// ============================================================================
// IPC Keys and IDs
// ============================================================================

typedef int32_t key_t;      // IPC key (user-defined identifier)
typedef int32_t ipc_id_t;   // IPC object ID (kernel-assigned)

// Special key value for private IPC objects
#define IPC_PRIVATE  ((key_t)0)

// IPC command flags (for shmctl, msgctl, etc.)
#define IPC_CREAT   0x0200  // Create if key doesn't exist
#define IPC_EXCL    0x0400  // Fail if key exists (with IPC_CREAT)
#define IPC_NOWAIT  0x0800  // Return error instead of blocking (msgsnd/msgrcv)
#define IPC_RMID    0x1000  // Remove identifier
#define IPC_STAT    0x2000  // Get status
#define IPC_SET     0x3000  // Set options

// Permission bits (octal for POSIX compatibility)
#define IPC_R   0400    // Read permission
#define IPC_W   0200    // Write permission

// ============================================================================
// Shared Memory
// ============================================================================

// Shared memory segment structure (kernel-internal)
typedef struct shm_segment {
    ipc_id_t id;                // Segment ID
    key_t key;                  // Key used to create segment
    void* phys_addr;            // Physical memory address
    size_t size;                // Segment size in bytes
    uint32_t pages;             // Number of pages allocated
    uint32_t attach_count;      // Number of attached processes
    uint32_t creator_uid;       // UID of creator
    uint32_t creator_gid;       // GID of creator
    uint32_t owner_pid;         // PID of creating process (for cleanup)
    uint32_t mode;              // Permission bits
    uint32_t pending_destroy;   // Non-zero: IPC_RMID called while attached;
                                //   free resources when attach_count reaches 0
    uint64_t create_time;       // Creation timestamp (ticks)
    uint64_t attach_time;       // Last attach timestamp
    uint64_t detach_time;       // Last detach timestamp
    struct shm_segment* next;   // Next segment in list
} shm_segment_t;

// Shared memory attachment structure (per-process)
typedef struct shm_attachment {
    ipc_id_t shm_id;            // Segment ID
    void* virt_addr;            // Virtual address in process space
    size_t size;                // Segment size
    uint32_t flags;             // Attachment flags
    struct shm_attachment* next;
} shm_attachment_t;

// Shared memory flags for shmat()
#define SHM_RDONLY  0x1000  // Attach read-only
#define SHM_RND     0x2000  // Round attach address to SHMLBA

// Shared memory alignment boundary
#define SHMLBA      4096    // Segment low boundary address (page size)

// Shared memory system calls (match syscall.h signatures)
int64_t sys_shmget(uint64_t key, uint64_t size, uint64_t shmflg,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_shmat(uint64_t shmid, uint64_t shmaddr, uint64_t shmflg,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_shmdt(uint64_t shmaddr, uint64_t arg2, uint64_t arg3,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_shmctl(uint64_t shmid, uint64_t cmd, uint64_t buf,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Kernel-internal functions
void shm_init(void);
shm_segment_t* shm_find_by_key(key_t key);
shm_segment_t* shm_find_by_id(ipc_id_t id);
struct process;
void shm_cleanup_process(struct process* proc);

// ============================================================================
// Message Queues
// ============================================================================

// Message structure (user-visible)
struct msgbuf {
    int64_t mtype;      // Message type (must be > 0)
    char mtext[1];      // Message data (variable length)
};

// Message queue message structure (kernel-internal)
typedef struct msg_message {
    int64_t mtype;              // Message type
    size_t msize;               // Message size (bytes)
    void* mtext;                // Message data
    struct msg_message* next;   // Next message in queue
} msg_message_t;

// Message queue structure (kernel-internal)
typedef struct msg_queue {
    ipc_id_t id;                // Queue ID
    key_t key;                  // Key used to create queue
    msg_message_t* first;       // First message in queue
    msg_message_t* last;        // Last message in queue
    uint32_t msg_count;         // Number of messages
    size_t total_bytes;         // Total bytes of all messages
    uint32_t max_msgs;          // Maximum number of messages
    size_t max_bytes;           // Maximum total bytes
    uint32_t creator_uid;       // UID of creator
    uint32_t creator_gid;       // GID of creator
    uint32_t owner_pid;         // PID of creating process (for cleanup)
    uint32_t mode;              // Permission bits
    uint64_t create_time;       // Creation timestamp
    uint64_t send_time;         // Last send timestamp
    uint64_t recv_time;         // Last receive timestamp
    struct msg_queue* next;     // Next queue in list
} msg_queue_t;

// Message queue flags for msgget()
#define MSG_NOERROR 0x1000  // Don't error on long messages (truncate)
#define MSG_COPY    0x2000  // Copy message without removing

// Message queue limits
#define MSGMAX      8192    // Max message size (bytes)
#define MSGMNB      16384   // Max bytes in queue
#define MSGMNI      256     // Max number of message queues

// Message queue system calls (match syscall.h signatures)
int64_t sys_msgget(uint64_t key, uint64_t msgflg, uint64_t arg3,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t sys_msgsnd(uint64_t msqid, uint64_t msgp, uint64_t msgsz,
                   uint64_t msgflg, uint64_t arg5, uint64_t arg6);
int64_t sys_msgrcv(uint64_t msqid, uint64_t msgp, uint64_t msgsz,
                   uint64_t msgtyp, uint64_t msgflg, uint64_t arg6);
int64_t sys_msgctl(uint64_t msqid, uint64_t cmd, uint64_t buf,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6);

// Kernel-internal functions
void msg_init(void);
msg_queue_t* msg_find_by_key(key_t key);
msg_queue_t* msg_find_by_id(ipc_id_t id);
void msg_cleanup_process(uint32_t pid);

// ============================================================================
// IPC Permissions
// ============================================================================

// IPC permission structure
typedef struct ipc_perm {
    uint32_t uid;       // Owner's user ID
    uint32_t gid;       // Owner's group ID
    uint32_t cuid;      // Creator's user ID
    uint32_t cgid;      // Creator's group ID
    uint32_t mode;      // Permission bits
    uint32_t seq;       // Sequence number
} ipc_perm_t;

// Check if process has permission to access IPC object
bool ipc_check_permission(ipc_perm_t* perm, uint32_t uid, uint32_t gid, int access_mode);

// ============================================================================
// IPC Namespace Integration
// ============================================================================

// IPC namespace table (referenced by namespace.h)
typedef struct ipc_table {
    shm_segment_t* shm_list;    // Linked list of shared memory segments
    msg_queue_t* msg_list;      // Linked list of message queues
    uint32_t next_shm_id;       // Next shared memory ID to allocate
    uint32_t next_msg_id;       // Next message queue ID to allocate
    void* lock;                 // Spinlock for thread safety
} ipc_table_t;

// Initialize IPC subsystem
void ipc_init(void);

// Create IPC table for a namespace
ipc_table_t* ipc_table_create(void);

// Destroy IPC table and all objects
void ipc_table_destroy(ipc_table_t* table);

// ============================================================================
// Error Codes (match syscall.h)
// ============================================================================

#define IPC_SUCCESS     0
#define IPC_ENOENT      -2   // No such IPC object
#define IPC_EINVAL      -22  // Invalid argument
#define IPC_EACCES      -13  // Permission denied
#define IPC_EEXIST      -17  // IPC object exists
#define IPC_ENOMEM      -12  // Out of memory
#define IPC_E2BIG       -7   // Message too long
#define IPC_EAGAIN      -11  // Resource temporarily unavailable
#define IPC_EIDRM       -43  // Identifier removed
#define IPC_EFAULT      -14  // Bad address
#define IPC_ENOMSG      -42  // No message of desired type

#endif // IPC_H
