/**
 * /init - First User Mode Program
 * ================================
 *
 * Simple init process to test ELF loader.
 * This is the first program that runs in user mode.
 */

// For now, just infinite loop
// Later will have proper syscalls

void _start(void) {
    // Entry point for user mode
    // Later: print "Hello from user mode!"
    // Later: call sys_write(1, "Hello!\n", 7)

    // For now, just loop
    while (1) {
        // Later: sys_yield() to give up CPU
        asm volatile("hlt");
    }
}
