// kernel/core/sched/nice.c - Process priority management
#include "../../include/syscall.h"
#include "../../include/sched.h"
#include "../../include/kernel.h"

// Priority constants
#define PRIO_MIN -20  // Highest priority
#define PRIO_MAX 19   // Lowest priority
#define PRIO_DEFAULT 0

// sys_nice - Adjust process priority
// Args: pid = target process (0 for current), increment = priority adjustment
// Returns: New priority on success, negative error code on failure
//
// POSIX nice() adds to current priority, but we implement a more flexible
// version that allows setting absolute priority when pid is specified.
int64_t sys_nice(uint64_t pid, uint64_t increment, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;  // Unused

    process_t* current = process_get_current();
    process_t* target = NULL;

    // Get target process
    if (pid == 0) {
        target = current;
        if (!target) return ESRCH;          // no current process -> don't deref NULL
        process_ref(target);  // Take reference for consistency
    } else {
        target = process_get_by_pid((uint32_t)pid);
        if (!target) {
            kprintf("[NICE] Process %llu not found\n", pid);
            return ESRCH;
        }
    }

    // Convert increment to signed value
    int32_t inc = (int32_t)(int64_t)increment;

    // Calculate new priority
    int32_t new_priority;
    if (pid == 0) {
        // For current process, add to existing priority (POSIX nice behavior)
        new_priority = target->priority + inc;
    } else {
        // For other processes, set absolute priority (taskmanager use case)
        new_priority = inc;
    }

    // Clamp to valid range
    if (new_priority < PRIO_MIN) {
        new_priority = PRIO_MIN;
    } else if (new_priority > PRIO_MAX) {
        new_priority = PRIO_MAX;
    }

    // Permission: only the same UID (or root, uid 0) may change a process's
    // priority. With today's single-UID-0 model this always passes, but it closes
    // the hole where any unprivileged process could renice/boost any other.
    if (current && current->uid != 0 && current->uid != target->uid) {
        process_unref(target);
        return EPERM;
    }

    int32_t old_priority = target->priority;
    target->priority = new_priority;

    kprintf("[NICE] Process %u (%s): priority %d -> %d\n",
            target->pid, target->name, old_priority, new_priority);

    process_unref(target);
    return new_priority;
}

// sys_getpriority - Get process priority
// Args: which = PRIO_PROCESS/PRIO_PGRP/PRIO_USER, who = target ID
// Returns: Priority value (negated, 20-(-20) = 0-40) on success
//
// Note: This is a stub for future implementation
int64_t sys_getpriority(uint64_t which, uint64_t who, uint64_t arg3,
                        uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg3; (void)arg4; (void)arg5; (void)arg6;  // Unused

    // For now, only support PRIO_PROCESS
    if (which != 0) {  // PRIO_PROCESS = 0
        return EINVAL;
    }

    process_t* target;
    if (who == 0) {
        target = process_get_current();
        if (!target) return ESRCH;
        process_ref(target);
    } else {
        target = process_get_by_pid((uint32_t)who);
        if (!target) {
            return ESRCH;
        }
    }

    int32_t priority = target->priority;
    process_unref(target);

    // POSIX getpriority returns 20-prio to avoid negative values
    // (which would be indistinguishable from errors)
    return 20 - priority;
}

// sys_setpriority - Set process priority
// Args: which = PRIO_PROCESS/PRIO_PGRP/PRIO_USER, who = target ID, prio = new priority
// Returns: 0 on success, negative error code on failure
//
// Note: This is a stub for future implementation
int64_t sys_setpriority(uint64_t which, uint64_t who, uint64_t prio,
                        uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg4; (void)arg5; (void)arg6;  // Unused

    // For now, only support PRIO_PROCESS
    if (which != 0) {  // PRIO_PROCESS = 0
        return EINVAL;
    }

    process_t* target;
    process_t* setter = process_get_current();
    if (who == 0) {
        target = setter;
        if (!target) return ESRCH;
        process_ref(target);
    } else {
        target = process_get_by_pid((uint32_t)who);
        if (!target) {
            return ESRCH;
        }
    }

    // Permission: same UID (or root) only — see sys_nice.
    if (setter && setter->uid != 0 && setter->uid != target->uid) {
        process_unref(target);
        return EPERM;
    }

    int32_t new_priority = (int32_t)(int64_t)prio;

    // Clamp to valid range
    if (new_priority < PRIO_MIN) {
        new_priority = PRIO_MIN;
    } else if (new_priority > PRIO_MAX) {
        new_priority = PRIO_MAX;
    }

    target->priority = new_priority;

    kprintf("[NICE] Process %u (%s): priority set to %d\n",
            target->pid, target->name, new_priority);

    process_unref(target);
    return ESUCCESS;
}
