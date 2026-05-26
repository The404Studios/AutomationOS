#include "../include/kernel.h"
#include "../include/x86_64.h"

void kernel_panic(const char* message) {
    cli();  // Disable interrupts

    kprintf("\n\n");
    kprintf("====================================\n");
    kprintf("       KERNEL PANIC                 \n");
    kprintf("====================================\n");
    kprintf("%s\n", message);
    kprintf("====================================\n");
    kprintf("System halted.\n");

    // Halt forever
    while (1) {
        hlt();
    }
}
