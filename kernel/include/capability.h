#ifndef CAPABILITY_H
#define CAPABILITY_H

#include "types.h"

// Forward declaration
struct process;

// Capability types
typedef enum {
    CAP_NONE = 0,

    // File system capabilities (1-9)
    CAP_FILE_READ = 1,
    CAP_FILE_WRITE = 2,
    CAP_FILE_EXECUTE = 3,
    CAP_FILE_CREATE = 4,
    CAP_FILE_DELETE = 5,
    CAP_FILE_CHOWN = 6,
    CAP_FILE_CHMOD = 7,

    // Network capabilities (10-19)
    CAP_NET_BIND = 10,
    CAP_NET_CONNECT = 11,
    CAP_NET_RAW = 12,
    CAP_NET_LISTEN = 13,

    // Device capabilities (20-29)
    CAP_DEVICE_ACCESS = 20,
    CAP_GPU = 21,
    CAP_AUDIO = 22,
    CAP_USB = 23,
    CAP_SERIAL = 24,

    // IPC capabilities (30-39)
    CAP_IPC = 30,
    CAP_IPC_BROADCAST = 31,
    CAP_IPC_RECEIVE = 32,
    CAP_SHARED_MEM = 33,

    // System capabilities (40-49)
    CAP_SYS_ADMIN = 40,
    CAP_SYS_MODULE = 41,
    CAP_SYS_TIME = 42,
    CAP_SYS_BOOT = 43,
    CAP_SYS_PTRACE = 44,

    // Process capabilities (50-59)
    CAP_PROCESS_KILL = 50,
    CAP_PROCESS_TRACE = 51,
    CAP_PROCESS_SETUID = 52,
    CAP_PROCESS_SETGID = 53,
    CAP_PROCESS_NICE = 54,

    CAP_MAX = 64
} capability_type_t;

// Capability flags
#define CAP_FLAG_INHERITABLE   0x01  // Child processes inherit this capability
#define CAP_FLAG_DELEGATABLE   0x02  // Can be granted to other processes
#define CAP_FLAG_PERMANENT     0x04  // Cannot be revoked (root init only)
#define CAP_FLAG_AUDIT         0x08  // Log all uses of this capability
#define CAP_FLAG_TIME_LIMITED  0x10  // Expires after timeout

// Capability structure
typedef struct capability {
    capability_type_t type;
    uint64_t flags;
    uint32_t ref_count;

    // Type-specific data
    union {
        // File capabilities
        struct {
            char path_pattern[256];  // e.g., "/home/user/*"
        } file;

        // Network capabilities
        struct {
            char host[256];          // e.g., "*.example.com" or "*" for any
            uint16_t port_min;       // Port range minimum
            uint16_t port_max;       // Port range maximum (0 = same as min)
        } net;

        // Device capabilities
        struct {
            uint32_t device_id;      // Device ID or 0xFFFFFFFF for all
            char device_class[32];   // Device class name
        } device;

        // IPC capabilities
        struct {
            uint32_t target_pid;     // Target process or 0 for any
        } ipc;
    } data;

    struct capability* next;
} capability_t;

// Capability set for a process
typedef struct {
    capability_t* head;          // Linked list of capabilities
    uint32_t count;              // Number of capabilities
    uint64_t bitmask;            // Fast lookup for simple caps (types 0-63)
    uint32_t generation;         // Generation counter (for revocation)
} capability_set_t;

// Global capability system state
extern uint32_t global_capability_generation;

// Capability management functions
void capability_init(void);
capability_set_t* capability_set_create(void);
void capability_set_destroy(capability_set_t* set);
capability_set_t* capability_set_clone(capability_set_t* set);
void capability_set_refresh(capability_set_t* set);

// Capability modification
int capability_add(capability_set_t* set, capability_t* cap);
int capability_remove(capability_set_t* set, capability_type_t type);
int capability_grant(struct process* granter, struct process* grantee, capability_t* cap);
int capability_revoke(struct process* proc, capability_type_t type);
int capability_revoke_all(struct process* proc);

// Capability checking
bool capability_has(capability_set_t* set, capability_type_t type);
bool capability_check_file(capability_set_t* set, const char* path, capability_type_t access);
bool capability_check_net(capability_set_t* set, const char* host, uint16_t port, capability_type_t access);
bool capability_check_device(capability_set_t* set, uint32_t device_id);
bool capability_check_ipc(capability_set_t* set, uint32_t target_pid);

// Inheritance and delegation
capability_set_t* capability_inherit(capability_set_t* parent, uint64_t inherit_mask);
bool capability_can_delegate(capability_t* cap);
int capability_restrict(capability_t* cap, const char* pattern);

// Syscall capability checks
bool syscall_check_capability(int syscall_num, capability_set_t* caps, void* args);

// Helper functions for creating capabilities
capability_t* capability_create_file(capability_type_t type, const char* path_pattern, uint64_t flags);
capability_t* capability_create_net(capability_type_t type, const char* host, uint16_t port_min, uint16_t port_max, uint64_t flags);
capability_t* capability_create_device(uint32_t device_id, const char* device_class, uint64_t flags);
capability_t* capability_create_ipc(uint32_t target_pid, uint64_t flags);
capability_t* capability_create_simple(capability_type_t type, uint64_t flags);
void capability_destroy(capability_t* cap);

// Audit logging (forward declarations - implemented in audit.c)
typedef enum {
    AUDIT_CAP_GRANTED,
    AUDIT_CAP_REVOKED,
    AUDIT_CAP_DENIED,
    AUDIT_CAP_INHERITED,
    AUDIT_CAP_DELEGATED
} audit_event_t;

void audit_log_capability(struct process* proc, capability_type_t cap_type,
                         audit_event_t event, const char* details);

// Error codes
#define CAP_SUCCESS      0
#define CAP_ERROR       -1
#define CAP_ENOMEM      -2
#define CAP_EINVAL      -3
#define CAP_EPERM       -4
#define CAP_ENOTFOUND   -5
#define CAP_EDUP        -6

#endif
