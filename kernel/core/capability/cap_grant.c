#include "../../include/capability.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/sched.h"

// String functions (external)
extern char* strncpy(char* dest, const char* src, size_t n);
extern size_t strlen(const char* str);
extern void* memset(void* s, int c, size_t n);

// Helper: Create file capability
capability_t* capability_create_file(capability_type_t type, const char* path_pattern,
                                    uint64_t flags) {
    if (type < CAP_FILE_READ || type > CAP_FILE_CHMOD) {
        kprintf("[CAP] Invalid file capability type: %d\n", type);
        return NULL;
    }

    capability_t* cap = (capability_t*)kmalloc(sizeof(capability_t));
    if (!cap) {
        kprintf("[CAP] Failed to allocate file capability\n");
        return NULL;
    }

    memset(cap, 0, sizeof(capability_t));
    cap->type = type;
    cap->flags = flags;
    cap->ref_count = 1;
    cap->next = NULL;

    strncpy(cap->data.file.path_pattern, path_pattern,
            sizeof(cap->data.file.path_pattern) - 1);
    cap->data.file.path_pattern[sizeof(cap->data.file.path_pattern) - 1] = '\0';

    kprintf("[CAP] Created file capability: type=%d, pattern='%s', flags=0x%llx\n",
            type, cap->data.file.path_pattern, flags);

    return cap;
}

// Helper: Create network capability
capability_t* capability_create_net(capability_type_t type, const char* host,
                                   uint16_t port_min, uint16_t port_max, uint64_t flags) {
    if (type < CAP_NET_BIND || type > CAP_NET_LISTEN) {
        kprintf("[CAP] Invalid network capability type: %d\n", type);
        return NULL;
    }

    capability_t* cap = (capability_t*)kmalloc(sizeof(capability_t));
    if (!cap) {
        kprintf("[CAP] Failed to allocate network capability\n");
        return NULL;
    }

    memset(cap, 0, sizeof(capability_t));
    cap->type = type;
    cap->flags = flags;
    cap->ref_count = 1;
    cap->next = NULL;

    strncpy(cap->data.net.host, host, sizeof(cap->data.net.host) - 1);
    cap->data.net.host[sizeof(cap->data.net.host) - 1] = '\0';
    cap->data.net.port_min = port_min;
    cap->data.net.port_max = port_max;

    kprintf("[CAP] Created network capability: type=%d, host='%s', ports=%u-%u, flags=0x%llx\n",
            type, cap->data.net.host, port_min, port_max, flags);

    return cap;
}

// Helper: Create device capability
capability_t* capability_create_device(uint32_t device_id, const char* device_class,
                                      uint64_t flags) {
    capability_t* cap = (capability_t*)kmalloc(sizeof(capability_t));
    if (!cap) {
        kprintf("[CAP] Failed to allocate device capability\n");
        return NULL;
    }

    memset(cap, 0, sizeof(capability_t));
    cap->type = CAP_DEVICE_ACCESS;
    cap->flags = flags;
    cap->ref_count = 1;
    cap->next = NULL;

    cap->data.device.device_id = device_id;
    strncpy(cap->data.device.device_class, device_class,
            sizeof(cap->data.device.device_class) - 1);
    cap->data.device.device_class[sizeof(cap->data.device.device_class) - 1] = '\0';

    kprintf("[CAP] Created device capability: device_id=0x%x, class='%s', flags=0x%llx\n",
            device_id, cap->data.device.device_class, flags);

    return cap;
}

// Helper: Create IPC capability
capability_t* capability_create_ipc(uint32_t target_pid, uint64_t flags) {
    capability_t* cap = (capability_t*)kmalloc(sizeof(capability_t));
    if (!cap) {
        kprintf("[CAP] Failed to allocate IPC capability\n");
        return NULL;
    }

    memset(cap, 0, sizeof(capability_t));
    cap->type = CAP_IPC;
    cap->flags = flags;
    cap->ref_count = 1;
    cap->next = NULL;

    cap->data.ipc.target_pid = target_pid;

    kprintf("[CAP] Created IPC capability: target_pid=%u, flags=0x%llx\n",
            target_pid, flags);

    return cap;
}

// Helper: Create simple capability (no constraints)
capability_t* capability_create_simple(capability_type_t type, uint64_t flags) {
    capability_t* cap = (capability_t*)kmalloc(sizeof(capability_t));
    if (!cap) {
        kprintf("[CAP] Failed to allocate simple capability\n");
        return NULL;
    }

    memset(cap, 0, sizeof(capability_t));
    cap->type = type;
    cap->flags = flags;
    cap->ref_count = 1;
    cap->next = NULL;

    kprintf("[CAP] Created simple capability: type=%d, flags=0x%llx\n", type, flags);

    return cap;
}

