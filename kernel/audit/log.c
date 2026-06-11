#include "../include/audit.h"
#include "../include/kernel.h"
#include "../include/mem.h"
#include "../include/sched.h"

extern void* kmalloc(size_t size);
extern void kfree(void* ptr);
extern void* memset(void* ptr, int value, size_t size);
extern void* memcpy(void* dest, const void* src, size_t n);
extern size_t strlen(const char* str);
extern char* strncpy(char* dest, const char* src, size_t n);
extern process_t* process_get_current(void);

// Global audit state
static audit_buffer_t* global_audit_buffer = NULL;
static audit_config_t global_audit_config;
audit_stats_t audit_stats;

// Rate limiting state (volatile for atomic operations)
static volatile uint64_t last_rate_check = 0;
static volatile uint32_t events_this_second = 0;

// Forward declarations
extern int audit_buffer_write(audit_buffer_t* buffer, audit_event_t* event);
extern uint32_t audit_buffer_count(audit_buffer_t* buffer);

void audit_init(void) {
    kprintf("[AUDIT] Initializing audit logging subsystem...\n");

    // Create ring buffer
    global_audit_buffer = audit_buffer_create();
    if (!global_audit_buffer) {
        kprintf("[AUDIT] CRITICAL: Failed to create audit buffer\n");
        return;
    }

    // Initialize configuration
    audit_config_init();

    // Initialize rules
    audit_rules_init();

    // Initialize filter
    audit_filter_init();

    // Reset statistics
    memset(&audit_stats, 0, sizeof(audit_stats_t));

    kprintf("[AUDIT] Audit logging initialized\n");

    // Log kernel boot event
    audit_log(AUDIT_KERNEL_BOOT, AUDIT_SUCCESS, 0, 0, NULL, 0, 0);
}

void audit_shutdown(void) {
    if (!global_audit_config.enabled) {
        return;
    }

    kprintf("[AUDIT] Shutting down audit subsystem...\n");

    // Log shutdown event
    audit_log(AUDIT_SYSTEM_SHUTDOWN, AUDIT_SUCCESS, 0, 0, NULL, 0, 0);

    // Flush any pending events to disk
    if (global_audit_config.log_to_disk && global_audit_buffer) {
        uint32_t count = audit_buffer_count(global_audit_buffer);
        kprintf("[AUDIT] Flushing %u pending events to disk...\n", count);

        audit_event_t event;
        while (audit_buffer_read(global_audit_buffer, &event) == 0) {
            audit_write_to_disk(&event);
        }
    }

    // Destroy buffer
    if (global_audit_buffer) {
        audit_buffer_destroy(global_audit_buffer);
        global_audit_buffer = NULL;
    }

    kprintf("[AUDIT] Audit subsystem shut down\n");
}

int audit_log(audit_event_type_t type, audit_result_t result,
              uint32_t pid, uint32_t uid, const char* path,
              uint32_t syscall, int32_t error_code) {
    if (!global_audit_config.enabled) {
        return 0;  // Auditing disabled
    }

    // Check rate limit
    uint64_t now = audit_get_timestamp();
    if (global_audit_config.rate_limit > 0) {
        uint64_t prev = __atomic_load_n(&last_rate_check, __ATOMIC_ACQUIRE);
        if ((now - prev) > 1000000000ULL) {  // 1 second
            // Only the CPU that wins the CAS resets the counter; others skip.
            if (__atomic_compare_exchange_n(&last_rate_check, &prev, now,
                                            false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&events_this_second, 0, __ATOMIC_RELEASE);
            }
        }

        uint32_t n = __atomic_add_fetch(&events_this_second, 1, __ATOMIC_ACQ_REL);
        if (n > global_audit_config.rate_limit) {
            __atomic_add_fetch(&audit_stats.events_filtered, 1, __ATOMIC_RELAXED);
            return -1;  // Rate limit exceeded
        }
    }

    // Create event
    audit_event_t event;
    memset(&event, 0, sizeof(audit_event_t));

    event.timestamp = now;
    event.type = type;
    event.result = result;
    event.pid = pid;
    event.uid = uid;
    event.syscall = syscall;
    event.error_code = error_code;

    // Get current process info if pid is 0
    process_t* current = process_get_current();
    if (current && pid == 0) {
        event.pid = current->pid;
        strncpy(event.comm, current->name, AUDIT_COMM_MAX - 1);
        event.comm[AUDIT_COMM_MAX - 1] = '\0';
    }

    // Copy path if provided
    if (path) {
        strncpy(event.path, path, AUDIT_PATH_MAX - 1);
        event.path[AUDIT_PATH_MAX - 1] = '\0';
    }

    // Log the event
    return audit_log_event(&event);
}

