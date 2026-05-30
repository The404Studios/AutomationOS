// MAC Audit Logging System
#include "../../include/mac.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/sched.h"

// String functions
extern int strcmp(const char* s1, const char* s2);
extern char* strncpy(char* dest, const char* src, size_t n);
extern void* memset(void* s, int c, size_t n);
extern int snprintf(char* str, size_t size, const char* format, ...);

// ============================================================================
// Audit Ring Buffer
// ============================================================================

#define AUDIT_BUFFER_SIZE 1024

static mac_audit_event_t audit_buffer[AUDIT_BUFFER_SIZE];
static uint32_t audit_head = 0;
static uint32_t audit_tail = 0;
static uint32_t audit_count = 0;
static uint64_t audit_total = 0;
static bool audit_enabled = true;

// Simple timestamp counter (should be replaced with real time source)
static uint64_t get_timestamp(void) {
    static uint64_t counter = 0;
    return counter++;
}

// ============================================================================
// Initialization
// ============================================================================

void mac_audit_init(void) {
    memset(audit_buffer, 0, sizeof(audit_buffer));
    audit_head = 0;
    audit_tail = 0;
    audit_count = 0;
    audit_total = 0;
    audit_enabled = true;

    kprintf("[MAC-AUDIT] Audit subsystem initialized (buffer size: %u)\n",
            AUDIT_BUFFER_SIZE);
}

// ============================================================================
// Audit Event Logging
// ============================================================================

void mac_audit_log(const mac_audit_event_t* event) {
    if (!audit_enabled || !event) {
        return;
    }

    // Add to ring buffer
    audit_buffer[audit_head] = *event;
    audit_head = (audit_head + 1) % AUDIT_BUFFER_SIZE;

    if (audit_count < AUDIT_BUFFER_SIZE) {
        audit_count++;
    } else {
        // Buffer full, overwrite oldest entry
        audit_tail = (audit_tail + 1) % AUDIT_BUFFER_SIZE;
    }

    audit_total++;

    // Print audit message to kernel log
    const char* type_str;
    switch (event->type) {
        case MAC_AUDIT_ALLOWED:
            type_str = "ALLOWED";
            break;
        case MAC_AUDIT_DENIED:
            type_str = "DENIED";
            break;
        case MAC_AUDIT_LABEL_CHANGE:
            type_str = "LABEL_CHANGE";
            break;
        case MAC_AUDIT_POLICY_LOAD:
            type_str = "POLICY_LOAD";
            break;
        case MAC_AUDIT_TRANSITION:
            type_str = "TRANSITION";
            break;
        default:
            type_str = "UNKNOWN";
            break;
    }

    kprintf("[MAC-AUDIT] %s: pid=%u domain=%s path=%s perms=0x%x\n",
            type_str, event->pid, event->subject.domain, event->path,
            event->requested_perms);
}

// ============================================================================
// Specific Audit Functions
// ============================================================================

void mac_audit_access(const process_t* proc, const char* path,
                      object_type_t type, uint32_t perms, bool allowed) {
    if (!proc) {
        return;
    }

    mac_audit_event_t event;
    memset(&event, 0, sizeof(event));

    event.type = allowed ? MAC_AUDIT_ALLOWED : MAC_AUDIT_DENIED;
    event.timestamp = get_timestamp();
    event.pid = proc->pid;
    event.obj_type = type;
    event.requested_perms = perms;
    event.denied_perms = allowed ? 0 : perms;

    // Get process label
    security_label_t* label = mac_process_get_label(proc);
    if (label) {
        event.subject = *label;
    }

    // Copy path
    if (path) {
        strncpy(event.path, path, sizeof(event.path) - 1);
    }

    // Format message
    snprintf(event.message, sizeof(event.message),
             "%s access by process %u (%s) to %s",
             allowed ? "Allowed" : "Denied",
             proc->pid, proc->name, path ? path : "unknown");

    mac_audit_log(&event);
}

void mac_audit_denial(const process_t* proc, const char* path,
                      object_type_t type, uint32_t denied_perms) {
    mac_audit_access(proc, path, type, denied_perms, false);
}