// Destroy capability
void capability_destroy(capability_t* cap) {
    if (!cap) return;

    cap->ref_count--;
    if (cap->ref_count == 0) {
        kprintf("[CAP] Destroying capability type %d\n", cap->type);
        kfree(cap);
    }
}

// Grant capability to another process (delegation)
int capability_delegate(process_t* granter, process_t* grantee,
                       capability_type_t type, const char* constraint) {
    if (!granter || !grantee) {
        return CAP_EINVAL;
    }

    kprintf("[CAP] Process %u delegating capability %d to process %u\n",
            granter->pid, type, grantee->pid);

    // Find capability in granter's set
    capability_t* granter_cap = granter->capabilities->head;
    while (granter_cap) {
        if (granter_cap->type == type) {
            // Found capability, check if it can be delegated
            if (!(granter_cap->flags & CAP_FLAG_DELEGATABLE)) {
                kprintf("[CAP] Capability %d is not delegatable\n", type);
                return CAP_EPERM;
            }

            // Clone capability for grantee
            capability_t* new_cap = (capability_t*)kmalloc(sizeof(capability_t));
            if (!new_cap) {
                return CAP_ENOMEM;
            }

            memcpy(new_cap, granter_cap, sizeof(capability_t));
            new_cap->ref_count = 1;
            new_cap->next = NULL;

            // Apply constraint if provided
            if (constraint && (type >= CAP_FILE_READ && type <= CAP_FILE_CHMOD)) {
                if (capability_restrict(new_cap, constraint) != CAP_SUCCESS) {
                    kfree(new_cap);
                    return CAP_EPERM;
                }
            }

            // Add to grantee's capability set
            int result = capability_add(grantee->capabilities, new_cap);
            if (result != CAP_SUCCESS) {
                kfree(new_cap);
                return result;
            }

            kprintf("[CAP] Successfully delegated capability %d from PID %u to PID %u\n",
                    type, granter->pid, grantee->pid);

            // Audit log
            audit_log_capability(grantee, type, AUDIT_CAP_DELEGATED, "capability delegated");

            return CAP_SUCCESS;
        }
        granter_cap = granter_cap->next;
    }

    kprintf("[CAP] Process %u does not have capability %d to delegate\n",
            granter->pid, type);
    return CAP_ENOTFOUND;
}

// Grant capability from kernel (administrative grant)
int capability_kernel_grant(process_t* proc, capability_t* cap) {
    if (!proc || !cap) {
        return CAP_EINVAL;
    }

    kprintf("[CAP] Kernel granting capability %d to process %u\n", cap->type, proc->pid);

    int result = capability_add(proc->capabilities, cap);
    if (result == CAP_SUCCESS) {
        audit_log_capability(proc, cap->type, AUDIT_CAP_GRANTED,
                           "kernel administrative grant");
    }

    return result;
}

// Revoke capability (global revocation)
int capability_global_revoke(capability_type_t type, const char* constraint) {
    kprintf("[CAP] Global revocation of capability %d (constraint: %s)\n",
            type, constraint ? constraint : "none");

    // TODO: Implement global revocation across all processes
    // For now, just increment generation counter
    global_capability_generation++;

    kprintf("[CAP] Global capability generation incremented to %u\n",
            global_capability_generation);

    return CAP_SUCCESS;
}

// Audit log stub (actual implementation would be in audit.c)
void audit_log_capability(process_t* proc, capability_type_t cap_type,
                         audit_event_t event, const char* details) {
    if (!proc) return;

    const char* event_str;
    switch (event) {
        case AUDIT_CAP_GRANTED:   event_str = "GRANTED"; break;
        case AUDIT_CAP_REVOKED:   event_str = "REVOKED"; break;
        case AUDIT_CAP_DENIED:    event_str = "DENIED"; break;
        case AUDIT_CAP_INHERITED: event_str = "INHERITED"; break;
        case AUDIT_CAP_DELEGATED: event_str = "DELEGATED"; break;
        default: event_str = "UNKNOWN"; break;
    }

    kprintf("[AUDIT] PID %u: capability %d %s - %s\n",
            proc->pid, cap_type, event_str, details);

    // TODO: Write to persistent audit log
}
