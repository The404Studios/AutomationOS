// Security Label Management for MAC System
#include "../../include/mac.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/sched.h"

// String functions
extern int strcmp(const char* s1, const char* s2);
extern int strncmp(const char* s1, const char* s2, size_t n);
extern size_t strlen(const char* str);
extern char* strcpy(char* dest, const char* src);
extern char* strncpy(char* dest, const char* src, size_t n);
extern void* memset(void* s, int c, size_t n);
extern void* memcpy(void* dest, const void* src, size_t n);

// ============================================================================
// Label Creation and Destruction
// ============================================================================

security_label_t* mac_label_create(const char* domain, label_type_t type, mls_level_t level) {
    if (!domain || !mac_is_valid_domain(domain)) {
        return NULL;
    }

    security_label_t* label = (security_label_t*)kmalloc(sizeof(security_label_t));
    if (!label) {
        return NULL;
    }

    // Initialize label
    strncpy(label->domain, domain, MAX_LABEL_NAME - 1);
    label->domain[MAX_LABEL_NAME - 1] = '\0';
    label->type = type;
    label->level = level;
    memset(label->categories, 0, sizeof(label->categories));
    label->category_count = 0;
    label->flags = LABEL_FLAG_ENFORCING;

    return label;
}

void mac_label_destroy(security_label_t* label) {
    if (label) {
        kfree(label);
    }
}

security_label_t* mac_label_copy(const security_label_t* label) {
    if (!label) {
        return NULL;
    }

    security_label_t* copy = (security_label_t*)kmalloc(sizeof(security_label_t));
    if (!copy) {
        return NULL;
    }

    memcpy(copy, label, sizeof(security_label_t));
    return copy;
}

// ============================================================================
// Category Management
// ============================================================================

int mac_label_add_category(security_label_t* label, uint32_t category) {
    if (!label || category >= (MAX_CATEGORIES * 32)) {
        return MAC_ERR_INVALID_ARG;
    }

    uint32_t word_idx = category / 32;
    uint32_t bit_idx = category % 32;

    if (!(label->categories[word_idx] & (1U << bit_idx))) {
        label->categories[word_idx] |= (1U << bit_idx);
        label->category_count++;
    }

    return MAC_SUCCESS;
}

int mac_label_remove_category(security_label_t* label, uint32_t category) {
    if (!label || category >= (MAX_CATEGORIES * 32)) {
        return MAC_ERR_INVALID_ARG;
    }

    uint32_t word_idx = category / 32;
    uint32_t bit_idx = category % 32;

    if (label->categories[word_idx] & (1U << bit_idx)) {
        label->categories[word_idx] &= ~(1U << bit_idx);
        label->category_count--;
    }

    return MAC_SUCCESS;
}

bool mac_label_has_category(const security_label_t* label, uint32_t category) {
    if (!label || category >= (MAX_CATEGORIES * 32)) {
        return false;
    }

    uint32_t word_idx = category / 32;
    uint32_t bit_idx = category % 32;

    return (label->categories[word_idx] & (1U << bit_idx)) != 0;
}

// ============================================================================
// Label Comparison
// ============================================================================

int mac_label_compare(const security_label_t* l1, const security_label_t* l2) {
    if (!l1 || !l2) {
        return -1;
    }

    // Compare domains
    int domain_cmp = strcmp(l1->domain, l2->domain);
    if (domain_cmp != 0) {
        return domain_cmp;
    }

    // Compare types
    if (l1->type != l2->type) {
        return l1->type - l2->type;
    }

    // Compare MLS levels
    if (l1->level != l2->level) {
        return l1->level - l2->level;
    }

    // Compare categories
    for (uint32_t i = 0; i < MAX_CATEGORIES; i++) {
        if (l1->categories[i] != l2->categories[i]) {
            return (l1->categories[i] > l2->categories[i]) ? 1 : -1;
        }
    }

    return 0;
}

// Check if l1 dominates l2 in MLS hierarchy
bool mac_label_dominates(const security_label_t* l1, const security_label_t* l2) {
    if (!l1 || !l2) {
        return false;
    }

    // l1 must have higher or equal security level
    if (l1->level < l2->level) {
        return false;
    }

    // l1 must contain all categories of l2
    for (uint32_t i = 0; i < MAX_CATEGORIES; i++) {
        if ((l2->categories[i] & l1->categories[i]) != l2->categories[i]) {
            return false;
        }
    }

    return true;
}

// ============================================================================
// Process Label Operations
// ============================================================================

