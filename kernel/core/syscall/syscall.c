#include "../../include/syscall.h"
#include "../../include/kernel.h"

// Syscall handler table
static syscall_handler_t syscall_table[MAX_SYSCALLS];

void syscall_init(void) {
    kprintf("[SYSCALL] Initializing system call interface...\n");

    // Clear syscall table
    for (int i = 0; i < MAX_SYSCALLS; i++) {
        syscall_table[i] = NULL;
    }

    // Register syscall handlers
    syscall_table[SYS_EXIT] = sys_exit;
    syscall_table[SYS_FORK] = sys_fork;
    syscall_table[SYS_READ] = sys_read;
    syscall_table[SYS_WRITE] = sys_write;
    syscall_table[SYS_GETPID] = sys_getpid;

    kprintf("[SYSCALL] Registered %d syscalls\n", 5);
    kprintf("[SYSCALL] System call interface initialized\n");
}

int64_t syscall_dispatch(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    // Validate syscall number
    if (syscall_num >= MAX_SYSCALLS) {
        kprintf("[SYSCALL] Invalid syscall number: %u\n", (uint32_t)syscall_num);
        return EINVAL;
    }

    // Check if handler exists
    if (syscall_table[syscall_num] == NULL) {
        kprintf("[SYSCALL] Unimplemented syscall: %u\n", (uint32_t)syscall_num);
        return ENOTSUP;
    }

    // Log syscall (for debugging)
    kprintf("[SYSCALL] Dispatching syscall %u\n", (uint32_t)syscall_num);

    // Call the handler
    syscall_handler_t handler = syscall_table[syscall_num];
    int64_t result = handler(arg1, arg2, arg3, arg4, arg5, arg6);

    kprintf("[SYSCALL] Syscall %u returned %d\n", (uint32_t)syscall_num, (int)result);

    return result;
}
