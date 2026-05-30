#include "../../include/capability.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/sched.h"

// Global capability generation counter for revocation
uint32_t global_capability_generation = 0;

// String functions (external)
extern int strcmp(const char* s1, const char* s2);
extern size_t strlen(const char* str);
extern char* strncpy(char* dest, const char* src, size_t n);

void capability_init(void) {
    kprintf("[CAP] Initializing capability system...\n");
    global_capability_generation = 1;
    kprintf("[CAP] Capability system initialized (generation: %u)\n",
            global_capability_generation);
}

capability_set_t* capability_set_create(void) {
    capability_set_t* set = (capability_set_t*)kmalloc(sizeof(capability_set_t));
    if (!set) {
        kprintf("[CAP] Failed to allocate capability set\n");
        return NULL;
    }

    set->head = NULL;
    set->count = 0;
    set->bitmask = 0;
    set->generation = global_capability_generation;

    return set;
}

void capability_set_destroy(capability_set_t* set) {
    if (!set) return;

    capability_t* cap = set->head;
    while (cap) {
        capability_t* next = cap->next;
        capability_destroy(cap);
        cap = next;
    }

    kfree(set);
}

capability_set_t* capability_set_clone(capability_set_t* set) {
    if (!set) return NULL;

    capability_set_t* new_set = capability_set_create();
    if (!new_set) return NULL;

    // Clone all capabilities
    capability_t* cap = set->head;
    while (cap) {
        capability_t* new_cap = (capability_t*)kmalloc(sizeof(capability_t));
        if (!new_cap) {
            capability_set_destroy(new_set);
            return NULL;
        }

        // Copy capability data
        memcpy(new_cap, cap, sizeof(capability_t));
        new_cap->ref_count = 1;
        new_cap->next = NULL;

        // Add to new set
        if (capability_add(new_set, new_cap) != CAP_SUCCESS) {
            kfree(new_cap);
            capability_set_destroy(new_set);
            return NULL;
        }

        cap = cap->next;
    }

    return new_set;
}

void capability_set_refresh(capability_set_t* set) {
    if (!set) return;

    // Update generation counter
    if (set->generation < global_capability_generation) {
        kprintf("[CAP] Refreshing capability set (gen %u -> %u)\n",
                set->generation, global_capability_generation);
        set->generation = global_capability_generation;

        // TODO: Remove revoked capabilities
        // For now, just update generation
    }
}

int capability_add(capability_set_t* set, capability_t* cap) {
    if (!set || !cap) {
        return CAP_EINVAL;
    }

    // Check if capability already exists
    capability_t* existing = set->head;
    while (existing) {
        if (existing->type == cap->type) {
            // Check if it's the same capability (same constraints)
            if (existing->type >= CAP_FILE_READ && existing->type <= CAP_FILE_CHMOD) {
                if (strcmp(existing->data.file.path_pattern, cap->data.file.path_pattern) == 0) {
                    return CAP_EDUP;
                }
            } else if (existing->type >= CAP_NET_BIND && existing->type <= CAP_NET_LISTEN) {
                if (strcmp(existing->data.net.host, cap->data.net.host) == 0 &&
                    existing->data.net.port_min == cap->data.net.port_min &&
                    existing->data.net.port_max == cap->data.net.port_max) {
                    return CAP_EDUP;
                }
            } else {
                // Simple capability - only one instance allowed
                return CAP_EDUP;
            }
        }
        existing = existing->next;
    }

    // Add to linked list
    cap->next = set->head;
    set->head = cap;
    set->count++;

    // Update bitmask for fast lookup (only for types 0-63)
    if (cap->type < 64) {
        set->bitmask |= (1ULL << cap->type);
    }

    kprintf("[CAP] Added capability type %d (count: %u)\n", cap->type, set->count);
    return CAP_SUCCESS;
}

int capability_remove(capability_set_t* set, capability_type_t type) {
    if (!set) {
        return CAP_EINVAL;
    }

    capability_t** cap_ptr = &set->head;
    bool found = false;

    while (*cap_ptr) {
        if ((*cap_ptr)->type == type) {
            capability_t* to_free = *cap_ptr;
            *cap_ptr = (*cap_ptr)->next;
            capability_destroy(to_free);
            set->count--;
            found = true;

            kprintf("[CAP] Removed capability type %d (count: %u)\n", type, set->count);

            // Continue to remove all instances of this type
        } else {
            cap_ptr = &(*cap_ptr)->next;
        }
    }

    if (!found) {
        return CAP_ENOTFOUND;
    }

    // Update bitmask - need to scan list to see if any capabilities of this type remain
    if (type < 64) {
        set->bitmask &= ~(1ULL << type);
        capability_t* cap = set->head;
        while (cap) {
            if (cap->type == type) {
                set->bitmask |= (1ULL << type);
                break;
            }
            cap = cap->next;
        }
    }

    return CAP_SUCCESS;
}