int mac_process_set_label(struct process* proc, const security_label_t* label) {
    if (!proc || !label) {
        return MAC_ERR_INVALID_ARG;
    }

    // Check if current process has permission to change labels
    process_t* current = process_get_current();
    if (current && current != proc) {
        security_label_t* current_label = mac_process_get_label(current);
        if (current_label && !(current_label->flags & LABEL_FLAG_PRIVILEGED)) {
            return MAC_ERR_PERMISSION;
        }
    }

    // Allocate new label if process doesn't have one
    // Note: In real implementation, this would be stored in process_t
    // For now, we'll use a placeholder approach

    return MAC_SUCCESS;
}

security_label_t* mac_process_get_label(const struct process* proc) {
    if (!proc) {
        return NULL;
    }

    // For now, return a default label
    // In real implementation, this would be stored in process_t structure
    static security_label_t default_label = {
        .domain = "user_t",
        .type = LABEL_TYPE_USER,
        .level = MLS_LEVEL_UNCLASSIFIED,
        .category_count = 0,
        .flags = LABEL_FLAG_ENFORCING
    };

    return &default_label;
}

// ============================================================================
// Helper Functions
// ============================================================================

bool mac_is_valid_domain(const char* domain) {
    if (!domain) {
        return false;
    }

    size_t len = strlen(domain);
    if (len == 0 || len >= MAX_LABEL_NAME) {
        return false;
    }

    // Domain name must end with "_t" suffix
    if (len < 3 || strcmp(domain + len - 2, "_t") != 0) {
        return false;
    }

    // Check for valid characters (alphanumeric and underscore only)
    for (size_t i = 0; i < len; i++) {
        char c = domain[i];
        if (!(c >= 'a' && c <= 'z') &&
            !(c >= 'A' && c <= 'Z') &&
            !(c >= '0' && c <= '9') &&
            c != '_') {
            return false;
        }
    }

    return true;
}

bool mac_is_privileged_domain(const char* domain) {
    if (!domain) {
        return false;
    }

    return (strcmp(domain, MAC_DOMAIN_KERNEL) == 0 ||
            strcmp(domain, MAC_DOMAIN_INIT) == 0);
}

// ============================================================================
// MLS (Multi-Level Security) Functions
// ============================================================================

bool mac_mls_range_valid(mls_level_t low, mls_level_t high) {
    return low <= high && high <= MLS_LEVEL_TOP_SECRET;
}

// Bell-LaPadula: No read up
bool mac_mls_can_read(mls_level_t subject_level, mls_level_t object_level) {
    return subject_level >= object_level;
}

// Bell-LaPadula: No write down
bool mac_mls_can_write(mls_level_t subject_level, mls_level_t object_level) {
    return subject_level <= object_level;
}

// ============================================================================
// String Conversion Functions
// ============================================================================

const char* mac_perm_to_string(object_type_t type, uint32_t perm) {
    switch (type) {
        case OBJ_TYPE_FILE:
            if (perm & MAC_FILE_READ) return "read";
            if (perm & MAC_FILE_WRITE) return "write";
            if (perm & MAC_FILE_EXECUTE) return "execute";
            if (perm & MAC_FILE_APPEND) return "append";
            if (perm & MAC_FILE_CREATE) return "create";
            if (perm & MAC_FILE_DELETE) return "delete";
            if (perm & MAC_FILE_CHMOD) return "chmod";
            if (perm & MAC_FILE_CHOWN) return "chown";
            break;

        case OBJ_TYPE_PROCESS:
            if (perm & MAC_PROC_SIGNAL) return "signal";
            if (perm & MAC_PROC_PTRACE) return "ptrace";
            if (perm & MAC_PROC_KILL) return "kill";
            if (perm & MAC_PROC_FORK) return "fork";
            if (perm & MAC_PROC_EXEC) return "exec";
            break;

        case OBJ_TYPE_SOCKET:
            if (perm & MAC_NET_BIND) return "bind";
            if (perm & MAC_NET_CONNECT) return "connect";
            if (perm & MAC_NET_LISTEN) return "listen";
            break;

        default:
            break;
    }

    return "unknown";
}

const char* mac_object_type_to_string(object_type_t type) {
    switch (type) {
        case OBJ_TYPE_FILE:     return "file";
        case OBJ_TYPE_DIR:      return "dir";
        case OBJ_TYPE_SOCKET:   return "socket";
        case OBJ_TYPE_DEVICE:   return "device";
        case OBJ_TYPE_PROCESS:  return "process";
        case OBJ_TYPE_IPC_SHM:  return "shm";
        case OBJ_TYPE_IPC_MSG:  return "msg";
        case OBJ_TYPE_IPC_SEM:  return "sem";
        default:                return "unknown";
    }
}