int audit_log_event(audit_event_t* event) {
    if (!event || !global_audit_buffer) {
        return -1;
    }

    // Filter event
    if (!audit_filter_should_log(event)) {
        __atomic_add_fetch(&audit_stats.events_filtered, 1, __ATOMIC_RELAXED);
        return 0;
    }

    // Evaluate rules
    audit_action_t action = audit_rules_evaluate(event);
    if (action == AUDIT_ACTION_IGNORE) {
        __atomic_add_fetch(&audit_stats.events_filtered, 1, __ATOMIC_RELAXED);
        return 0;
    }

    // Write to ring buffer
    int result = audit_buffer_write(global_audit_buffer, event);
    if (result != 0) {
        __atomic_add_fetch(&audit_stats.events_dropped, 1, __ATOMIC_RELAXED);
        return -1;
    }

    __atomic_add_fetch(&audit_stats.total_events, 1, __ATOMIC_RELAXED);

    // Write to disk if enabled
    if (global_audit_config.log_to_disk) {
        if (audit_write_to_disk(event) == 0) {
            __atomic_add_fetch(&audit_stats.disk_writes, 1, __ATOMIC_RELAXED);
        } else {
            __atomic_add_fetch(&audit_stats.disk_errors, 1, __ATOMIC_RELAXED);
        }
    }

    // Handle alert action
    if (action == AUDIT_ACTION_ALERT) {
        kprintf("[AUDIT ALERT] Type: %d, PID: %u, UID: %u, Path: %s, Result: %d\n",
                event->type, event->pid, event->uid, event->path, event->result);
    }

    return 0;
}

// Simple hash function for tamper detection (FNV-1a)
uint64_t audit_hash_event(audit_event_t* event) {
    if (!event) return 0;

    uint64_t hash = 0xcbf29ce484222325ULL;  // FNV offset basis
    const uint64_t fnv_prime = 0x100000001b3ULL;

    // Hash all fields except prev_hash and hash
    uint8_t* data = (uint8_t*)event;
    size_t size = sizeof(audit_event_t) - sizeof(uint64_t) * 2;  // Exclude hash fields

    for (size_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= fnv_prime;
    }

    return hash;
}

// Get timestamp (nanoseconds since boot)
// TODO: Replace with actual timer implementation
uint64_t audit_get_timestamp(void) {
    // Placeholder: Use TSC or PIT timer
    static uint64_t fake_time = 0;
    return fake_time++;
}

// Write event to disk (stub for now, needs VFS)
int audit_write_to_disk(audit_event_t* event) {
    if (!event || !global_audit_config.log_to_disk) {
        return -1;
    }

    // TODO: Implement actual disk I/O when VFS is available
    // For now, just return success
    // Format: JSON or binary format

    return 0;
}

// Rotate log file when it reaches configured size
void audit_rotate_log(void) {
    if (!global_audit_config.log_to_disk) {
        return;
    }

    kprintf("[AUDIT] Rotating log file...\n");

    // TODO: Implement log rotation
    // 1. Close current log file
    // 2. Rename to audit.log.1, audit.log.2, etc.
    // 3. Compress old logs
    // 4. Delete oldest logs beyond retention policy
    // 5. Open new log file
}

// Configuration management
void audit_config_init(void) {
    global_audit_config.enabled = true;
    global_audit_config.log_successful = false;  // Don't log successful ops by default
    global_audit_config.log_failed = true;       // Log failures
    global_audit_config.tamper_detection = true; // Enable hash chain
    global_audit_config.rate_limit = 10000;      // Max 10k events/second
    global_audit_config.log_to_disk = false;     // Disabled until VFS available
    global_audit_config.log_rotation_size = 100; // 100 MB

    strncpy(global_audit_config.log_file, "/var/log/audit/audit.log", 255);
    global_audit_config.log_file[255] = '\0';

    kprintf("[AUDIT] Configuration initialized\n");
}

int audit_config_set(audit_config_t* config) {
    if (!config) {
        return -1;
    }

    // TODO: Add capability check (CAP_AUDIT_CONTROL)

    memcpy(&global_audit_config, config, sizeof(audit_config_t));

    kprintf("[AUDIT] Configuration updated\n");
    return 0;
}

audit_config_t* audit_config_get(void) {
    return &global_audit_config;
}

// Statistics
audit_stats_t* audit_get_stats(void) {
    return &audit_stats;
}

void audit_reset_stats(void) {
    memset(&audit_stats, 0, sizeof(audit_stats_t));
    kprintf("[AUDIT] Statistics reset\n");
}

// Syscall interface
int sys_audit_enable(void) {
    // TODO: Check capability (CAP_AUDIT_CONTROL)

    global_audit_config.enabled = true;
    kprintf("[AUDIT] Audit logging enabled via syscall\n");

    audit_log(AUDIT_CONFIG_POLICY_RELOAD, AUDIT_SUCCESS, 0, 0, NULL, 0, 0);
    return 0;
}

