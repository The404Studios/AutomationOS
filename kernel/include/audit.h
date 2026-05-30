#ifndef AUDIT_H
#define AUDIT_H

#include "types.h"

// Audit event types
typedef enum {
    // Authentication events
    AUDIT_AUTH_LOGIN = 1000,
    AUDIT_AUTH_LOGOUT = 1001,
    AUDIT_AUTH_SU = 1002,
    AUDIT_AUTH_SUDO = 1003,
    AUDIT_AUTH_FAILED = 1004,

    // File access events
    AUDIT_FILE_OPEN = 2000,
    AUDIT_FILE_READ = 2001,
    AUDIT_FILE_WRITE = 2002,
    AUDIT_FILE_DELETE = 2003,
    AUDIT_FILE_CHMOD = 2004,
    AUDIT_FILE_CHOWN = 2005,
    AUDIT_FILE_RENAME = 2006,

    // Process events
    AUDIT_PROC_EXEC = 3000,
    AUDIT_PROC_FORK = 3001,
    AUDIT_PROC_EXIT = 3002,
    AUDIT_PROC_KILL = 3003,
    AUDIT_PROC_SETUID = 3004,
    AUDIT_PROC_SETGID = 3005,

    // Network events
    AUDIT_NET_CONNECT = 4000,
    AUDIT_NET_BIND = 4001,
    AUDIT_NET_LISTEN = 4002,
    AUDIT_NET_ACCEPT = 4003,
    AUDIT_NET_SEND = 4004,
    AUDIT_NET_RECV = 4005,

    // Security violations
    AUDIT_SECURITY_CAP_DENIED = 5000,
    AUDIT_SECURITY_MAC_DENIED = 5001,
    AUDIT_SECURITY_SANDBOX_VIOLATION = 5002,
    AUDIT_SECURITY_INVALID_SYSCALL = 5003,
    AUDIT_SECURITY_PRIVILEGE_ESCALATION = 5004,

    // Configuration changes
    AUDIT_CONFIG_POLICY_RELOAD = 6000,
    AUDIT_CONFIG_USER_ADD = 6001,
    AUDIT_CONFIG_USER_DELETE = 6002,
    AUDIT_CONFIG_GROUP_ADD = 6003,
    AUDIT_CONFIG_GROUP_DELETE = 6004,
    AUDIT_CONFIG_CAPABILITY_GRANT = 6005,
    AUDIT_CONFIG_CAPABILITY_REVOKE = 6006,

    // Kernel events
    AUDIT_KERNEL_MODULE_LOAD = 7000,
    AUDIT_KERNEL_MODULE_UNLOAD = 7001,
    AUDIT_KERNEL_PANIC = 7002,
    AUDIT_KERNEL_CRASH = 7003,
    AUDIT_KERNEL_BOOT = 7004,

    // System events
    AUDIT_SYSTEM_SHUTDOWN = 8000,
    AUDIT_SYSTEM_REBOOT = 8001,
    AUDIT_SYSTEM_TIME_CHANGE = 8002,

    AUDIT_EVENT_MAX = 9999
} audit_event_type_t;

// Result codes
typedef enum {
    AUDIT_SUCCESS = 0,
    AUDIT_FAILURE = 1,
    AUDIT_DENIED = 2,
    AUDIT_ERROR = 3
} audit_result_t;

// Audit event structure (fixed size for ring buffer efficiency)
#define AUDIT_PATH_MAX 256
#define AUDIT_COMM_MAX 64
#define AUDIT_DATA_MAX 128

typedef struct audit_event {
    // Event metadata
    uint64_t timestamp;              // Nanoseconds since boot
    uint64_t sequence;               // Monotonic sequence number
    audit_event_type_t type;         // Event type
    audit_result_t result;           // Success/failure

    // Subject (who performed the action)
    uint32_t pid;                    // Process ID
    uint32_t uid;                    // User ID
    uint32_t gid;                    // Group ID
    char comm[AUDIT_COMM_MAX];       // Process name/command

    // Object (what was affected)
    char path[AUDIT_PATH_MAX];       // File path, device, etc.
    uint32_t object_pid;             // Target process (for kill, etc.)
    uint32_t object_uid;             // Target user

    // Operation details
    uint32_t syscall;                // Syscall number (if applicable)
    int32_t error_code;              // Error code (errno)
    uint64_t flags;                  // Operation-specific flags

    // Additional data (type-specific)
    uint8_t data[AUDIT_DATA_MAX];    // Extra context data

    // Integrity (hash chain for tamper detection)
    uint64_t prev_hash;              // Hash of previous event
    uint64_t hash;                   // Hash of this event
} audit_event_t;

// Audit ring buffer structure
#define AUDIT_BUFFER_SIZE 8192       // Number of events in ring buffer

typedef struct audit_buffer {
    audit_event_t* events;           // Ring buffer array
    uint32_t head;                   // Write position
    uint32_t tail;                   // Read position
    uint32_t count;                  // Number of events
    uint64_t dropped;                // Count of dropped events (overflow)
    uint64_t sequence;               // Global sequence number
    uint64_t last_hash;              // Hash of last event (for chain)
    uint32_t lock;                   // Spinlock for thread safety
} audit_buffer_t;

