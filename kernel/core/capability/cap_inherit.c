#include "../../include/capability.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/sched.h"

// Default inheritance mask - which capabilities are inherited by child processes
// By default: file, network, and IPC capabilities are inherited
// System and process capabilities are NOT inherited
#define DEFAULT_INHERIT_MASK ( \
    (1ULL << CAP_FILE_READ) | \
    (1ULL << CAP_FILE_WRITE) | \
    (1ULL << CAP_FILE_EXECUTE) | \
    (1ULL << CAP_FILE_CREATE) | \
    (1ULL << CAP_NET_BIND) | \
    (1ULL << CAP_NET_CONNECT) | \
    (1ULL << CAP_NET_LISTEN) | \
    (1ULL << CAP_IPC) | \
    (1ULL << CAP_IPC_RECEIVE) \
)

// Create default capability set for init process
capability_set_t* capability_create_init_set(void) {
    kprintf("[CAP] Creating init process capability set...\n");

    capability_set_t* set = capability_set_create();
    if (!set) {
        kprintf("[CAP] Failed to create init capability set\n");
        return NULL;
    }

    // Init process gets all capabilities with PERMANENT flag
    capability_t* caps[] = {
        capability_create_file(CAP_FILE_READ, "/*", CAP_FLAG_PERMANENT | CAP_FLAG_INHERITABLE | CAP_FLAG_DELEGATABLE),
        capability_create_file(CAP_FILE_WRITE, "/*", CAP_FLAG_PERMANENT | CAP_FLAG_INHERITABLE | CAP_FLAG_DELEGATABLE),
        capability_create_file(CAP_FILE_EXECUTE, "/*", CAP_FLAG_PERMANENT | CAP_FLAG_INHERITABLE | CAP_FLAG_DELEGATABLE),
        capability_create_file(CAP_FILE_CREATE, "/*", CAP_FLAG_PERMANENT | CAP_FLAG_INHERITABLE | CAP_FLAG_DELEGATABLE),
        capability_create_file(CAP_FILE_DELETE, "/*", CAP_FLAG_PERMANENT | CAP_FLAG_INHERITABLE | CAP_FLAG_DELEGATABLE),
        capability_create_net(CAP_NET_BIND, "*", 0, 65535, CAP_FLAG_PERMANENT | CAP_FLAG_INHERITABLE | CAP_FLAG_DELEGATABLE),
        capability_create_net(CAP_NET_CONNECT, "*", 0, 65535, CAP_FLAG_PERMANENT | CAP_FLAG_INHERITABLE | CAP_FLAG_DELEGATABLE),
        capability_create_device(0xFFFFFFFF, "all", CAP_FLAG_PERMANENT | CAP_FLAG_INHERITABLE | CAP_FLAG_DELEGATABLE),
        capability_create_simple(CAP_IPC_BROADCAST, CAP_FLAG_PERMANENT | CAP_FLAG_INHERITABLE | CAP_FLAG_DELEGATABLE),
        capability_create_simple(CAP_SYS_ADMIN, CAP_FLAG_PERMANENT | CAP_FLAG_DELEGATABLE),
        capability_create_simple(CAP_SYS_MODULE, CAP_FLAG_PERMANENT | CAP_FLAG_DELEGATABLE),
        capability_create_simple(CAP_PROCESS_KILL, CAP_FLAG_PERMANENT | CAP_FLAG_DELEGATABLE),
    };

    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        if (caps[i] && capability_add(set, caps[i]) != CAP_SUCCESS) {
            kprintf("[CAP] Failed to add capability to init set\n");
            capability_destroy(caps[i]);
        }
    }

    kprintf("[CAP] Init capability set created with %u capabilities\n", set->count);
    return set;
}

// Create minimal capability set for userspace processes
capability_set_t* capability_create_user_set(const char* process_name) {
    kprintf("[CAP] Creating userspace capability set for '%s'...\n", process_name);

    capability_set_t* set = capability_set_create();
    if (!set) {
        kprintf("[CAP] Failed to create userspace capability set\n");
        return NULL;
    }

    // Userspace processes get minimal capabilities
    // Read/write access to /tmp and /home/<user>
    // Network access to common ports
    capability_t* caps[] = {
        capability_create_file(CAP_FILE_READ, "/tmp/*", CAP_FLAG_INHERITABLE),
        capability_create_file(CAP_FILE_WRITE, "/tmp/*", CAP_FLAG_INHERITABLE),
        capability_create_file(CAP_FILE_READ, "/home/*", CAP_FLAG_INHERITABLE),
        capability_create_file(CAP_FILE_WRITE, "/home/*", CAP_FLAG_INHERITABLE),
        capability_create_net(CAP_NET_CONNECT, "*", 80, 443, CAP_FLAG_INHERITABLE),
        capability_create_simple(CAP_IPC_RECEIVE, CAP_FLAG_INHERITABLE),
    };

    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        if (caps[i] && capability_add(set, caps[i]) != CAP_SUCCESS) {
            kprintf("[CAP] Failed to add capability to userspace set\n");
            capability_destroy(caps[i]);
        }
    }

    kprintf("[CAP] Userspace capability set created with %u capabilities\n", set->count);
    return set;
}

