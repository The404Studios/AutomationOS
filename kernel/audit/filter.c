#include "../include/audit.h"
#include "../include/kernel.h"

extern audit_config_t* audit_config_get(void);

// Filter state
static bool filter_initialized = false;

// Event type filtering bitmask (for fast filtering)
static uint64_t event_type_filter_mask[160];  // 9999 / 64 = 156.23, round up to 160

void audit_filter_init(void) {
    kprintf("[AUDIT] Initializing audit filter...\n");

    // Initialize all event types to be logged by default
    for (int i = 0; i < 160; i++) {
        event_type_filter_mask[i] = 0xFFFFFFFFFFFFFFFFULL;
    }

    // By default, filter out some noisy events
    // (can be re-enabled via rules)

    // Example: Filter out successful read operations (too noisy)
    uint32_t noisy_events[] = {
        // AUDIT_FILE_READ,  // Uncomment to filter by default
        // AUDIT_NET_RECV,
        0  // Sentinel
    };

    for (int i = 0; noisy_events[i] != 0; i++) {
        uint32_t event_type = noisy_events[i];
        uint32_t word = event_type / 64;
        uint32_t bit = event_type % 64;
        event_type_filter_mask[word] &= ~(1ULL << bit);
    }

    filter_initialized = true;
    kprintf("[AUDIT] Audit filter initialized\n");
}

bool audit_filter_should_log(audit_event_t* event) {
    if (!event || !filter_initialized) {
        return true;  // Log by default if not initialized
    }

    audit_config_t* config = audit_config_get();
    if (!config) {
        return true;
    }

    // Filter based on result (success/failure)
    if (event->result == AUDIT_SUCCESS && !config->log_successful) {
        return false;
    }

    if (event->result != AUDIT_SUCCESS && !config->log_failed) {
        return false;
    }

    // Check event type bitmask for fast filtering
    uint32_t event_type = event->type;
    if (event_type < AUDIT_EVENT_MAX) {
        uint32_t word = event_type / 64;
        uint32_t bit = event_type % 64;

        if (word < 160) {
            if (!(event_type_filter_mask[word] & (1ULL << bit))) {
                return false;  // Event type filtered out
            }
        }
    }

    // Additional filtering logic can be added here
    // e.g., filter by UID, PID, path patterns, etc.

    return true;
}

// Enable/disable specific event type
int audit_filter_set_event_type(audit_event_type_t event_type, bool enabled) {
    if (event_type >= AUDIT_EVENT_MAX) {
        return -1;
    }

    uint32_t word = event_type / 64;
    uint32_t bit = event_type % 64;

    if (word >= 160) {
        return -1;
    }

    if (enabled) {
        event_type_filter_mask[word] |= (1ULL << bit);
    } else {
        event_type_filter_mask[word] &= ~(1ULL << bit);
    }

    kprintf("[AUDIT] Event type %d filter: %s\n",
            event_type, enabled ? "enabled" : "disabled");

    return 0;
}

// Check if event type is enabled
bool audit_filter_is_enabled(audit_event_type_t event_type) {
    if (event_type >= AUDIT_EVENT_MAX) {
        return false;
    }

    uint32_t word = event_type / 64;
    uint32_t bit = event_type % 64;

    if (word >= 160) {
        return false;
    }

    return (event_type_filter_mask[word] & (1ULL << bit)) != 0;
}

// Advanced filtering: UID-based filtering
#define MAX_FILTERED_UIDS 32
static uint32_t filtered_uids[MAX_FILTERED_UIDS];
static uint32_t filtered_uid_count = 0;

int audit_filter_add_uid(uint32_t uid) {
    if (filtered_uid_count >= MAX_FILTERED_UIDS) {
        return -1;
    }

    // Check for duplicates
    for (uint32_t i = 0; i < filtered_uid_count; i++) {
        if (filtered_uids[i] == uid) {
            return 0;  // Already exists
        }
    }

    filtered_uids[filtered_uid_count++] = uid;
    kprintf("[AUDIT] Added UID %u to filter list\n", uid);

    return 0;
}

int audit_filter_remove_uid(uint32_t uid) {
    for (uint32_t i = 0; i < filtered_uid_count; i++) {
        if (filtered_uids[i] == uid) {
            // Shift remaining UIDs
            for (uint32_t j = i; j < filtered_uid_count - 1; j++) {
                filtered_uids[j] = filtered_uids[j + 1];
            }
            filtered_uid_count--;
            kprintf("[AUDIT] Removed UID %u from filter list\n", uid);
            return 0;
        }
    }

    return -1;  // Not found
}

bool audit_filter_uid_is_filtered(uint32_t uid) {
    for (uint32_t i = 0; i < filtered_uid_count; i++) {
        if (filtered_uids[i] == uid) {
            return true;
        }
    }
    return false;
}

// Advanced filtering: PID-based filtering
#define MAX_FILTERED_PIDS 64
static uint32_t filtered_pids[MAX_FILTERED_PIDS];
static uint32_t filtered_pid_count = 0;

