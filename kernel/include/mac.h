#ifndef MAC_H
#define MAC_H

#include "types.h"

// Forward declarations
struct process;

// ============================================================================
// Security Label Definitions
// ============================================================================

#define MAX_LABEL_NAME 64
#define MAX_CATEGORIES 32

// Security label types
typedef enum {
    LABEL_TYPE_UNCONFINED = 0,  // No restrictions
    LABEL_TYPE_SYSTEM,          // System services
    LABEL_TYPE_USER,            // User processes
    LABEL_TYPE_UNTRUSTED,       // Untrusted/sandboxed
    LABEL_TYPE_ISOLATED,        // Fully isolated
    LABEL_TYPE_CUSTOM           // Custom domain
} label_type_t;

// Multi-Level Security (MLS) levels
typedef enum {
    MLS_LEVEL_UNCLASSIFIED = 0,
    MLS_LEVEL_CONFIDENTIAL = 1,
    MLS_LEVEL_SECRET = 2,
    MLS_LEVEL_TOP_SECRET = 3
} mls_level_t;

// Security label for processes and objects
typedef struct security_label {
    char domain[MAX_LABEL_NAME];        // Security domain (e.g., "web_server_t")
    label_type_t type;                   // Label type
    mls_level_t level;                   // MLS security level
    uint32_t categories[MAX_CATEGORIES]; // Compartments/categories (bitmap)
    uint32_t category_count;
    uint64_t flags;                      // Additional flags
} security_label_t;

// Label flags
#define LABEL_FLAG_PRIVILEGED   (1 << 0)  // Can manage labels
#define LABEL_FLAG_AUDIT        (1 << 1)  // Audit all accesses
#define LABEL_FLAG_ENFORCING    (1 << 2)  // Enforcing mode (vs permissive)
#define LABEL_FLAG_TRANSITION   (1 << 3)  // Can transition domains

// ============================================================================
// Object Types for MAC Policy
// ============================================================================

typedef enum {
    OBJ_TYPE_FILE = 0,
    OBJ_TYPE_DIR,
    OBJ_TYPE_SOCKET,
    OBJ_TYPE_DEVICE,
    OBJ_TYPE_PROCESS,
    OBJ_TYPE_IPC_SHM,
    OBJ_TYPE_IPC_MSG,
    OBJ_TYPE_IPC_SEM,
    OBJ_TYPE_MAX
} object_type_t;

// ============================================================================
// Access Permissions
// ============================================================================

// File permissions
#define MAC_FILE_READ       (1 << 0)
#define MAC_FILE_WRITE      (1 << 1)
#define MAC_FILE_EXECUTE    (1 << 2)
#define MAC_FILE_APPEND     (1 << 3)
#define MAC_FILE_CREATE     (1 << 4)
#define MAC_FILE_DELETE     (1 << 5)
#define MAC_FILE_CHOWN      (1 << 6)
#define MAC_FILE_CHMOD      (1 << 7)

// Network permissions
#define MAC_NET_BIND        (1 << 0)
#define MAC_NET_CONNECT     (1 << 1)
#define MAC_NET_LISTEN      (1 << 2)
#define MAC_NET_ACCEPT      (1 << 3)
#define MAC_NET_SEND        (1 << 4)
#define MAC_NET_RECV        (1 << 5)
#define MAC_NET_RAW         (1 << 6)

// Process permissions
#define MAC_PROC_SIGNAL     (1 << 0)
#define MAC_PROC_PTRACE     (1 << 1)
#define MAC_PROC_KILL       (1 << 2)
#define MAC_PROC_SETPRIO    (1 << 3)
#define MAC_PROC_FORK       (1 << 4)
#define MAC_PROC_EXEC       (1 << 5)
#define MAC_PROC_TRANSITION (1 << 6)

// IPC permissions
#define MAC_IPC_READ        (1 << 0)
#define MAC_IPC_WRITE       (1 << 1)
#define MAC_IPC_CREATE      (1 << 2)
#define MAC_IPC_DESTROY     (1 << 3)
#define MAC_IPC_GETATTR     (1 << 4)
#define MAC_IPC_SETATTR     (1 << 5)

// ============================================================================
// MAC Policy Rules
// ============================================================================

#define MAX_POLICY_RULES 4096

// Policy rule structure
typedef struct mac_rule {
    char source_domain[MAX_LABEL_NAME];     // Source domain/type
    char target_domain[MAX_LABEL_NAME];     // Target domain/type
    object_type_t object_type;               // Object type
    uint32_t permissions;                    // Allowed permissions
    mls_level_t min_level;                   // Minimum security level
    mls_level_t max_level;                   // Maximum security level
    uint32_t flags;                          // Rule flags
    struct mac_rule* next;                   // Next rule in chain
} mac_rule_t;

