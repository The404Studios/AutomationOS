#include "../../include/syscall.h"
#include "../../include/kernel.h"
#include "../../include/sched.h"

// External serial driver functions
extern void serial_write(const char* str, size_t len);

// SYS_EXIT - Terminate calling process
int64_t sys_exit(uint64_t status, uint64_t arg2, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    process_t* current = process_get_current();
    if (!current) {
        kprintf("[SYSCALL] sys_exit: No current process\n");
        return ESRCH;
    }

    kprintf("[SYSCALL] sys_exit: Process '%s' (PID %d) exiting with status %d\n",
            current->name, current->pid, (int)status);

    // Mark process as terminated
    current->state = PROCESS_TERMINATED;

    // Remove from scheduler
    scheduler_remove_process(current);

    // Schedule next process
    // Note: This will not return to this process
    schedule();

    // Should never reach here
    return ESUCCESS;
}

// SYS_FORK - Create child process (stub)
int64_t sys_fork(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    kprintf("[SYSCALL] sys_fork: Not yet implemented\n");
    return ENOTSUP;
}

// SYS_READ - Read from file descriptor
int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count,
                 uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg4; (void)arg5; (void)arg6;

    kprintf("[SYSCALL] sys_read: fd=%d buf=%p count=%d\n",
            (int)fd, (void*)buf, (int)count);

    // Basic validation
    if (buf == 0 || count == 0) {
        return EINVAL;
    }

    // For now, only support stdin (fd 0)
    if (fd != 0) {
        return EBADF;
    }

    // TODO: Implement actual keyboard input
    kprintf("[SYSCALL] sys_read: Not yet implemented\n");
    return ENOTSUP;
}

// SYS_WRITE - Write to file descriptor
int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count,
                  uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg4; (void)arg5; (void)arg6;

    kprintf("[SYSCALL] sys_write: fd=%d buf=%p count=%d\n",
            (int)fd, (void*)buf, (int)count);

    // Basic validation
    if (buf == 0 || count == 0) {
        return EINVAL;
    }

    // For now, only support stdout (fd 1) and stderr (fd 2)
    if (fd != 1 && fd != 2) {
        return EBADF;
    }

    // Write to serial console
    const char* str = (const char*)buf;
    serial_write(str, (size_t)count);

    return (int64_t)count;  // Return number of bytes written
}

// SYS_GETPID - Get process ID
int64_t sys_getpid(uint64_t arg1, uint64_t arg2, uint64_t arg3,
                   uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5; (void)arg6;

    process_t* current = process_get_current();
    if (!current) {
        kprintf("[SYSCALL] sys_getpid: No current process\n");
        return ESRCH;
    }

    kprintf("[SYSCALL] sys_getpid: Returning PID %d\n", current->pid);

    return (int64_t)current->pid;
}