// Audit filter rule
typedef enum {
    AUDIT_FILTER_TYPE,               // Filter by event type
    AUDIT_FILTER_UID,                // Filter by user ID
    AUDIT_FILTER_PID,                // Filter by process ID
    AUDIT_FILTER_SYSCALL,            // Filter by syscall number
    AUDIT_FILTER_PATH,               // Filter by path pattern
    AUDIT_FILTER_RESULT              // Filter by result (success/failure)
} audit_filter_type_t;

typedef enum {
    AUDIT_ACTION_LOG,                // Log this event
    AUDIT_ACTION_IGNORE,             // Ignore this event
    AUDIT_ACTION_ALERT               // Log and trigger alert
} audit_action_t;

typedef struct audit_rule {
    uint32_t id;                     // Rule ID
    audit_filter_type_t filter_type; // What to filter on
    audit_action_t action;           // What to do

    // Filter criteria (union for type-specific data)
    union {
        audit_event_type_t event_type;
        uint32_t uid;
        uint32_t pid;
        uint32_t syscall;
        char path_pattern[AUDIT_PATH_MAX];
        audit_result_t result;
    } criteria;

    bool enabled;                    // Rule enabled/disabled
    struct audit_rule* next;         // Next rule in list
} audit_rule_t;

// Audit configuration
typedef struct audit_config {
    bool enabled;                    // Global audit enable/disable
    bool log_successful;             // Log successful operations
    bool log_failed;                 // Log failed operations
    bool tamper_detection;           // Enable hash chain
    uint32_t rate_limit;             // Max events/second (0 = unlimited)
    char log_file[256];              // Disk log file path
    bool log_to_disk;                // Enable disk logging
    uint32_t log_rotation_size;      // Size in MB before rotation
} audit_config_t;

// Core audit functions
void audit_init(void);
void audit_shutdown(void);
int audit_log(audit_event_type_t type, audit_result_t result,
              uint32_t pid, uint32_t uid, const char* path,
              uint32_t syscall, int32_t error_code);
int audit_log_event(audit_event_t* event);

// Buffer management
audit_buffer_t* audit_buffer_create(void);
void audit_buffer_destroy(audit_buffer_t* buffer);
int audit_buffer_write(audit_buffer_t* buffer, audit_event_t* event);
int audit_buffer_read(audit_buffer_t* buffer, audit_event_t* event);
uint32_t audit_buffer_count(audit_buffer_t* buffer);
bool audit_buffer_is_full(audit_buffer_t* buffer);

// Rule management
void audit_rules_init(void);
int audit_rule_add(audit_rule_t* rule);
int audit_rule_delete(uint32_t rule_id);
audit_rule_t* audit_rule_find(uint32_t rule_id);
bool audit_rule_matches(audit_rule_t* rule, audit_event_t* event);
audit_action_t audit_rules_evaluate(audit_event_t* event);

// Filter functions
void audit_filter_init(void);
bool audit_filter_should_log(audit_event_t* event);

// Configuration
void audit_config_init(void);
int audit_config_set(audit_config_t* config);
audit_config_t* audit_config_get(void);

// Syscall interface
int sys_audit_enable(void);
int sys_audit_disable(void);
int sys_audit_read(audit_event_t* events, uint32_t count);
int sys_audit_rule_add(audit_rule_t* rule);
int sys_audit_rule_del(uint32_t rule_id);
int sys_audit_get_stats(uint64_t* total, uint64_t* dropped);

// Utility functions
uint64_t audit_get_timestamp(void);
uint64_t audit_hash_event(audit_event_t* event);
void audit_rotate_log(void);
int audit_write_to_disk(audit_event_t* event);

// Helper macros for common audit operations
#define AUDIT_LOG_FILE_ACCESS(path, result) \
    audit_log(AUDIT_FILE_OPEN, result, current->pid, current->uid, \
              path, 0, 0)

#define AUDIT_LOG_CAPABILITY_DENIED(cap_type) \
    audit_log(AUDIT_SECURITY_CAP_DENIED, AUDIT_DENIED, \
              current->pid, current->uid, NULL, 0, cap_type)

#define AUDIT_LOG_SYSCALL(syscall_num, result, error) \
    audit_log(AUDIT_PROC_EXEC, result, current->pid, current->uid, \
              NULL, syscall_num, error)

#define AUDIT_LOG_MAC_DENIED(path, label) \
    audit_log(AUDIT_SECURITY_MAC_DENIED, AUDIT_DENIED, \
              current->pid, current->uid, path, 0, 0)

// Performance monitoring
typedef struct audit_stats {
    uint64_t total_events;           // Total events logged
    uint64_t events_dropped;         // Events dropped (buffer full)
    uint64_t events_filtered;        // Events filtered out
    uint64_t disk_writes;            // Events written to disk
    uint64_t disk_errors;            // Disk write errors
    uint64_t hash_collisions;        // Hash collisions detected
    uint64_t rule_evaluations;       // Rule evaluations performed
} audit_stats_t;

extern audit_stats_t audit_stats;

// Get audit statistics
audit_stats_t* audit_get_stats(void);
void audit_reset_stats(void);

#endif
