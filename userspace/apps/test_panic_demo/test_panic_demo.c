/*
 * Panic Handler Demonstration Program
 * ====================================
 *
 * This program demonstrates the enhanced kernel panic handler by
 * intentionally triggering various types of faults and errors.
 *
 * Each test is designed to showcase different panic handler features:
 *  - Stack trace unwinding
 *  - Register dumps
 *  - Memory dumps around fault addresses
 *  - Page fault details
 *  - Assertion failures
 *
 * Usage: Run with argument to select test:
 *   test_panic_demo 1  - Null pointer dereference
 *   test_panic_demo 2  - Invalid memory access
 *   test_panic_demo 3  - Stack overflow (deep recursion)
 *   test_panic_demo 4  - Division by zero
 *   test_panic_demo 5  - Assertion failure demo
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test 1: Null pointer dereference */
void test_null_dereference(void) {
    printf("Test 1: Triggering null pointer dereference...\n");
    printf("Expected: Page fault with detailed diagnostic information\n\n");

    volatile int *ptr = NULL;
    *ptr = 42;  // This will cause a page fault

    printf("ERROR: Should not reach here!\n");
}

/* Test 2: Invalid memory access */
void test_invalid_access(void) {
    printf("Test 2: Accessing unmapped memory...\n");
    printf("Expected: Page fault showing unmapped address\n\n");

    // Try to access kernel space from user mode
    volatile int *kernel_ptr = (int*)0xFFFF800000001000ULL;
    int value = *kernel_ptr;

    printf("ERROR: Read succeeded (got %d)!\n", value);
}

/* Test 3: Stack overflow via recursion */
int recursive_function(int depth) {
    char buffer[1024];  // Consume stack space

    // Fill buffer to prevent optimization
    memset(buffer, depth & 0xFF, sizeof(buffer));

    printf("Recursion depth: %d\n", depth);

    // Recurse infinitely until stack overflows
    return recursive_function(depth + 1) + buffer[0];
}

void test_stack_overflow(void) {
    printf("Test 3: Triggering stack overflow...\n");
    printf("Expected: Page fault or stack-related exception\n\n");

    recursive_function(0);

    printf("ERROR: Should not reach here!\n");
}

/* Test 4: Division by zero */
void test_divide_by_zero(void) {
    printf("Test 4: Division by zero...\n");
    printf("Expected: Division exception (#DE)\n\n");

    volatile int x = 42;
    volatile int y = 0;
    volatile int z = x / y;

    printf("ERROR: Division succeeded (got %d)!\n", z);
}

/* Test 5: Simulate assertion failure */
void level3_function(int value) {
    printf("  Level 3: value=%d (expecting 42)\n", value);

    // This simulates an assertion failure
    if (value != 42) {
        printf("\nASSERTION FAILED: value == 42\n");
        printf("  Expected: 42\n");
        printf("  Got: %d\n", value);

        // Trigger a deliberate crash to show panic handler
        volatile int *crash = NULL;
        *crash = 0;
    }
}

void level2_function(int value) {
    printf("  Level 2: calling level 3...\n");
    level3_function(value);
}

void level1_function(int value) {
    printf("  Level 1: calling level 2...\n");
    level2_function(value);
}

void test_assertion_failure(void) {
    printf("Test 5: Assertion failure with stack trace...\n");
    printf("Expected: Full stack trace showing call chain\n\n");

    level1_function(99);  // Wrong value to trigger assertion

    printf("ERROR: Should not reach here!\n");
}

/* Main menu and test selector */
void print_menu(void) {
    printf("╔════════════════════════════════════════════════════════╗\n");
    printf("║       Kernel Panic Handler Demonstration              ║\n");
    printf("╚════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("Available tests:\n");
    printf("  1 - Null pointer dereference\n");
    printf("  2 - Invalid memory access (kernel space)\n");
    printf("  3 - Stack overflow (deep recursion)\n");
    printf("  4 - Division by zero\n");
    printf("  5 - Assertion failure with stack trace\n");
    printf("\n");
    printf("Usage: test_panic_demo <test_number>\n");
    printf("\n");
    printf("WARNING: Each test will crash the system!\n");
    printf("         This is intentional to demonstrate panic handling.\n");
    printf("\n");
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        print_menu();
        return 1;
    }

    int test_num = atoi(argv[1]);

    printf("\n");
    printf("════════════════════════════════════════════════════════\n");
    printf("  PANIC HANDLER DEMONSTRATION\n");
    printf("════════════════════════════════════════════════════════\n");
    printf("\n");

    switch (test_num) {
        case 1:
            test_null_dereference();
            break;
        case 2:
            test_invalid_access();
            break;
        case 3:
            test_stack_overflow();
            break;
        case 4:
            test_divide_by_zero();
            break;
        case 5:
            test_assertion_failure();
            break;
        default:
            printf("Invalid test number: %d\n", test_num);
            print_menu();
            return 1;
    }

    return 0;
}
