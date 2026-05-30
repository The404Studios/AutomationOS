// MAC Enforcement Hooks - Policy Decision Engine
#include "../../include/mac.h"
#include "../../include/kernel.h"
#include "../../include/sched.h"
#include "../../include/mem.h"

// String functions
extern int strcmp(const char* s1, const char* s2);
extern int strncmp(const char* s1, const char* s2, size_t n);
extern size_t strlen(const char* str);

// ============================================================================
// Statistics
// ============================================================================

static mac_stats_t global_stats;

void mac_stats_get(mac_stats_t* stats) {
    if (stats) {
        *stats = global_stats;
    }
}

void mac_stats_reset(void) {
    global_stats.checks_total = 0;
    global_stats.checks_allowed = 0;
    global_stats.checks_denied = 0;
    global_stats.label_changes = 0;
    global_stats.policy_loads = 0;
    global_stats.transitions = 0;
    global_stats.cache_hits = 0;
    global_stats.cache_misses = 0;
}

// ============================================================================
// Core Access Check Function
// ============================================================================

static int mac_check_permission(const process_t* proc, const char* target_domain,
                                object_type_t obj_type, uint32_t requested_perms) {
    global_stats.checks_total++;

    if (!mac_is_enforcing()) {
        global_stats.checks_allowed++;
        return MAC_SUCCESS;
    }

    if (!proc) {
        global_stats.checks_denied++;
        return MAC_ERR_DENIED;
    }

    // Get process security label
    security_label_t* subject = mac_process_get_label(proc);
    if (!subject) {
        global_stats.checks_denied++;
        return MAC_ERR_INVALID_LABEL;
    }

    // Kernel domain has full access
    if (strcmp(subject->domain, MAC_DOMAIN_KERNEL) == 0) {
        global_stats.checks_allowed++;
        return MAC_SUCCESS;
    }

    // Find matching policy rule
    mac_rule_t* rule = mac_policy_find_rule(subject->domain, target_domain, obj_type);

    if (!rule) {
        // Try wildcard target
        rule = mac_policy_find_rule(subject->domain, "*", obj_type);
    }

    if (rule) {
        // Check if this is an explicit deny rule
        if (rule->flags & RULE_FLAG_DENY) {
            global_stats.checks_denied++;
            if (rule->flags & RULE_FLAG_AUDIT) {
                mac_audit_denial(proc, target_domain, obj_type, requested_perms);
            }
            return MAC_ERR_DENIED;
        }

        // Check if requested permissions are allowed
        if ((rule->permissions & requested_perms) == requested_perms) {
            global_stats.checks_allowed++;
            if (rule->flags & RULE_FLAG_AUDIT) {
                mac_audit_access(proc, target_domain, obj_type, requested_perms, true);
            }
            return MAC_SUCCESS;
        }
    }

    // Default deny - no rule found or insufficient permissions
    global_stats.checks_denied++;
    mac_audit_denial(proc, target_domain, obj_type, requested_perms);

    return MAC_ERR_DENIED;
}

// ============================================================================
// File Access Checks
// ============================================================================

static const char* get_file_type(const char* path) {
    if (!path) {
        return MAC_TYPE_FILE;
    }

    // Simple path-based type inference
    if (strncmp(path, "/etc/shadow", 11) == 0) {
        return MAC_TYPE_SHADOW;
    } else if (strncmp(path, "/etc/", 5) == 0) {
        return MAC_TYPE_ETC;
    } else if (strncmp(path, "/bin/", 5) == 0 || strncmp(path, "/sbin/", 6) == 0) {
        return MAC_TYPE_BIN;
    } else if (strncmp(path, "/lib/", 5) == 0) {
        return MAC_TYPE_LIB;
    } else if (strncmp(path, "/dev/", 5) == 0) {
        return MAC_TYPE_DEV;
    } else if (strncmp(path, "/tmp/", 5) == 0) {
        return MAC_TYPE_TMP;
    } else if (strncmp(path, "/home/", 6) == 0) {
        return MAC_TYPE_HOME;
    }

    return MAC_TYPE_FILE;
}

