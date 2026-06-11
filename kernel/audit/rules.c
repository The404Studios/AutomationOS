#include "../include/audit.h"
#include "../include/kernel.h"
#include "../include/mem.h"

extern void* kmalloc(size_t size);
extern void kfree(void* ptr);
extern void* memcpy(void* dest, const void* src, size_t n);
extern audit_stats_t audit_stats;

// Rule list head
static audit_rule_t* rule_list_head = NULL;
static uint32_t next_rule_id = 1;
static uint32_t rule_count = 0;

// Simple spinlock for rule list
static uint32_t rules_lock = 0;

static inline void spin_lock(uint32_t* lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        __asm__ volatile("pause");
    }
}

static inline void spin_unlock(uint32_t* lock) {
    __sync_lock_release(lock);
}

// Simple string comparison
static int simple_strcmp(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        if (*s1 != *s2) {
            return *s1 - *s2;
        }
        s1++;
        s2++;
    }
    return *s1 - *s2;
}

// Simple glob match (for path patterns) with depth limit
#define GLOB_MAX_DEPTH 16

static bool path_matches_pattern_depth(const char* path, const char* pattern, int depth) {
    if (!path || !pattern) return false;
    if (depth >= GLOB_MAX_DEPTH) return false;

    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return true;

            while (*path) {
                if (path_matches_pattern_depth(path, pattern, depth + 1)) {
                    return true;
                }
                path++;
            }
            return false;
        } else if (*pattern == *path) {
            pattern++;
            path++;
        } else {
            return false;
        }
    }

    return *path == '\0';
}

static bool path_matches_pattern(const char* path, const char* pattern) {
    return path_matches_pattern_depth(path, pattern, 0);
}

void audit_rules_init(void) {
    kprintf("[AUDIT] Initializing audit rules engine...\n");

    rule_list_head = NULL;
    next_rule_id = 1;
    rule_count = 0;
    rules_lock = 0;

    // Add some default rules

    // Rule 1: Always log security violations
    audit_rule_t* rule1 = (audit_rule_t*)kmalloc(sizeof(audit_rule_t));
    if (rule1) {
        rule1->id = next_rule_id++;
        rule1->filter_type = AUDIT_FILTER_TYPE;
        rule1->action = AUDIT_ACTION_ALERT;
        rule1->criteria.event_type = AUDIT_SECURITY_CAP_DENIED;
        rule1->enabled = true;
        rule1->next = NULL;

        rule_list_head = rule1;
        rule_count++;
        kprintf("[AUDIT] Default rule: Alert on capability denied\n");
    }

    // Rule 2: Always log MAC denials
    audit_rule_t* rule2 = (audit_rule_t*)kmalloc(sizeof(audit_rule_t));
    if (rule2) {
        rule2->id = next_rule_id++;
        rule2->filter_type = AUDIT_FILTER_TYPE;
        rule2->action = AUDIT_ACTION_ALERT;
        rule2->criteria.event_type = AUDIT_SECURITY_MAC_DENIED;
        rule2->enabled = true;
        rule2->next = rule_list_head;

        rule_list_head = rule2;
        rule_count++;
        kprintf("[AUDIT] Default rule: Alert on MAC denied\n");
    }

    // Rule 3: Always log authentication failures
    audit_rule_t* rule3 = (audit_rule_t*)kmalloc(sizeof(audit_rule_t));
    if (rule3) {
        rule3->id = next_rule_id++;
        rule3->filter_type = AUDIT_FILTER_TYPE;
        rule3->action = AUDIT_ACTION_ALERT;
        rule3->criteria.event_type = AUDIT_AUTH_FAILED;
        rule3->enabled = true;
        rule3->next = rule_list_head;

        rule_list_head = rule3;
        rule_count++;
        kprintf("[AUDIT] Default rule: Alert on auth failure\n");
    }

    // Rule 4: Log kernel panics (critical events)
    audit_rule_t* rule4 = (audit_rule_t*)kmalloc(sizeof(audit_rule_t));
    if (rule4) {
        rule4->id = next_rule_id++;
        rule4->filter_type = AUDIT_FILTER_TYPE;
        rule4->action = AUDIT_ACTION_ALERT;
        rule4->criteria.event_type = AUDIT_KERNEL_PANIC;
        rule4->enabled = true;
        rule4->next = rule_list_head;

        rule_list_head = rule4;
        rule_count++;
        kprintf("[AUDIT] Default rule: Alert on kernel panic\n");
    }

    kprintf("[AUDIT] Audit rules engine initialized (%u rules)\n", rule_count);
}