int capability_grant(process_t* granter, process_t* grantee, capability_t* cap) {
    if (!granter || !grantee || !cap) {
        return CAP_EINVAL;
    }

    // Check if granter has the capability
    if (!capability_has(granter->capabilities, cap->type)) {
        kprintf("[CAP] Process %u does not have capability %d to grant\n",
                granter->pid, cap->type);
        return CAP_EPERM;
    }

    // Check if capability is delegatable
    if (!capability_can_delegate(cap)) {
        kprintf("[CAP] Capability %d is not delegatable\n", cap->type);
        return CAP_EPERM;
    }

    // Clone capability and add to grantee
    capability_t* new_cap = (capability_t*)kmalloc(sizeof(capability_t));
    if (!new_cap) {
        return CAP_ENOMEM;
    }

    memcpy(new_cap, cap, sizeof(capability_t));
    new_cap->ref_count = 1;
    new_cap->next = NULL;

    int result = capability_add(grantee->capabilities, new_cap);
    if (result != CAP_SUCCESS) {
        kfree(new_cap);
        return result;
    }

    kprintf("[CAP] Process %u granted capability %d to process %u\n",
            granter->pid, cap->type, grantee->pid);

    // Audit log
    audit_log_capability(grantee, cap->type, AUDIT_CAP_GRANTED, "delegated");

    return CAP_SUCCESS;
}

int capability_revoke(process_t* proc, capability_type_t type) {
    if (!proc) {
        return CAP_EINVAL;
    }

    int result = capability_remove(proc->capabilities, type);
    if (result == CAP_SUCCESS) {
        kprintf("[CAP] Revoked capability %d from process %u\n", type, proc->pid);

        // Increment global generation counter
        global_capability_generation++;

        // Audit log
        audit_log_capability(proc, type, AUDIT_CAP_REVOKED, "administratively revoked");
    }

    return result;
}

int capability_revoke_all(process_t* proc) {
    if (!proc || !proc->capabilities) {
        return CAP_EINVAL;
    }

    kprintf("[CAP] Revoking all capabilities from process %u\n", proc->pid);

    capability_set_destroy(proc->capabilities);
    proc->capabilities = capability_set_create();

    // Increment global generation counter
    global_capability_generation++;

    return CAP_SUCCESS;
}

bool capability_has(capability_set_t* set, capability_type_t type) {
    if (!set) return false;

    // Refresh if stale
    if (set->generation < global_capability_generation) {
        capability_set_refresh(set);
    }

    // Fast check using bitmask (for simple capabilities)
    if (type < 64) {
        return (set->bitmask & (1ULL << type)) != 0;
    }

    // Slow path: search linked list
    capability_t* cap = set->head;
    while (cap) {
        if (cap->type == type) return true;
        cap = cap->next;
    }

    return false;
}

capability_set_t* capability_inherit(capability_set_t* parent, uint64_t inherit_mask) {
    if (!parent) {
        return capability_set_create();
    }

    capability_set_t* child = capability_set_create();
    if (!child) return NULL;

    // Copy inheritable capabilities
    capability_t* cap = parent->head;
    while (cap) {
        // Check if capability is inheritable
        if ((cap->flags & CAP_FLAG_INHERITABLE) &&
            (cap->type < 64 && (inherit_mask & (1ULL << cap->type)))) {

            capability_t* new_cap = (capability_t*)kmalloc(sizeof(capability_t));
            if (!new_cap) {
                capability_set_destroy(child);
                return NULL;
            }

            memcpy(new_cap, cap, sizeof(capability_t));
            new_cap->ref_count = 1;
            new_cap->next = NULL;

            if (capability_add(child, new_cap) != CAP_SUCCESS) {
                kfree(new_cap);
                capability_set_destroy(child);
                return NULL;
            }
        }
        cap = cap->next;
    }

    kprintf("[CAP] Inherited %u capabilities (mask: 0x%llx)\n",
            child->count, inherit_mask);

    return child;
}

bool capability_can_delegate(capability_t* cap) {
    if (!cap) return false;
    return (cap->flags & CAP_FLAG_DELEGATABLE) != 0;
}

int capability_restrict(capability_t* cap, const char* pattern) {
    if (!cap || !pattern) {
        return CAP_EINVAL;
    }

    // Only file capabilities support restriction
    if (cap->type < CAP_FILE_READ || cap->type > CAP_FILE_CHMOD) {
        kprintf("[CAP] Capability type %d does not support restriction\n", cap->type);
        return CAP_EINVAL;
    }

    // Verify that new pattern is more restrictive than old pattern
    // For simplicity, we just require that new pattern starts with old pattern
    size_t old_len = strlen(cap->data.file.path_pattern);
    if (strncmp(cap->data.file.path_pattern, pattern, old_len) != 0) {
        kprintf("[CAP] New pattern '%s' is not more restrictive than '%s'\n",
                pattern, cap->data.file.path_pattern);
        return CAP_EPERM;
    }

    // Update pattern
    strncpy(cap->data.file.path_pattern, pattern, sizeof(cap->data.file.path_pattern) - 1);
    cap->data.file.path_pattern[sizeof(cap->data.file.path_pattern) - 1] = '\0';

    kprintf("[CAP] Restricted capability to pattern: %s\n", pattern);
    return CAP_SUCCESS;
}