int mac_check_file_open(const process_t* proc, const char* path, uint32_t mode) {
    uint32_t perms = 0;

    if (mode & 0x01) perms |= MAC_FILE_READ;
    if (mode & 0x02) perms |= MAC_FILE_WRITE;
    if (mode & 0x04) perms |= MAC_FILE_CREATE;

    const char* file_type = get_file_type(path);
    return mac_check_permission(proc, file_type, OBJ_TYPE_FILE, perms);
}

int mac_check_file_read(const process_t* proc, const char* path) {
    const char* file_type = get_file_type(path);
    return mac_check_permission(proc, file_type, OBJ_TYPE_FILE, MAC_FILE_READ);
}

int mac_check_file_write(const process_t* proc, const char* path) {
    const char* file_type = get_file_type(path);
    return mac_check_permission(proc, file_type, OBJ_TYPE_FILE, MAC_FILE_WRITE);
}

int mac_check_file_execute(const process_t* proc, const char* path) {
    const char* file_type = get_file_type(path);
    return mac_check_permission(proc, file_type, OBJ_TYPE_FILE, MAC_FILE_EXECUTE);
}

int mac_check_file_create(const process_t* proc, const char* path) {
    const char* file_type = get_file_type(path);
    return mac_check_permission(proc, file_type, OBJ_TYPE_FILE, MAC_FILE_CREATE);
}

int mac_check_file_delete(const process_t* proc, const char* path) {
    const char* file_type = get_file_type(path);
    return mac_check_permission(proc, file_type, OBJ_TYPE_FILE, MAC_FILE_DELETE);
}

int mac_check_file_chmod(const process_t* proc, const char* path) {
    const char* file_type = get_file_type(path);
    return mac_check_permission(proc, file_type, OBJ_TYPE_FILE, MAC_FILE_CHMOD);
}

int mac_check_file_chown(const process_t* proc, const char* path) {
    const char* file_type = get_file_type(path);
    return mac_check_permission(proc, file_type, OBJ_TYPE_FILE, MAC_FILE_CHOWN);
}

// ============================================================================
// Network Access Checks
// ============================================================================

static const char* get_port_type(uint16_t port) {
    if (port < 1024) {
        // Reserved ports
        if (port == 80 || port == 443 || port == 8080) {
            return MAC_TYPE_PORT_HTTP;
        } else if (port == 22) {
            return MAC_TYPE_PORT_SSH;
        }
        return MAC_TYPE_PORT_RESERVED;
    }
    return "unrestricted_port_t";
}

int mac_check_net_bind(const process_t* proc, uint16_t port) {
    const char* port_type = get_port_type(port);
    return mac_check_permission(proc, port_type, OBJ_TYPE_SOCKET, MAC_NET_BIND);
}

int mac_check_net_connect(const process_t* proc, const char* host, uint16_t port) {
    const char* port_type = get_port_type(port);
    return mac_check_permission(proc, port_type, OBJ_TYPE_SOCKET, MAC_NET_CONNECT);
}

int mac_check_net_listen(const process_t* proc, uint16_t port) {
    const char* port_type = get_port_type(port);
    return mac_check_permission(proc, port_type, OBJ_TYPE_SOCKET, MAC_NET_LISTEN);
}

int mac_check_net_raw(const process_t* proc) {
    return mac_check_permission(proc, "raw_socket_t", OBJ_TYPE_SOCKET, MAC_NET_RAW);
}

// ============================================================================
// Process Access Checks
// ============================================================================

int mac_check_process_signal(const process_t* source, const process_t* target) {
    if (!source || !target) {
        return MAC_ERR_INVALID_ARG;
    }

    security_label_t* target_label = mac_process_get_label(target);
    if (!target_label) {
        return MAC_ERR_INVALID_LABEL;
    }

    return mac_check_permission(source, target_label->domain, OBJ_TYPE_PROCESS,
                               MAC_PROC_SIGNAL);
}

int mac_check_process_ptrace(const process_t* source, const process_t* target) {
    if (!source || !target) {
        return MAC_ERR_INVALID_ARG;
    }

    security_label_t* target_label = mac_process_get_label(target);
    if (!target_label) {
        return MAC_ERR_INVALID_LABEL;
    }

    return mac_check_permission(source, target_label->domain, OBJ_TYPE_PROCESS,
                               MAC_PROC_PTRACE);
}