int audit_rule_add(audit_rule_t* rule) {
    if (!rule) {
        return -1;
    }

    // Allocate BEFORE acquiring lock to avoid sleeping under spinlock
    audit_rule_t* new_rule = (audit_rule_t*)kmalloc(sizeof(audit_rule_t));
    if (!new_rule) {
        return -1;
    }

    // Copy rule data
    memcpy(new_rule, rule, sizeof(audit_rule_t));

    spin_lock(&rules_lock);

    // Assign ID if not provided
    if (new_rule->id == 0) {
        new_rule->id = next_rule_id++;
    }

    // Add to list
    new_rule->next = rule_list_head;
    rule_list_head = new_rule;
    rule_count++;

    spin_unlock(&rules_lock);

    kprintf("[AUDIT] Added rule ID %u (filter type: %d, action: %d)\n",
            new_rule->id, new_rule->filter_type, new_rule->action);

    return new_rule->id;
}

int audit_rule_delete(uint32_t rule_id) {
    audit_rule_t* to_delete = NULL;

    spin_lock(&rules_lock);

    audit_rule_t** current = &rule_list_head;

    while (*current) {
        if ((*current)->id == rule_id) {
            to_delete = *current;
            *current = (*current)->next;
            rule_count--;
            break;
        }

        current = &(*current)->next;
    }

    spin_unlock(&rules_lock);

    // Free AFTER releasing lock to avoid kfree under spinlock
    if (to_delete) {
        kfree(to_delete);
        kprintf("[AUDIT] Deleted rule ID %u\n", rule_id);
        return 0;
    }

    return -1;  // Rule not found
}

audit_rule_t* audit_rule_find(uint32_t rule_id) {
    spin_lock(&rules_lock);

    audit_rule_t* current = rule_list_head;

    while (current) {
        if (current->id == rule_id) {
            spin_unlock(&rules_lock);
            return current;
        }
        current = current->next;
    }

    spin_unlock(&rules_lock);
    return NULL;
}

bool audit_rule_matches(audit_rule_t* rule, audit_event_t* event) {
    if (!rule || !event || !rule->enabled) {
        return false;
    }

    switch (rule->filter_type) {
        case AUDIT_FILTER_TYPE:
            return event->type == rule->criteria.event_type;

        case AUDIT_FILTER_UID:
            return event->uid == rule->criteria.uid;

        case AUDIT_FILTER_PID:
            return event->pid == rule->criteria.pid;

        case AUDIT_FILTER_SYSCALL:
            return event->syscall == rule->criteria.syscall;

        case AUDIT_FILTER_PATH:
            if (event->path[0] != '\0') {
                return path_matches_pattern(event->path,
                                          rule->criteria.path_pattern);
            }
            return false;

        case AUDIT_FILTER_RESULT:
            return event->result == rule->criteria.result;

        default:
            return false;
    }
}

audit_action_t audit_rules_evaluate(audit_event_t* event) {
    if (!event) {
        return AUDIT_ACTION_LOG;  // Default action
    }

    spin_lock(&rules_lock);

    audit_action_t action = AUDIT_ACTION_LOG;  // Default
    audit_rule_t* current = rule_list_head;

    // Evaluate all matching rules
    // Last matching rule wins
    while (current) {
        audit_stats.rule_evaluations++;

        if (audit_rule_matches(current, event)) {
            action = current->action;
            // Don't break - continue to check for higher priority rules
        }

        current = current->next;
    }

    spin_unlock(&rules_lock);

    return action;
}