// Rule flags
#define RULE_FLAG_AUDIT      (1 << 0)  // Audit this rule
#define RULE_FLAG_DENY       (1 << 1)  // Explicit deny (overrides allow)
#define RULE_FLAG_TEMPORARY  (1 << 2)  // Temporary rule (runtime only)

// Policy structure
typedef struct mac_policy {
    mac_rule_t* rules[MAX_POLICY_RULES];    // Hash table of rules
    uint32_t rule_count;
    uint64_t version;                        // Policy version
    bool enforcing;                          // Enforcing mode
    uint32_t flags;
} mac_policy_t;

// Policy flags
#define POLICY_FLAG_DEFAULT_DENY   (1 << 0)  // Default deny (mandatory)
#define POLICY_FLAG_AUDIT_DENIALS  (1 << 1)  // Audit all denials
#define POLICY_FLAG_AUDIT_ALLOWED  (1 << 2)  // Audit allowed accesses
#define POLICY_FLAG_MLS_ENABLED    (1 << 3)  // Enable MLS checks

// ============================================================================
// Type Transitions (Domain transitions on exec)
// ============================================================================

#define MAX_TRANSITIONS 1024

typedef struct mac_transition {
    char source_domain[MAX_LABEL_NAME];     // Source process domain
    char target_domain[MAX_LABEL_NAME];     // Target file domain/type
    char result_domain[MAX_LABEL_NAME];     // Resulting process domain
    char path_pattern[256];                  // Executable path pattern
    uint32_t flags;
    struct mac_transition* next;
} mac_transition_t;

// ============================================================================
// Audit Events
// ============================================================================

typedef enum {
    MAC_AUDIT_ALLOWED = 0,
    MAC_AUDIT_DENIED,
    MAC_AUDIT_LABEL_CHANGE,
    MAC_AUDIT_POLICY_LOAD,
    MAC_AUDIT_TRANSITION
} mac_audit_type_t;

typedef struct mac_audit_event {
    mac_audit_type_t type;
    uint64_t timestamp;
    uint32_t pid;
    security_label_t subject;
    security_label_t object;
    object_type_t obj_type;
    uint32_t requested_perms;
    uint32_t denied_perms;
    char path[256];
    char message[512];
} mac_audit_event_t;

// ============================================================================
// Core MAC Functions
// ============================================================================

// Initialization
void mac_init(void);
void mac_set_enforcing(bool enforcing);
bool mac_is_enforcing(void);

// Label Management
security_label_t* mac_label_create(const char* domain, label_type_t type, mls_level_t level);
void mac_label_destroy(security_label_t* label);
security_label_t* mac_label_copy(const security_label_t* label);
int mac_label_add_category(security_label_t* label, uint32_t category);
int mac_label_remove_category(security_label_t* label, uint32_t category);
bool mac_label_has_category(const security_label_t* label, uint32_t category);
int mac_label_compare(const security_label_t* l1, const security_label_t* l2);
bool mac_label_dominates(const security_label_t* l1, const security_label_t* l2);

// Process Label Operations
int mac_process_set_label(struct process* proc, const security_label_t* label);
security_label_t* mac_process_get_label(const struct process* proc);
int mac_process_transition(struct process* proc, const char* exec_path);

// Policy Management
int mac_policy_load(const void* policy_data, size_t size);
int mac_policy_add_rule(const mac_rule_t* rule);
int mac_policy_remove_rule(const char* source, const char* target, object_type_t type);
mac_rule_t* mac_policy_find_rule(const char* source, const char* target, object_type_t type);
void mac_policy_clear(void);
uint64_t mac_policy_get_version(void);

// Transition Management
int mac_transition_add(const mac_transition_t* trans);
mac_transition_t* mac_transition_find(const char* source_domain, const char* target_path);
void mac_transition_clear(void);

// ============================================================================
// Policy Enforcement Hooks
// ============================================================================

// File access checks
int mac_check_file_open(const struct process* proc, const char* path, uint32_t mode);
int mac_check_file_read(const struct process* proc, const char* path);
int mac_check_file_write(const struct process* proc, const char* path);
int mac_check_file_execute(const struct process* proc, const char* path);
int mac_check_file_create(const struct process* proc, const char* path);
int mac_check_file_delete(const struct process* proc, const char* path);
int mac_check_file_chmod(const struct process* proc, const char* path);
int mac_check_file_chown(const struct process* proc, const char* path);

// Network access checks
int mac_check_net_bind(const struct process* proc, uint16_t port);
int mac_check_net_connect(const struct process* proc, const char* host, uint16_t port);
int mac_check_net_listen(const struct process* proc, uint16_t port);
int mac_check_net_raw(const struct process* proc);

