#ifndef NAMESPACE_H
#define NAMESPACE_H

#include "types.h"

// Namespace types
typedef enum {
    NS_PID = 0,      // Process ID namespace
    NS_MOUNT = 1,    // Mount namespace
    NS_NET = 2,      // Network namespace
    NS_IPC = 3,      // IPC namespace (shared memory, semaphores, message queues)
    NS_UTS = 4,      // Hostname/domain namespace
    NS_MAX = 5
} namespace_type_t;

// Forward declarations
struct process;
struct mount_table;
struct network_stack;
struct ipc_table;

// PID namespace - isolates process ID space
// Each PID namespace has its own process tree starting from PID 1
// Processes can only see PIDs in their namespace and children namespaces
typedef struct pid_namespace {
    uint32_t id;                        // Unique namespace ID
    uint32_t next_pid;                  // Next PID to allocate
    struct process** process_table;     // Per-namespace process table (max 1024)
    uint32_t process_count;             // Number of processes in this namespace
    struct pid_namespace* parent;       // Parent namespace (for nested containers)
    uint32_t ref_count;                 // Reference count for lifetime management
    uint32_t level;                     // Nesting level (0 = root)
    void* lock;                         // Spinlock protecting namespace operations (RACE-006 fix)
} pid_namespace_t;

// Mount namespace - isolates filesystem mount points
// Each mount namespace has its own view of the filesystem tree
// Changes to mounts in one namespace don't affect others
typedef struct mount_namespace {
    uint32_t id;                        // Unique namespace ID
    struct mount_table* mounts;         // Mount table (will be implemented in VFS)
    char root_path[256];                // Root directory for this namespace
    uint32_t flags;                     // Mount namespace flags
    uint32_t ref_count;                 // Reference count
} mount_namespace_t;

// Network namespace - isolates network stack
// Each network namespace has its own network devices, IP addresses, routing tables
typedef struct net_namespace {
    uint32_t id;                        // Unique namespace ID
    struct network_stack* stack;        // Network stack (will be implemented in net/)
    uint32_t ref_count;                 // Reference count
} net_namespace_t;

// IPC namespace - isolates inter-process communication
// Each IPC namespace has its own System V IPC objects:
// - Shared memory segments
// - Semaphore arrays
// - Message queues
typedef struct ipc_namespace {
    uint32_t id;                        // Unique namespace ID
    struct ipc_table* table;            // IPC object table (will be implemented in ipc/)
    uint32_t ref_count;                 // Reference count
} ipc_namespace_t;

// UTS namespace - isolates hostname and domain name
// Allows each container to have its own hostname without affecting others
typedef struct uts_namespace {
    uint32_t id;                        // Unique namespace ID
    char hostname[256];                 // System hostname
    char domainname[256];               // System domain name
    uint32_t ref_count;                 // Reference count
} uts_namespace_t;

// Namespace container - holds all namespace types for a process
// A process belongs to one namespace of each type
typedef struct namespace_container {
    pid_namespace_t* pid_ns;
    mount_namespace_t* mount_ns;
    net_namespace_t* net_ns;
    ipc_namespace_t* ipc_ns;
    uts_namespace_t* uts_ns;
} namespace_container_t;

// Namespace flags for cloning (used in clone/unshare syscalls)
#define CLONE_NEWPID   (1 << 0)  // Create new PID namespace
#define CLONE_NEWMOUNT (1 << 1)  // Create new mount namespace
#define CLONE_NEWNET   (1 << 2)  // Create new network namespace
#define CLONE_NEWIPC   (1 << 3)  // Create new IPC namespace
#define CLONE_NEWUTS   (1 << 4)  // Create new UTS namespace

// Mount namespace flags
#define MNT_NS_SHARED  (1 << 0)  // Share mounts with parent
#define MNT_NS_PRIVATE (1 << 1)  // Private mounts (copy-on-write)

// Global namespace initialization
void namespace_init(void);

// Namespace container management
namespace_container_t* namespace_create_container(uint32_t flags);
void namespace_destroy_container(namespace_container_t* ns);
namespace_container_t* namespace_clone_container(namespace_container_t* parent, uint32_t flags);
namespace_container_t* namespace_get_root(void);

// PID namespace functions
pid_namespace_t* pid_namespace_create(pid_namespace_t* parent);
void pid_namespace_destroy(pid_namespace_t* ns);
uint32_t pid_namespace_alloc_pid(pid_namespace_t* ns, struct process* proc);
void pid_namespace_free_pid(pid_namespace_t* ns, uint32_t pid);
struct process* pid_namespace_find_process(pid_namespace_t* ns, uint32_t pid);
uint32_t pid_namespace_translate(pid_namespace_t* from, pid_namespace_t* to, uint32_t pid);

// Mount namespace functions
mount_namespace_t* mount_namespace_create(void);
void mount_namespace_destroy(mount_namespace_t* ns);
mount_namespace_t* mount_namespace_clone(mount_namespace_t* parent);

// Network namespace functions
net_namespace_t* net_namespace_create(void);
void net_namespace_destroy(net_namespace_t* ns);

// IPC namespace functions
ipc_namespace_t* ipc_namespace_create(void);
void ipc_namespace_destroy(ipc_namespace_t* ns);

// UTS namespace functions
uts_namespace_t* uts_namespace_create(void);
void uts_namespace_destroy(uts_namespace_t* ns);
int uts_namespace_set_hostname(uts_namespace_t* ns, const char* hostname);
int uts_namespace_set_domainname(uts_namespace_t* ns, const char* domainname);

// Namespace entry/exit (for setns syscall)
int namespace_enter(struct process* proc, namespace_type_t type, uint32_t ns_id);
int namespace_unshare(struct process* proc, uint32_t flags);

#endif
