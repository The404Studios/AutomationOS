#include "../../include/capability.h"
#include "../../include/kernel.h"
#include "../../include/syscall.h"
#include "../../include/sched.h"

// String functions (external)
extern int strcmp(const char* s1, const char* s2);
extern int strncmp(const char* s1, const char* s2, size_t n);
extern size_t strlen(const char* str);

// Pattern matching helper function
// Supports * wildcard only (matches any sequence of characters)
static bool path_matches_pattern(const char* path, const char* pattern) {
    if (!path || !pattern) return false;

    while (*pattern) {
        if (*pattern == '*') {
            // Match any sequence
            pattern++;
            if (!*pattern) return true;  // Pattern ends with *, matches everything after

            // Try to match rest of pattern with remaining path
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

// Check if capability set has file access to specific path
bool capability_check_file(capability_set_t* set, const char* path, capability_type_t access) {
    if (!set || !path) {
        kprintf("[CAP] Invalid arguments to capability_check_file\n");
        return false;
    }

    // Refresh if stale
    if (set->generation < global_capability_generation) {
        capability_set_refresh(set);
    }

    // Scan for matching file capability
    capability_t* cap = set->head;
    while (cap) {
        if (cap->type == access) {
            // Check if path matches pattern
            if (path_matches_pattern(path, cap->data.file.path_pattern)) {
                kprintf("[CAP] File access granted: %s matches pattern %s\n",
                        path, cap->data.file.path_pattern);
                return true;
            }
        }
        cap = cap->next;
    }

    kprintf("[CAP] File access denied: %s (capability type %d not found)\n",
            path, access);
    return false;
}

// Host pattern matching (supports * wildcard)
static bool host_matches_pattern(const char* host, const char* pattern) {
    if (!host || !pattern) return false;

    // Special case: "*" matches any host
    if (strcmp(pattern, "*") == 0) return true;

    // Exact match
    if (strcmp(host, pattern) == 0) return true;

    // Wildcard pattern matching
    return path_matches_pattern(host, pattern);
}

// Check if capability set has network access
bool capability_check_net(capability_set_t* set, const char* host, uint16_t port,
                         capability_type_t access) {
    if (!set) {
        kprintf("[CAP] Invalid arguments to capability_check_net\n");
        return false;
    }

    // Refresh if stale
    if (set->generation < global_capability_generation) {
        capability_set_refresh(set);
    }

    // Scan for matching network capability
    capability_t* cap = set->head;
    while (cap) {
        if (cap->type == access) {
            // Check if host and port match
            bool host_match = host_matches_pattern(host, cap->data.net.host);
            bool port_match = false;

            // Port matching logic
            if (cap->data.net.port_max == 0) {
                // Single port or any port (if port_min == 0)
                port_match = (cap->data.net.port_min == 0 || cap->data.net.port_min == port);
            } else {
                // Port range
                port_match = (port >= cap->data.net.port_min && port <= cap->data.net.port_max);
            }

            if (host_match && port_match) {
                kprintf("[CAP] Network access granted: %s:%u matches pattern %s:%u-%u\n",
                        host, port, cap->data.net.host,
                        cap->data.net.port_min, cap->data.net.port_max);
                return true;
            }
        }
        cap = cap->next;
    }

    kprintf("[CAP] Network access denied: %s:%u (capability type %d not found)\n",
            host, port, access);
    return false;
}

// Check if capability set has device access
bool capability_check_device(capability_set_t* set, uint32_t device_id) {
    if (!set) {
        kprintf("[CAP] Invalid arguments to capability_check_device\n");
        return false;
    }

    // Refresh if stale
    if (set->generation < global_capability_generation) {
        capability_set_refresh(set);
    }

    // Scan for matching device capability
    capability_t* cap = set->head;
    while (cap) {
        if (cap->type == CAP_DEVICE_ACCESS || cap->type == CAP_GPU ||
            cap->type == CAP_AUDIO || cap->type == CAP_USB || cap->type == CAP_SERIAL) {

            // Check if device ID matches
            if (cap->data.device.device_id == 0xFFFFFFFF ||  // All devices
                cap->data.device.device_id == device_id) {
                kprintf("[CAP] Device access granted: device_id 0x%x\n", device_id);
                return true;
            }
        }
        cap = cap->next;
    }

    kprintf("[CAP] Device access denied: device_id 0x%x\n", device_id);
    return false;
}

// Check if capability set has IPC access to target process
bool capability_check_ipc(capability_set_t* set, uint32_t target_pid) {
    if (!set) {
        kprintf("[CAP] Invalid arguments to capability_check_ipc\n");
        return false;
    }

    // Refresh if stale
    if (set->generation < global_capability_generation) {
        capability_set_refresh(set);
    }

    // Check for broadcast capability (allows IPC to any process)
    if (capability_has(set, CAP_IPC_BROADCAST)) {
        kprintf("[CAP] IPC broadcast capability allows access to PID %u\n", target_pid);
        return true;
    }

    // Scan for matching IPC capability
    capability_t* cap = set->head;
    while (cap) {
        if (cap->type == CAP_IPC) {
            // Check if target PID matches
            if (cap->data.ipc.target_pid == 0 ||  // Any process
                cap->data.ipc.target_pid == target_pid) {
                kprintf("[CAP] IPC access granted to PID %u\n", target_pid);
                return true;
            }
        }
        cap = cap->next;
    }

    kprintf("[CAP] IPC access denied to PID %u\n", target_pid);
    return false;
}

// Syscall capability checking dispatcher
bool syscall_check_capability(int syscall_num, capability_set_t* caps, void* args) {
    if (!caps) {
        kprintf("[CAP] No capability set provided for syscall %d\n", syscall_num);
        return false;
    }

    // Refresh if stale
    if (caps->generation < global_capability_generation) {
        capability_set_refresh(caps);
    }

    switch (syscall_num) {
        case SYS_READ:
            // For now, allow read without capability check
            // TODO: Check FD-specific capabilities once file descriptors are implemented
            return true;

        case SYS_WRITE:
            // For now, allow write to stdout/stderr without capability check
            // TODO: Check FD-specific capabilities
            return true;

        case SYS_OPEN:
            // TODO: Extract path and flags from args, check CAP_FILE_READ or CAP_FILE_WRITE
            kprintf("[CAP] sys_open capability check not yet implemented\n");
            return true;  // Temporarily allow

        case SYS_FORK:
            // Fork requires no special capability (inherits from parent)
            return true;

        case SYS_EXIT:
            // Exit requires no capability
            return true;

        case SYS_GETPID:
            // Getting own PID requires no capability
            return true;

        case SYS_CLOSE:
            // Closing FDs requires no capability
            return true;

        case SYS_WAITPID:
            // Waiting for child processes requires no capability
            return true;

        case SYS_EXECVE:
            // TODO: Check executable file capabilities
            kprintf("[CAP] sys_execve capability check not yet implemented\n");
            return true;  // Temporarily allow

        case SYS_SLEEP:
            // Sleep requires no capability
            return true;

        default:
            kprintf("[CAP] Unknown syscall %d, denying by default\n", syscall_num);
            return false;
    }
}
