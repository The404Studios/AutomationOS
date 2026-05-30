// MAC Policy Engine - Policy Loading and Rule Management
#include "../../include/mac.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"

// String functions
extern int strcmp(const char* s1, const char* s2);
extern int strncmp(const char* s1, const char* s2, size_t n);
extern size_t strlen(const char* str);
extern char* strcpy(char* dest, const char* src);
extern char* strncpy(char* dest, const char* src, size_t n);
extern void* memset(void* s, int c, size_t n);
extern void* memcpy(void* dest, const void* src, size_t n);

// ============================================================================
// Global Policy State
// ============================================================================

static mac_policy_t global_policy;
static mac_transition_t* transition_list = NULL;
static uint32_t transition_count = 0;
static bool policy_initialized = false;

// ============================================================================
// Policy Initialization
// ============================================================================

void mac_init(void) {
    kprintf("[MAC] Initializing Mandatory Access Control system...\n");

    // Initialize global policy
    memset(&global_policy, 0, sizeof(mac_policy_t));
    global_policy.enforcing = true;
    global_policy.flags = POLICY_FLAG_DEFAULT_DENY |
                          POLICY_FLAG_AUDIT_DENIALS;
    global_policy.version = 1;

    // Initialize audit subsystem
    mac_audit_init();

    policy_initialized = true;
    kprintf("[MAC] MAC system initialized (enforcing mode: %s)\n",
            global_policy.enforcing ? "ON" : "OFF");
}

void mac_set_enforcing(bool enforcing) {
    if (!policy_initialized) {
        return;
    }

    global_policy.enforcing = enforcing;
    kprintf("[MAC] Enforcing mode: %s\n", enforcing ? "ON" : "OFF");
}

bool mac_is_enforcing(void) {
    return policy_initialized && global_policy.enforcing;
}

uint64_t mac_policy_get_version(void) {
    return global_policy.version;
}

// ============================================================================
// Hash Function for Rule Lookup
// ============================================================================

static uint32_t hash_rule_key(const char* source, const char* target, object_type_t type) {
    uint32_t hash = 5381;

    // Hash source domain
    for (const char* p = source; *p; p++) {
        hash = ((hash << 5) + hash) + (uint32_t)(*p);
    }

    // Hash target domain
    for (const char* p = target; *p; p++) {
        hash = ((hash << 5) + hash) + (uint32_t)(*p);
    }

    // Hash object type
    hash = ((hash << 5) + hash) + (uint32_t)type;

    return hash % MAX_POLICY_RULES;
}

// ============================================================================
// Rule Management
// ============================================================================

int mac_policy_add_rule(const mac_rule_t* rule) {
    if (!policy_initialized || !rule) {
        return MAC_ERR_INVALID_ARG;
    }

    // Validate rule
    if (!mac_is_valid_domain(rule->source_domain) ||
        !mac_is_valid_domain(rule->target_domain)) {
        return MAC_ERR_INVALID_POLICY;
    }

    if (rule->object_type >= OBJ_TYPE_MAX) {
        return MAC_ERR_INVALID_POLICY;
    }

    // Allocate new rule
    mac_rule_t* new_rule = (mac_rule_t*)kmalloc(sizeof(mac_rule_t));
    if (!new_rule) {
        return MAC_ERR_NO_MEMORY;
    }

    // Copy rule data
    memcpy(new_rule, rule, sizeof(mac_rule_t));
    new_rule->next = NULL;

    // Hash and insert into policy
    uint32_t hash = hash_rule_key(rule->source_domain, rule->target_domain,
                                   rule->object_type);

    // Insert at head of chain
    new_rule->next = global_policy.rules[hash];
    global_policy.rules[hash] = new_rule;
    global_policy.rule_count++;

    kprintf("[MAC] Added rule: %s -> %s (%s) perms=0x%x\n",
            rule->source_domain, rule->target_domain,
            mac_object_type_to_string(rule->object_type),
            rule->permissions);

    return MAC_SUCCESS;
}

mac_rule_t* mac_policy_find_rule(const char* source, const char* target,
                                 object_type_t type) {
    if (!policy_initialized || !source || !target) {
        return NULL;
    }

    uint32_t hash = hash_rule_key(source, target, type);
    mac_rule_t* rule = global_policy.rules[hash];

    while (rule) {
        if (strcmp(rule->source_domain, source) == 0 &&
            strcmp(rule->target_domain, target) == 0 &&
            rule->object_type == type) {
            return rule;
        }
        rule = rule->next;
    }

    return NULL;
}

