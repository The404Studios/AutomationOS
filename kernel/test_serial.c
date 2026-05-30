/*
 * Minimal test program for serial driver and kprintf
 * Compile: x86_64-elf-gcc -ffreestanding -nostdlib -mno-red-zone -c test_serial.c -o test_serial.o
 */

#include "include/kernel.h"
#include "include/drivers.h"
#include "include/types.h"

void test_serial_driver(void) {
    // Initialize serial port
    serial_init();

    // Test basic string output
    kprintf("=== Serial Driver Test ===\n");
    kprintf("Hello from AutomationOS!\n");
    kprintf("\n");

    // Test format specifiers
    kprintf("String: %s\n", "Test string");
    kprintf("Decimal: %d\n", 42);
    kprintf("Negative: %d\n", -123);
    kprintf("Unsigned: %u\n", 4294967295U);
    kprintf("Hex: %x\n", 0xDEADBEEF);
    kprintf("Pointer: %p\n", (void*)0xFFFFFFFF80000000ULL);
    kprintf("Percent: %%\n");
    kprintf("\n");

    // Test null string
    kprintf("Null string: %s\n", NULL);

    // Test multiple arguments
    kprintf("Multiple: %s = %d (0x%x)\n", "answer", 42, 42);

    kprintf("\n=== Test Complete ===\n");
}

// Note: This test can't be run standalone as it needs the full kernel environment.
// This is just for compilation verification.