// Enable/disable specific rule
int audit_rule_set_enabled(uint32_t rule_id, bool enabled) {
    spin_lock(&rules_lock);

    audit_rule_t* current = rule_list_head;

    while (current) {
        if (current->id == rule_id) {
            current->enabled = enabled;
            spin_unlock(&rules_lock);

            kprintf("[AUDIT] Rule ID %u %s\n",
                    rule_id, enabled ? "enabled" : "disabled");
            return 0;
        }
        current = current->next;
    }

    spin_unlock(&rules_lock);
    return -1;
}

// List all rules (for debugging/userspace tools)
int audit_rule_list(audit_rule_t* buffer, uint32_t max_count) {
    if (!buffer || max_count == 0) {
        return -1;
    }

    spin_lock(&rules_lock);

    audit_rule_t* current = rule_list_head;
    uint32_t count = 0;

    while (current && count < max_count) {
        memcpy(&buffer[count], current, sizeof(audit_rule_t));
        count++;
        current = current->next;
    }

    spin_unlock(&rules_lock);

    return count;
}

// Delete all rules
void audit_rule_clear_all(void) {
    spin_lock(&rules_lock);

    audit_rule_t* current = rule_list_head;

    while (current) {
        audit_rule_t* next = current->next;
        kfree(current);
        current = next;
    }

    rule_list_head = NULL;
    rule_count = 0;

    spin_unlock(&rules_lock);

    kprintf("[AUDIT] All rules cleared\n");
}

// Get rule count
uint32_t audit_rule_count(void) {
    return rule_count;
}

// Advanced: Add compound rule (multiple criteria)
// For Phase 2 MVP, we keep it simple with single-criteria rules
// Future: Extend to support AND/OR combinations

// Example rule builder functions for common scenarios
int audit_rule_add_alert_on_type(audit_event_type_t event_type) {
    audit_rule_t rule;
    rule.id = 0;  // Auto-assign
    rule.filter_type = AUDIT_FILTER_TYPE;
    rule.action = AUDIT_ACTION_ALERT;
    rule.criteria.event_type = event_type;
    rule.enabled = true;
    rule.next = NULL;

    return audit_rule_add(&rule);
}

int audit_rule_add_ignore_uid(uint32_t uid) {
    audit_rule_t rule;
    rule.id = 0;
    rule.filter_type = AUDIT_FILTER_UID;
    rule.action = AUDIT_ACTION_IGNORE;
    rule.criteria.uid = uid;
    rule.enabled = true;
    rule.next = NULL;

    return audit_rule_add(&rule);
}

int audit_rule_add_log_path(const char* path_pattern) {
    if (!path_pattern) {
        return -1;
    }

    audit_rule_t rule;
    rule.id = 0;
    rule.filter_type = AUDIT_FILTER_PATH;
    rule.action = AUDIT_ACTION_LOG;
    rule.enabled = true;
    rule.next = NULL;

    // Copy path pattern
    for (uint32_t i = 0; i < AUDIT_PATH_MAX && path_pattern[i]; i++) {
        rule.criteria.path_pattern[i] = path_pattern[i];
    }
    rule.criteria.path_pattern[AUDIT_PATH_MAX - 1] = '\0';

    return audit_rule_add(&rule);
}

int audit_rule_add_alert_on_failure(void) {
    audit_rule_t rule;
    rule.id = 0;
    rule.filter_type = AUDIT_FILTER_RESULT;
    rule.action = AUDIT_ACTION_ALERT;
    rule.criteria.result = AUDIT_FAILURE;
    rule.enabled = true;
    rule.next = NULL;

    return audit_rule_add(&rule);
}

// Rule statistics
void audit_rule_print_stats(void) {
    spin_lock(&rules_lock);

    kprintf("[AUDIT] Rule Statistics:\n");
    kprintf("  Total rules: %u\n", rule_count);

    audit_rule_t* current = rule_list_head;
    uint32_t enabled_count = 0;

    while (current) {
        if (current->enabled) {
            enabled_count++;
        }
        current = current->next;
    }

    kprintf("  Enabled rules: %u\n", enabled_count);
    kprintf("  Disabled rules: %u\n", rule_count - enabled_count);
    kprintf("  Rule evaluations: %llu\n", audit_stats.rule_evaluations);

    spin_unlock(&rules_lock);
}