// Process access checks
int mac_check_process_signal(const struct process* source, const struct process* target);
int mac_check_process_ptrace(const struct process* source, const struct process* target);
int mac_check_process_kill(const struct process* source, const struct process* target);
int mac_check_process_fork(const struct process* proc);
int mac_check_process_exec(const struct process* proc, const char* path);

// IPC access checks
int mac_check_ipc_create(const struct process* proc, object_type_t ipc_type);
int mac_check_ipc_access(const struct process* proc, uint32_t ipc_id, uint32_t perms);

// System checks
int mac_check_load_module(const struct process* proc, const char* module_path);
int mac_check_set_time(const struct process* proc);
int mac_check_reboot(const struct process* proc);
int mac_check_mount(const struct process* proc, const char* dev, const char* path);

// ============================================================================
// Audit Functions
// ============================================================================

void mac_audit_init(void);
void mac_audit_log(const mac_audit_event_t* event);
void mac_audit_access(const struct process* proc, const char* path,
                      object_type_t type, uint32_t perms, bool allowed);
void mac_audit_denial(const struct process* proc, const char* path,
                      object_type_t type, uint32_t denied_perms);
void mac_audit_label_change(const struct process* proc, const security_label_t* old_label,
                            const security_label_t* new_label);
void mac_audit_policy_load(uint64_t version, uint32_t rule_count);
void mac_audit_transition(const struct process* proc, const char* from_domain,
                          const char* to_domain, const char* exec_path);
uint32_t mac_audit_get_count(void);
int mac_audit_get_events(mac_audit_event_t* buffer, uint32_t count);

// ============================================================================
// System Calls
// ============================================================================

// User-space MAC syscalls
int sys_mac_load_policy(const void* policy_data, size_t size);
int sys_mac_get_label(uint32_t pid, security_label_t* label);
int sys_mac_set_label(uint32_t pid, const security_label_t* label);
int sys_mac_check_access(const char* path, uint32_t perms);
int sys_mac_set_enforcing(bool enforcing);
int sys_mac_get_stats(void* stats_buffer, size_t size);

// ============================================================================
// Helper Functions
// ============================================================================

// Domain name validation
bool mac_is_valid_domain(const char* domain);
bool mac_is_privileged_domain(const char* domain);

// MLS checks
bool mac_mls_range_valid(mls_level_t low, mls_level_t high);
bool mac_mls_can_read(mls_level_t subject_level, mls_level_t object_level);
bool mac_mls_can_write(mls_level_t subject_level, mls_level_t object_level);

// Permission conversion
const char* mac_perm_to_string(object_type_t type, uint32_t perm);
const char* mac_object_type_to_string(object_type_t type);

// Statistics
typedef struct mac_stats {
    uint64_t checks_total;
    uint64_t checks_allowed;
    uint64_t checks_denied;
    uint64_t label_changes;
    uint64_t policy_loads;
    uint64_t transitions;
    uint64_t cache_hits;
    uint64_t cache_misses;
} mac_stats_t;

void mac_stats_get(mac_stats_t* stats);
void mac_stats_reset(void);

// ============================================================================
// Default Security Contexts
// ============================================================================

// Default domain names
#define MAC_DOMAIN_KERNEL       "kernel_t"
#define MAC_DOMAIN_INIT         "init_t"
#define MAC_DOMAIN_USER         "user_t"
#define MAC_DOMAIN_UNTRUSTED    "untrusted_t"
#define MAC_DOMAIN_ISOLATED     "isolated_t"

// Default file types
#define MAC_TYPE_FILE           "file_t"
#define MAC_TYPE_ETC            "etc_t"
#define MAC_TYPE_SHADOW         "shadow_t"
#define MAC_TYPE_BIN            "bin_t"
#define MAC_TYPE_LIB            "lib_t"
#define MAC_TYPE_DEV            "dev_t"
#define MAC_TYPE_TMP            "tmp_t"
#define MAC_TYPE_HOME           "home_t"

// Network types
#define MAC_TYPE_PORT_HTTP      "http_port_t"
#define MAC_TYPE_PORT_SSH       "ssh_port_t"
#define MAC_TYPE_PORT_RESERVED  "reserved_port_t"

// Error codes
#define MAC_SUCCESS             0
#define MAC_ERR_DENIED          -1
#define MAC_ERR_INVALID_LABEL   -2
#define MAC_ERR_INVALID_POLICY  -3
#define MAC_ERR_NO_MEMORY       -4
#define MAC_ERR_NOT_FOUND       -5
#define MAC_ERR_PERMISSION      -6
#define MAC_ERR_INVALID_ARG     -7

#endif