void mac_audit_label_change(const process_t* proc,
                            const security_label_t* old_label,
                            const security_label_t* new_label) {
    if (!proc || !old_label || !new_label) {
        return;
    }

    mac_audit_event_t event;
    memset(&event, 0, sizeof(event));

    event.type = MAC_AUDIT_LABEL_CHANGE;
    event.timestamp = get_timestamp();
    event.pid = proc->pid;
    event.subject = *old_label;
    event.object = *new_label;

    snprintf(event.message, sizeof(event.message),
             "Process %u label changed: %s -> %s",
             proc->pid, old_label->domain, new_label->domain);

    mac_audit_log(&event);
}

void mac_audit_policy_load(uint64_t version, uint32_t rule_count) {
    mac_audit_event_t event;
    memset(&event, 0, sizeof(event));

    event.type = MAC_AUDIT_POLICY_LOAD;
    event.timestamp = get_timestamp();
    event.pid = 0;  // System event

    snprintf(event.message, sizeof(event.message),
             "Policy loaded: version=%llu, rules=%u",
             version, rule_count);

    mac_audit_log(&event);
}

void mac_audit_transition(const process_t* proc, const char* from_domain,
                          const char* to_domain, const char* exec_path) {
    if (!proc || !from_domain || !to_domain) {
        return;
    }

    mac_audit_event_t event;
    memset(&event, 0, sizeof(event));

    event.type = MAC_AUDIT_TRANSITION;
    event.timestamp = get_timestamp();
    event.pid = proc->pid;

    strncpy(event.subject.domain, from_domain, MAX_LABEL_NAME - 1);
    strncpy(event.object.domain, to_domain, MAX_LABEL_NAME - 1);

    if (exec_path) {
        strncpy(event.path, exec_path, sizeof(event.path) - 1);
    }

    snprintf(event.message, sizeof(event.message),
             "Domain transition: pid=%u %s -> %s (exec: %s)",
             proc->pid, from_domain, to_domain,
             exec_path ? exec_path : "unknown");

    mac_audit_log(&event);
}

// ============================================================================
// Audit Query Functions
// ============================================================================

uint32_t mac_audit_get_count(void) {
    return audit_count;
}

int mac_audit_get_events(mac_audit_event_t* buffer, uint32_t count) {
    if (!buffer || count == 0) {
        return -1;
    }

    uint32_t copied = 0;
    uint32_t index = audit_tail;

    while (copied < count && copied < audit_count) {
        buffer[copied] = audit_buffer[index];
        index = (index + 1) % AUDIT_BUFFER_SIZE;
        copied++;
    }

    return copied;
}

// ============================================================================
// Audit Configuration
// ============================================================================

void mac_audit_enable(void) {
    audit_enabled = true;
    kprintf("[MAC-AUDIT] Audit logging enabled\n");
}

void mac_audit_disable(void) {
    audit_enabled = false;
    kprintf("[MAC-AUDIT] Audit logging disabled\n");
}

bool mac_audit_is_enabled(void) {
    return audit_enabled;
}

void mac_audit_clear(void) {
    memset(audit_buffer, 0, sizeof(audit_buffer));
    audit_head = 0;
    audit_tail = 0;
    audit_count = 0;
    kprintf("[MAC-AUDIT] Audit buffer cleared\n");
}

uint64_t mac_audit_get_total(void) {
    return audit_total;
}

// ============================================================================
// Audit Report Generation
// ============================================================================