int audit_filter_add_pid(uint32_t pid) {
    if (filtered_pid_count >= MAX_FILTERED_PIDS) {
        return -1;
    }

    for (uint32_t i = 0; i < filtered_pid_count; i++) {
        if (filtered_pids[i] == pid) {
            return 0;
        }
    }

    filtered_pids[filtered_pid_count++] = pid;
    kprintf("[AUDIT] Added PID %u to filter list\n", pid);

    return 0;
}

int audit_filter_remove_pid(uint32_t pid) {
    for (uint32_t i = 0; i < filtered_pid_count; i++) {
        if (filtered_pids[i] == pid) {
            for (uint32_t j = i; j < filtered_pid_count - 1; j++) {
                filtered_pids[j] = filtered_pids[j + 1];
            }
            filtered_pid_count--;
            kprintf("[AUDIT] Removed PID %u from filter list\n", pid);
            return 0;
        }
    }

    return -1;
}

bool audit_filter_pid_is_filtered(uint32_t pid) {
    for (uint32_t i = 0; i < filtered_pid_count; i++) {
        if (filtered_pids[i] == pid) {
            return true;
        }
    }
    return false;
}

// Path pattern filtering (simple glob matching)
#define MAX_PATH_FILTERS 16

typedef struct {
    char pattern[AUDIT_PATH_MAX];
    bool enabled;
} path_filter_t;

static path_filter_t path_filters[MAX_PATH_FILTERS];
static uint32_t path_filter_count = 0;

static bool simple_glob_match(const char* text, const char* pattern) {
    if (!text || !pattern) return false;

    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return true;

            while (*text) {
                if (simple_glob_match(text, pattern)) {
                    return true;
                }
                text++;
            }
            return false;
        } else if (*pattern == *text) {
            pattern++;
            text++;
        } else {
            return false;
        }
    }

    return *text == '\0';
}

int audit_filter_add_path_pattern(const char* pattern) {
    if (!pattern || path_filter_count >= MAX_PATH_FILTERS) {
        return -1;
    }

    for (uint32_t i = 0; i < MAX_PATH_FILTERS; i++) {
        if (!path_filters[i].enabled) {
            // Found empty slot
            for (uint32_t j = 0; j < AUDIT_PATH_MAX && pattern[j]; j++) {
                path_filters[i].pattern[j] = pattern[j];
            }
            path_filters[i].pattern[AUDIT_PATH_MAX - 1] = '\0';
            path_filters[i].enabled = true;
            path_filter_count++;

            kprintf("[AUDIT] Added path filter: %s\n", pattern);
            return 0;
        }
    }

    return -1;
}

int audit_filter_remove_path_pattern(const char* pattern) {
    if (!pattern) return -1;

    for (uint32_t i = 0; i < MAX_PATH_FILTERS; i++) {
        if (path_filters[i].enabled) {
            // Simple string comparison
            bool match = true;
            for (uint32_t j = 0; j < AUDIT_PATH_MAX; j++) {
                if (path_filters[i].pattern[j] != pattern[j]) {
                    match = false;
                    break;
                }
                if (pattern[j] == '\0') break;
            }

            if (match) {
                path_filters[i].enabled = false;
                path_filter_count--;
                kprintf("[AUDIT] Removed path filter: %s\n", pattern);
                return 0;
            }
        }
    }

    return -1;
}

bool audit_filter_path_is_filtered(const char* path) {
    if (!path) return false;

    for (uint32_t i = 0; i < MAX_PATH_FILTERS; i++) {
        if (path_filters[i].enabled) {
            if (simple_glob_match(path, path_filters[i].pattern)) {
                return true;
            }
        }
    }

    return false;
}

// Comprehensive filter check (used after basic checks)
bool audit_filter_comprehensive_check(audit_event_t* event) {
    if (!event) return false;

    // Check UID filter
    if (audit_filter_uid_is_filtered(event->uid)) {
        return false;
    }

    // Check PID filter
    if (audit_filter_pid_is_filtered(event->pid)) {
        return false;
    }

    // Check path filter
    if (event->path[0] != '\0') {
        if (audit_filter_path_is_filtered(event->path)) {
            return false;
        }
    }

    return true;
}

// Reset all filters to default
void audit_filter_reset(void) {
    kprintf("[AUDIT] Resetting all filters to default...\n");

    // Re-enable all event types
    for (int i = 0; i < 160; i++) {
        event_type_filter_mask[i] = 0xFFFFFFFFFFFFFFFFULL;
    }

    // Clear UID filters
    filtered_uid_count = 0;

    // Clear PID filters
    filtered_pid_count = 0;

    // Clear path filters
    for (int i = 0; i < MAX_PATH_FILTERS; i++) {
        path_filters[i].enabled = false;
    }
    path_filter_count = 0;

    kprintf("[AUDIT] All filters reset\n");
}