// Inherit capabilities from parent process (called during fork)
capability_set_t* capability_inherit_from_parent(process_t* parent) {
    if (!parent || !parent->capabilities) {
        kprintf("[CAP] No parent capabilities to inherit\n");
        return capability_set_create();
    }

    kprintf("[CAP] Inheriting capabilities from parent PID %u\n", parent->pid);

    // Use default inheritance mask
    capability_set_t* child_caps = capability_inherit(parent->capabilities, DEFAULT_INHERIT_MASK);

    if (child_caps) {
        kprintf("[CAP] Child inherited %u capabilities from parent\n", child_caps->count);

        // Audit log
        audit_log_capability(parent, CAP_NONE, AUDIT_CAP_INHERITED,
                           "child process inherited capabilities");
    }

    return child_caps;
}

// Reset capabilities for exec (load new program)
capability_set_t* capability_reset_for_exec(process_t* proc, const char* executable_path) {
    if (!proc) {
        kprintf("[CAP] Invalid process for exec capability reset\n");
        return NULL;
    }

    kprintf("[CAP] Resetting capabilities for exec of '%s'\n", executable_path);

    // TODO: Load capabilities from executable manifest
    // For now, create a minimal set based on executable path

    capability_set_t* new_caps = capability_set_create();
    if (!new_caps) {
        kprintf("[CAP] Failed to create exec capability set\n");
        return NULL;
    }

    // Grant minimal capabilities based on executable location
    if (strncmp(executable_path, "/bin/", 5) == 0 ||
        strncmp(executable_path, "/sbin/", 6) == 0) {
        // System binaries get broader access
        capability_t* caps[] = {
            capability_create_file(CAP_FILE_READ, "/*", CAP_FLAG_INHERITABLE),
            capability_create_file(CAP_FILE_WRITE, "/tmp/*", CAP_FLAG_INHERITABLE),
            capability_create_net(CAP_NET_CONNECT, "*", 0, 65535, CAP_FLAG_INHERITABLE),
            capability_create_simple(CAP_IPC_RECEIVE, CAP_FLAG_INHERITABLE),
        };

        for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
            if (caps[i]) capability_add(new_caps, caps[i]);
        }
    } else {
        // User programs get restricted access
        capability_t* caps[] = {
            capability_create_file(CAP_FILE_READ, "/tmp/*", CAP_FLAG_INHERITABLE),
            capability_create_file(CAP_FILE_WRITE, "/tmp/*", CAP_FLAG_INHERITABLE),
            capability_create_file(CAP_FILE_READ, "/home/*", CAP_FLAG_INHERITABLE),
            capability_create_net(CAP_NET_CONNECT, "*", 80, 443, CAP_FLAG_INHERITABLE),
        };

        for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
            if (caps[i]) capability_add(new_caps, caps[i]);
        }
    }

    kprintf("[CAP] Exec capability set created with %u capabilities\n", new_caps->count);
    return new_caps;
}

// Drop capabilities (used for privilege separation)
int capability_drop_privileged(capability_set_t* set) {
    if (!set) return CAP_EINVAL;

    kprintf("[CAP] Dropping privileged capabilities...\n");

    // Remove system and administrative capabilities
    capability_remove(set, CAP_SYS_ADMIN);
    capability_remove(set, CAP_SYS_MODULE);
    capability_remove(set, CAP_SYS_TIME);
    capability_remove(set, CAP_SYS_BOOT);
    capability_remove(set, CAP_SYS_PTRACE);
    capability_remove(set, CAP_PROCESS_KILL);
    capability_remove(set, CAP_PROCESS_SETUID);
    capability_remove(set, CAP_PROCESS_SETGID);

    kprintf("[CAP] Privileged capabilities dropped\n");
    return CAP_SUCCESS;
}

// Elevate capabilities (used for privilege escalation - requires auth)
int capability_elevate_privileged(capability_set_t* set, uint32_t auth_token) {
    if (!set) return CAP_EINVAL;

    // TODO: Verify auth_token (password, biometric, etc.)
    kprintf("[CAP] Capability elevation requested (auth_token: 0x%x)\n", auth_token);

    // For now, deny elevation (not implemented)
    kprintf("[CAP] Capability elevation denied: not implemented\n");
    return CAP_EPERM;
}