int mac_policy_remove_rule(const char* source, const char* target, object_type_t type) {
    if (!policy_initialized || !source || !target) {
        return MAC_ERR_INVALID_ARG;
    }

    uint32_t hash = hash_rule_key(source, target, type);
    mac_rule_t** rule_ptr = &global_policy.rules[hash];

    while (*rule_ptr) {
        mac_rule_t* rule = *rule_ptr;
        if (strcmp(rule->source_domain, source) == 0 &&
            strcmp(rule->target_domain, target) == 0 &&
            rule->object_type == type) {

            *rule_ptr = rule->next;
            kfree(rule);
            global_policy.rule_count--;
            return MAC_SUCCESS;
        }
        rule_ptr = &rule->next;
    }

    return MAC_ERR_NOT_FOUND;
}

void mac_policy_clear(void) {
    if (!policy_initialized) {
        return;
    }

    kprintf("[MAC] Clearing policy (rules: %u)\n", global_policy.rule_count);

    for (uint32_t i = 0; i < MAX_POLICY_RULES; i++) {
        mac_rule_t* rule = global_policy.rules[i];
        while (rule) {
            mac_rule_t* next = rule->next;
            kfree(rule);
            rule = next;
        }
        global_policy.rules[i] = NULL;
    }

    global_policy.rule_count = 0;
    global_policy.version++;
}

// ============================================================================
// Policy Binary Format Loading
// ============================================================================

#define POLICY_MAGIC 0x4D414350  // "MACP"
#define POLICY_VERSION 1

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t rule_count;
    uint32_t transition_count;
    uint32_t flags;
    uint32_t reserved[3];
} policy_header_t;

int mac_policy_load(const void* policy_data, size_t size) {
    if (!policy_initialized || !policy_data || size < sizeof(policy_header_t)) {
        return MAC_ERR_INVALID_POLICY;
    }

    const policy_header_t* header = (const policy_header_t*)policy_data;

    // Validate header
    if (header->magic != POLICY_MAGIC) {
        kprintf("[MAC] Invalid policy magic: 0x%x\n", header->magic);
        return MAC_ERR_INVALID_POLICY;
    }

    if (header->version != POLICY_VERSION) {
        kprintf("[MAC] Unsupported policy version: %u\n", header->version);
        return MAC_ERR_INVALID_POLICY;
    }

    // Clear existing policy
    mac_policy_clear();
    mac_transition_clear();

    // Load rules
    const mac_rule_t* rules = (const mac_rule_t*)(header + 1);
    for (uint32_t i = 0; i < header->rule_count; i++) {
        int ret = mac_policy_add_rule(&rules[i]);
        if (ret != MAC_SUCCESS) {
            kprintf("[MAC] Failed to add rule %u: %d\n", i, ret);
            return ret;
        }
    }

    // Load transitions
    const mac_transition_t* transitions =
        (const mac_transition_t*)(rules + header->rule_count);
    for (uint32_t i = 0; i < header->transition_count; i++) {
        int ret = mac_transition_add(&transitions[i]);
        if (ret != MAC_SUCCESS) {
            kprintf("[MAC] Failed to add transition %u: %d\n", i, ret);
            return ret;
        }
    }

    // Update policy flags
    global_policy.flags = header->flags;
    global_policy.version++;

    kprintf("[MAC] Policy loaded: version=%llu, rules=%u, transitions=%u\n",
            global_policy.version, header->rule_count, header->transition_count);

    // Audit policy load
    mac_audit_policy_load(global_policy.version, header->rule_count);

    return MAC_SUCCESS;
}

// ============================================================================
// Default Policy Creation
// ============================================================================