void mac_audit_print_summary(void) {
    kprintf("[MAC-AUDIT] ===== Audit Summary =====\n");
    kprintf("[MAC-AUDIT] Total events: %llu\n", audit_total);
    kprintf("[MAC-AUDIT] Buffer size: %u\n", AUDIT_BUFFER_SIZE);
    kprintf("[MAC-AUDIT] Current count: %u\n", audit_count);
    kprintf("[MAC-AUDIT] Status: %s\n", audit_enabled ? "ENABLED" : "DISABLED");

    // Count event types
    uint32_t allowed_count = 0;
    uint32_t denied_count = 0;
    uint32_t label_change_count = 0;
    uint32_t policy_load_count = 0;
    uint32_t transition_count = 0;

    uint32_t index = audit_tail;
    for (uint32_t i = 0; i < audit_count; i++) {
        switch (audit_buffer[index].type) {
            case MAC_AUDIT_ALLOWED:
                allowed_count++;
                break;
            case MAC_AUDIT_DENIED:
                denied_count++;
                break;
            case MAC_AUDIT_LABEL_CHANGE:
                label_change_count++;
                break;
            case MAC_AUDIT_POLICY_LOAD:
                policy_load_count++;
                break;
            case MAC_AUDIT_TRANSITION:
                transition_count++;
                break;
        }
        index = (index + 1) % AUDIT_BUFFER_SIZE;
    }

    kprintf("[MAC-AUDIT] Event breakdown:\n");
    kprintf("[MAC-AUDIT]   Allowed: %u\n", allowed_count);
    kprintf("[MAC-AUDIT]   Denied: %u\n", denied_count);
    kprintf("[MAC-AUDIT]   Label changes: %u\n", label_change_count);
    kprintf("[MAC-AUDIT]   Policy loads: %u\n", policy_load_count);
    kprintf("[MAC-AUDIT]   Transitions: %u\n", transition_count);
    kprintf("[MAC-AUDIT] ========================\n");
}

void mac_audit_print_recent(uint32_t count) {
    if (count > audit_count) {
        count = audit_count;
    }

    kprintf("[MAC-AUDIT] ===== Recent %u Events =====\n", count);

    // Start from the most recent
    uint32_t start_index = (audit_head - count + AUDIT_BUFFER_SIZE) % AUDIT_BUFFER_SIZE;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t index = (start_index + i) % AUDIT_BUFFER_SIZE;
        mac_audit_event_t* event = &audit_buffer[index];

        const char* type_str;
        switch (event->type) {
            case MAC_AUDIT_ALLOWED:
                type_str = "ALLOWED";
                break;
            case MAC_AUDIT_DENIED:
                type_str = "DENIED";
                break;
            case MAC_AUDIT_LABEL_CHANGE:
                type_str = "LABEL_CHG";
                break;
            case MAC_AUDIT_POLICY_LOAD:
                type_str = "POLICY_LD";
                break;
            case MAC_AUDIT_TRANSITION:
                type_str = "TRANSITION";
                break;
            default:
                type_str = "UNKNOWN";
                break;
        }

        kprintf("[MAC-AUDIT] [%llu] %-11s pid=%u %s\n",
                event->timestamp, type_str, event->pid, event->message);
    }

    kprintf("[MAC-AUDIT] ========================\n");
}

// ============================================================================
// Audit Filters (for future use)
// ============================================================================

// Filter events by PID
uint32_t mac_audit_filter_by_pid(uint32_t pid, mac_audit_event_t* buffer,
                                 uint32_t max_count) {
    if (!buffer || max_count == 0) {
        return 0;
    }

    uint32_t found = 0;
    uint32_t index = audit_tail;

    for (uint32_t i = 0; i < audit_count && found < max_count; i++) {
        if (audit_buffer[index].pid == pid) {
            buffer[found++] = audit_buffer[index];
        }
        index = (index + 1) % AUDIT_BUFFER_SIZE;
    }

    return found;
}

// Filter events by type
uint32_t mac_audit_filter_by_type(mac_audit_type_t type, mac_audit_event_t* buffer,
                                  uint32_t max_count) {
    if (!buffer || max_count == 0) {
        return 0;
    }

    uint32_t found = 0;
    uint32_t index = audit_tail;

    for (uint32_t i = 0; i < audit_count && found < max_count; i++) {
        if (audit_buffer[index].type == type) {
            buffer[found++] = audit_buffer[index];
        }
        index = (index + 1) % AUDIT_BUFFER_SIZE;
    }

    return found;
}

// Filter events by domain
uint32_t mac_audit_filter_by_domain(const char* domain, mac_audit_event_t* buffer,
                                    uint32_t max_count) {
    if (!buffer || !domain || max_count == 0) {
        return 0;
    }

    uint32_t found = 0;
    uint32_t index = audit_tail;

    for (uint32_t i = 0; i < audit_count && found < max_count; i++) {
        if (strcmp(audit_buffer[index].subject.domain, domain) == 0) {
            buffer[found++] = audit_buffer[index];
        }
        index = (index + 1) % AUDIT_BUFFER_SIZE;
    }

    return found;
}
