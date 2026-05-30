#include "../../include/rlimit.h"
#include "../../include/syscall.h"
#include "../../include/sched.h"
#include "../../include/kernel.h"

// sys_setrlimit - Set resource limit for current process
// Args: resource = resource type, rlim = pointer to rlimit_t structure
int64_t sys_setrlimit(uint64_t resource, uint64_t rlim, uint64_t arg3,
                      uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;  // Unused

    process_t* current = process_get_current();
    if (!current || !current->rlimits) {
        return ESRCH;
    }

    // Validate resource type
    if (resource >= RLIMIT_MAX) {
        return EINVAL;
    }

    // Validate pointer
    if (!rlim) {
        return EFAULT;
    }

    rlimit_t* new_limit = (rlimit_t*)rlim;

    // TODO: Check capability for raising hard limit (requires CAP_SYS_RESOURCE)
    // For now, allow any process to set its own limits

    // Set the limit
    int result = rlimit_set(current->rlimits, (rlimit_type_t)resource, new_limit);
    if (result != 0) {
        return EINVAL;
    }

    return ESUCCESS;
}

// sys_getrlimit - Get resource limit for current process
// Args: resource = resource type, rlim = pointer to rlimit_t structure
int64_t sys_getrlimit(uint64_t resource, uint64_t rlim, uint64_t arg3,
                      uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;  // Unused

    process_t* current = process_get_current();
    if (!current || !current->rlimits) {
        return ESRCH;
    }

    // Validate resource type
    if (resource >= RLIMIT_MAX) {
        return EINVAL;
    }

    // Validate pointer
    if (!rlim) {
        return EFAULT;
    }

    rlimit_t* out = (rlimit_t*)rlim;

    // Get the limit
    int result = rlimit_get(current->rlimits, (rlimit_type_t)resource, out);
    if (result != 0) {
        return EINVAL;
    }

    return ESUCCESS;
}

// sys_getrusage - Get resource usage statistics
// Args: who = RUSAGE_SELF (0) or RUSAGE_CHILDREN (1), usage = pointer to rusage_t
#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN 1

int64_t sys_getrusage(uint64_t who, uint64_t usage, uint64_t arg3,
                      uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;  // Unused

    process_t* current = process_get_current();
    if (!current || !current->rlimits) {
        return ESRCH;
    }

    // Validate pointer
    if (!usage) {
        return EFAULT;
    }

    rusage_t* out = (rusage_t*)usage;

    // Only support RUSAGE_SELF for now
    if (who != RUSAGE_SELF) {
        return EINVAL;
    }

    // Get usage statistics
    rlimit_get_usage(current->rlimits, out);

    return ESUCCESS;
}

// sys_prlimit - Set/get resource limits for another process (privileged)
// Args: pid = target process, resource = resource type,
//       new_limit = pointer to new limit (or NULL),
//       old_limit = pointer to receive old limit (or NULL)
int64_t sys_prlimit(uint64_t pid, uint64_t resource, uint64_t new_limit,
                    uint64_t old_limit, uint64_t arg5, uint64_t arg6) {
    (void)arg5; (void)arg6;  // Unused

    // Get target process
    process_t* target = NULL;
    if (pid == 0) {
        target = process_get_current();
    } else {
        // RACE-002 fix: process_get_by_pid() now takes reference
        target = process_get_by_pid((uint32_t)pid);
    }

    if (!target || !target->rlimits) {
        if (target && pid != 0) {
            process_unref(target);  // Release reference if we took one
        }
        return ESRCH;
    }

    // Validate resource type
    if (resource >= RLIMIT_MAX) {
        if (pid != 0) process_unref(target);
        return EINVAL;
    }

    // TODO: Check capability (requires CAP_SYS_RESOURCE or same UID)
    process_t* current = process_get_current();
    if (current->pid != target->pid) {
        // For now, only allow process to modify its own limits
        if (pid != 0) process_unref(target);
        return EINVAL;
    }

    // Get old limit if requested
    if (old_limit) {
        rlimit_t* old = (rlimit_t*)old_limit;
        int result = rlimit_get(target->rlimits, (rlimit_type_t)resource, old);
        if (result != 0) {
            if (pid != 0) process_unref(target);
            return EINVAL;
        }
    }

    // Set new limit if provided
    if (new_limit) {
        rlimit_t* new = (rlimit_t*)new_limit;
        int result = rlimit_set(target->rlimits, (rlimit_type_t)resource, new);
        if (result != 0) {
            if (pid != 0) process_unref(target);
            return EINVAL;
        }
    }

    // RACE-002 fix: Release reference before returning
    if (pid != 0) {
        process_unref(target);
    }

    return ESUCCESS;
}