int sys_audit_disable(void) {
    // TODO: Check capability (CAP_AUDIT_CONTROL)

    audit_log(AUDIT_CONFIG_POLICY_RELOAD, AUDIT_SUCCESS, 0, 0, NULL, 0, 0);

    global_audit_config.enabled = false;
    kprintf("[AUDIT] Audit logging disabled via syscall\n");

    return 0;
}

int sys_audit_read(audit_event_t* events, uint32_t count) {
    if (!events || count == 0) {
        return -1;
    }

    // TODO: Check capability (CAP_AUDIT_READ)

    if (!global_audit_buffer) {
        return -1;
    }

    uint32_t read_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (audit_buffer_read(global_audit_buffer, &events[i]) != 0) {
            break;  // No more events
        }
        read_count++;
    }

    return read_count;
}

int sys_audit_rule_add(audit_rule_t* rule) {
    if (!rule) {
        return -1;
    }

    // TODO: Check capability (CAP_AUDIT_CONTROL)

    int result = audit_rule_add(rule);

    if (result == 0) {
        audit_log(AUDIT_CONFIG_POLICY_RELOAD, AUDIT_SUCCESS, 0, 0,
                 "rule_add", 0, rule->id);
    }

    return result;
}

int sys_audit_rule_del(uint32_t rule_id) {
    // TODO: Check capability (CAP_AUDIT_CONTROL)

    int result = audit_rule_delete(rule_id);

    if (result == 0) {
        audit_log(AUDIT_CONFIG_POLICY_RELOAD, AUDIT_SUCCESS, 0, 0,
                 "rule_delete", 0, rule_id);
    }

    return result;
}

int sys_audit_get_stats(uint64_t* total, uint64_t* dropped) {
    if (!total || !dropped) {
        return -1;
    }

    // TODO: Check capability (CAP_AUDIT_READ)

    *total = audit_stats.total_events;
    *dropped = audit_stats.events_dropped;

    return 0;
}

// Helper function for common security violations
void audit_log_security_violation(const char* violation_type,
                                  uint32_t denied_capability,
                                  const char* resource) {
    audit_event_t event;
    memset(&event, 0, sizeof(audit_event_t));

    event.timestamp = audit_get_timestamp();
    event.type = AUDIT_SECURITY_CAP_DENIED;
    event.result = AUDIT_DENIED;

    process_t* current = process_get_current();
    if (current) {
        event.pid = current->pid;
        strncpy(event.comm, current->name, AUDIT_COMM_MAX - 1);
        event.comm[AUDIT_COMM_MAX - 1] = '\0';
    }

    if (resource) {
        strncpy(event.path, resource, AUDIT_PATH_MAX - 1);
        event.path[AUDIT_PATH_MAX - 1] = '\0';
    }

    event.flags = denied_capability;

    audit_log_event(&event);

    kprintf("[AUDIT] Security violation: %s by PID %u (%s)\n",
            violation_type, event.pid, event.comm);
}

// Helper for file access logging
void audit_log_file_access(const char* path, audit_event_type_t access_type,
                          audit_result_t result, int error_code) {
    if (!global_audit_config.log_successful && result == AUDIT_SUCCESS) {
        return;  // Skip successful operations if not configured
    }

    audit_log(access_type, result, 0, 0, path, 0, error_code);
}

// Helper for process events
void audit_log_process_event(audit_event_type_t event_type,
                            uint32_t target_pid, audit_result_t result) {
    audit_event_t event;
    memset(&event, 0, sizeof(audit_event_t));

    event.timestamp = audit_get_timestamp();
    event.type = event_type;
    event.result = result;
    event.object_pid = target_pid;

    process_t* current = process_get_current();
    if (current) {
        event.pid = current->pid;
        strncpy(event.comm, current->name, AUDIT_COMM_MAX - 1);
        event.comm[AUDIT_COMM_MAX - 1] = '\0';
    }

    audit_log_event(&event);
}

// Helper for network events
void audit_log_network_event(audit_event_type_t event_type,
                            const char* host, uint16_t port,
                            audit_result_t result) {
    audit_event_t event;
    memset(&event, 0, sizeof(audit_event_t));

    event.timestamp = audit_get_timestamp();
    event.type = event_type;
    event.result = result;

    if (host) {
        strncpy(event.path, host, AUDIT_PATH_MAX - 1);
        event.path[AUDIT_PATH_MAX - 1] = '\0';
    }

    // Store port in flags field
    event.flags = port;

    process_t* current = process_get_current();
    if (current) {
        event.pid = current->pid;
        strncpy(event.comm, current->name, AUDIT_COMM_MAX - 1);
        event.comm[AUDIT_COMM_MAX - 1] = '\0';
    }

    audit_log_event(&event);
}