int mac_check_process_kill(const process_t* source, const process_t* target) {
    if (!source || !target) {
        return MAC_ERR_INVALID_ARG;
    }

    security_label_t* target_label = mac_process_get_label(target);
    if (!target_label) {
        return MAC_ERR_INVALID_LABEL;
    }

    return mac_check_permission(source, target_label->domain, OBJ_TYPE_PROCESS,
                               MAC_PROC_KILL);
}

int mac_check_process_fork(const process_t* proc) {
    if (!proc) {
        return MAC_ERR_INVALID_ARG;
    }

    security_label_t* label = mac_process_get_label(proc);
    if (!label) {
        return MAC_ERR_INVALID_LABEL;
    }

    return mac_check_permission(proc, label->domain, OBJ_TYPE_PROCESS, MAC_PROC_FORK);
}

int mac_check_process_exec(const process_t* proc, const char* path) {
    if (!proc || !path) {
        return MAC_ERR_INVALID_ARG;
    }

    // First check if we can execute the file
    int ret = mac_check_file_execute(proc, path);
    if (ret != MAC_SUCCESS) {
        return ret;
    }

    // Check process exec permission
    security_label_t* label = mac_process_get_label(proc);
    if (!label) {
        return MAC_ERR_INVALID_LABEL;
    }

    return mac_check_permission(proc, label->domain, OBJ_TYPE_PROCESS, MAC_PROC_EXEC);
}

// ============================================================================
// IPC Access Checks
// ============================================================================

int mac_check_ipc_create(const process_t* proc, object_type_t ipc_type) {
    if (!proc) {
        return MAC_ERR_INVALID_ARG;
    }

    return mac_check_permission(proc, "ipc_t", ipc_type, MAC_IPC_CREATE);
}

int mac_check_ipc_access(const process_t* proc, uint32_t ipc_id, uint32_t perms) {
    if (!proc) {
        return MAC_ERR_INVALID_ARG;
    }

    // In a full implementation, we would look up the IPC object and get its label
    return mac_check_permission(proc, "ipc_t", OBJ_TYPE_IPC_SHM, perms);
}

// ============================================================================
// System Access Checks
// ============================================================================

int mac_check_load_module(const process_t* proc, const char* module_path) {
    if (!proc || !module_path) {
        return MAC_ERR_INVALID_ARG;
    }

    // Loading kernel modules requires privileged domain
    security_label_t* label = mac_process_get_label(proc);
    if (!label) {
        return MAC_ERR_INVALID_LABEL;
    }

    if (!mac_is_privileged_domain(label->domain)) {
        mac_audit_denial(proc, "kernel_module_t", OBJ_TYPE_FILE, MAC_FILE_READ);
        return MAC_ERR_DENIED;
    }

    return MAC_SUCCESS;
}

int mac_check_set_time(const process_t* proc) {
    if (!proc) {
        return MAC_ERR_INVALID_ARG;
    }

    return mac_check_permission(proc, "clock_t", OBJ_TYPE_DEVICE, MAC_FILE_WRITE);
}

int mac_check_reboot(const process_t* proc) {
    if (!proc) {
        return MAC_ERR_INVALID_ARG;
    }

    security_label_t* label = mac_process_get_label(proc);
    if (!label) {
        return MAC_ERR_INVALID_LABEL;
    }

    if (!mac_is_privileged_domain(label->domain)) {
        mac_audit_denial(proc, "system_t", OBJ_TYPE_PROCESS, 0);
        return MAC_ERR_DENIED;
    }

    return MAC_SUCCESS;
}

int mac_check_mount(const process_t* proc, const char* dev, const char* path) {
    if (!proc || !dev || !path) {
        return MAC_ERR_INVALID_ARG;
    }

    return mac_check_permission(proc, "filesystem_t", OBJ_TYPE_FILE, MAC_FILE_WRITE);
}

// ============================================================================
// System Call Implementations
// ============================================================================