static void load_default_policy(void) {
    kprintf("[MAC] Loading default policy...\n");

    // Kernel domain has full access
    mac_rule_t rule;
    memset(&rule, 0, sizeof(rule));

    strcpy(rule.source_domain, MAC_DOMAIN_KERNEL);
    strcpy(rule.target_domain, "*");  // Wildcard
    rule.object_type = OBJ_TYPE_FILE;
    rule.permissions = 0xFFFFFFFF;  // All permissions
    rule.min_level = MLS_LEVEL_UNCLASSIFIED;
    rule.max_level = MLS_LEVEL_TOP_SECRET;
    mac_policy_add_rule(&rule);

    // User processes can read most files
    memset(&rule, 0, sizeof(rule));
    strcpy(rule.source_domain, MAC_DOMAIN_USER);
    strcpy(rule.target_domain, MAC_TYPE_FILE);
    rule.object_type = OBJ_TYPE_FILE;
    rule.permissions = MAC_FILE_READ | MAC_FILE_EXECUTE;
    mac_policy_add_rule(&rule);

    // User processes can read/write their home directories
    memset(&rule, 0, sizeof(rule));
    strcpy(rule.source_domain, MAC_DOMAIN_USER);
    strcpy(rule.target_domain, MAC_TYPE_HOME);
    rule.object_type = OBJ_TYPE_FILE;
    rule.permissions = MAC_FILE_READ | MAC_FILE_WRITE | MAC_FILE_CREATE | MAC_FILE_DELETE;
    mac_policy_add_rule(&rule);

    // User processes CANNOT read shadow files
    memset(&rule, 0, sizeof(rule));
    strcpy(rule.source_domain, MAC_DOMAIN_USER);
    strcpy(rule.target_domain, MAC_TYPE_SHADOW);
    rule.object_type = OBJ_TYPE_FILE;
    rule.permissions = 0;  // No access
    rule.flags = RULE_FLAG_DENY | RULE_FLAG_AUDIT;
    mac_policy_add_rule(&rule);

    // Untrusted domain has minimal access
    memset(&rule, 0, sizeof(rule));
    strcpy(rule.source_domain, MAC_DOMAIN_UNTRUSTED);
    strcpy(rule.target_domain, MAC_TYPE_TMP);
    rule.object_type = OBJ_TYPE_FILE;
    rule.permissions = MAC_FILE_READ | MAC_FILE_WRITE | MAC_FILE_CREATE;
    mac_policy_add_rule(&rule);

    kprintf("[MAC] Default policy loaded\n");
}

// ============================================================================
// Type Transitions
// ============================================================================

int mac_transition_add(const mac_transition_t* trans) {
    if (!policy_initialized || !trans) {
        return MAC_ERR_INVALID_ARG;
    }

    if (transition_count >= MAX_TRANSITIONS) {
        return MAC_ERR_NO_MEMORY;
    }

    mac_transition_t* new_trans = (mac_transition_t*)kmalloc(sizeof(mac_transition_t));
    if (!new_trans) {
        return MAC_ERR_NO_MEMORY;
    }

    memcpy(new_trans, trans, sizeof(mac_transition_t));
    new_trans->next = transition_list;
    transition_list = new_trans;
    transition_count++;

    kprintf("[MAC] Added transition: %s -> %s => %s (path: %s)\n",
            trans->source_domain, trans->target_domain,
            trans->result_domain, trans->path_pattern);

    return MAC_SUCCESS;
}

// Simple pattern matching for paths (supports * wildcard)
static bool path_matches_pattern(const char* path, const char* pattern) {
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return true;

            while (*path) {
                if (path_matches_pattern(path, pattern)) {
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

mac_transition_t* mac_transition_find(const char* source_domain, const char* target_path) {
    if (!policy_initialized || !source_domain || !target_path) {
        return NULL;
    }

    mac_transition_t* trans = transition_list;
    while (trans) {
        if (strcmp(trans->source_domain, source_domain) == 0) {
            if (path_matches_pattern(target_path, trans->path_pattern)) {
                return trans;
            }
        }
        trans = trans->next;
    }

    return NULL;
}

void mac_transition_clear(void) {
    mac_transition_t* trans = transition_list;
    while (trans) {
        mac_transition_t* next = trans->next;
        kfree(trans);
        trans = next;
    }

    transition_list = NULL;
    transition_count = 0;
}

int mac_process_transition(struct process* proc, const char* exec_path) {
    if (!proc || !exec_path) {
        return MAC_ERR_INVALID_ARG;
    }

    security_label_t* current_label = mac_process_get_label(proc);
    if (!current_label) {
        return MAC_ERR_INVALID_LABEL;
    }

    // Find matching transition
    mac_transition_t* trans = mac_transition_find(current_label->domain, exec_path);
    if (trans) {
        kprintf("[MAC] Process %u transitioning: %s -> %s (exec: %s)\n",
                proc->pid, current_label->domain, trans->result_domain, exec_path);

        // Audit the transition
        mac_audit_transition(proc, current_label->domain,
                           trans->result_domain, exec_path);

        // Create new label with result domain
        security_label_t new_label;
        memcpy(&new_label, current_label, sizeof(security_label_t));
        strncpy(new_label.domain, trans->result_domain, MAX_LABEL_NAME - 1);

        return mac_process_set_label(proc, &new_label);
    }

    // No transition found - keep current domain
    return MAC_SUCCESS;
}

// ============================================================================
// Module Initialization
// ============================================================================

void __attribute__((constructor)) mac_policy_init(void) {
    // This will be called during kernel initialization
    // Load default policy on first use
    static bool default_loaded = false;

    if (policy_initialized && !default_loaded) {
        load_default_policy();
        default_loaded = true;
    }
}