int sys_mac_load_policy(const void* policy_data, size_t size) {
    process_t* current = process_get_current();
    if (!current) {
        return MAC_ERR_PERMISSION;
    }

    // Check permission to load policy
    security_label_t* label = mac_process_get_label(current);
    if (!label || !mac_is_privileged_domain(label->domain)) {
        return MAC_ERR_PERMISSION;
    }

    // Validate user buffer
    if (!validate_user_buffer(policy_data, size)) {
        return MAC_ERR_INVALID_ARG;
    }

    // Copy policy data to kernel space
    void* kernel_buffer = kmalloc(size);
    if (!kernel_buffer) {
        return MAC_ERR_NO_MEMORY;
    }

    if (copy_from_user(kernel_buffer, policy_data, size) != COPY_SUCCESS) {
        kfree(kernel_buffer);
        return MAC_ERR_INVALID_ARG;
    }

    int ret = mac_policy_load(kernel_buffer, size);
    kfree(kernel_buffer);

    return ret;
}

int sys_mac_get_label(uint32_t pid, security_label_t* label) {
    if (!label || !validate_user_buffer(label, sizeof(security_label_t))) {
        return MAC_ERR_INVALID_ARG;
    }

    // RACE-002 fix: process_get_by_pid() now takes reference
    process_t* target = process_get_by_pid(pid);
    if (!target) {
        return MAC_ERR_NOT_FOUND;
    }

    security_label_t* target_label = mac_process_get_label(target);
    if (!target_label) {
        process_unref(target);  // Release reference
        return MAC_ERR_INVALID_LABEL;
    }

    if (copy_to_user(label, target_label, sizeof(security_label_t)) != COPY_SUCCESS) {
        process_unref(target);  // Release reference
        return MAC_ERR_INVALID_ARG;
    }

    // RACE-002 fix: Release reference before returning
    process_unref(target);
    return MAC_SUCCESS;
}

int sys_mac_set_label(uint32_t pid, const security_label_t* label) {
    if (!label || !validate_user_buffer(label, sizeof(security_label_t))) {
        return MAC_ERR_INVALID_ARG;
    }

    process_t* current = process_get_current();
    if (!current) {
        return MAC_ERR_PERMISSION;
    }

    // Check permission
    security_label_t* current_label = mac_process_get_label(current);
    if (!current_label || !(current_label->flags & LABEL_FLAG_PRIVILEGED)) {
        return MAC_ERR_PERMISSION;
    }

    // RACE-002 fix: process_get_by_pid() now takes reference
    process_t* target = process_get_by_pid(pid);
    if (!target) {
        return MAC_ERR_NOT_FOUND;
    }

    security_label_t kernel_label;
    if (copy_from_user(&kernel_label, label, sizeof(security_label_t)) != COPY_SUCCESS) {
        process_unref(target);  // Release reference
        return MAC_ERR_INVALID_ARG;
    }

    int result = mac_process_set_label(target, &kernel_label);

    // RACE-002 fix: Release reference before returning
    process_unref(target);
    return result;
}

int sys_mac_check_access(const char* path, uint32_t perms) {
    process_t* current = process_get_current();
    if (!current) {
        return MAC_ERR_PERMISSION;
    }

    if (!path || !validate_user_string(path, 256)) {
        return MAC_ERR_INVALID_ARG;
    }

    const char* file_type = get_file_type(path);
    return mac_check_permission(current, file_type, OBJ_TYPE_FILE, perms);
}

int sys_mac_set_enforcing(bool enforcing) {
    process_t* current = process_get_current();
    if (!current) {
        return MAC_ERR_PERMISSION;
    }

    security_label_t* label = mac_process_get_label(current);
    if (!label || !mac_is_privileged_domain(label->domain)) {
        return MAC_ERR_PERMISSION;
    }

    mac_set_enforcing(enforcing);
    return MAC_SUCCESS;
}

int sys_mac_get_stats(void* stats_buffer, size_t size) {
    if (!stats_buffer || size < sizeof(mac_stats_t)) {
        return MAC_ERR_INVALID_ARG;
    }

    if (!validate_user_buffer(stats_buffer, size)) {
        return MAC_ERR_INVALID_ARG;
    }

    mac_stats_t stats;
    mac_stats_get(&stats);

    if (copy_to_user(stats_buffer, &stats, sizeof(mac_stats_t)) != COPY_SUCCESS) {
        return MAC_ERR_INVALID_ARG;
    }

    return MAC_SUCCESS;
}
