/*
 * Panic Handler Test Suite
 * =========================
 *
 * Tests the enhanced kernel panic handler to verify:
 * - Stack traces
 * - Register dumps
 * - Memory dumps
 * - Exception details
 * - Assertion failures
 */

#include "../../kernel/include/kernel.h"
#include "../../kernel/include/x86_64.h"

// Test functions with distinct call stack for tracing
static void level3_function(void) {
    // Trigger a kernel panic
    kernel_panic("Test panic from level3_function");
}

static void level2_function(void) {
    level3_function();
}

static void level1_function(void) {
    level2_function();
}

void test_panic_handler_basic(void) {
    kprintf("\n=== Test 1: Basic Panic Handler ===\n");
    kprintf("This will trigger a panic with stack trace...\n");

    level1_function();  // Should never return

    kprintf("ERROR: Returned from panic!\n");
}

void test_assertion_failure(void) {
    kprintf("\n=== Test 2: Assertion Failure ===\n");
    kprintf("Testing ASSERT macro...\n");

    int x = 5;
    ASSERT(x == 5);  // Should pass

    kprintf("First assertion passed.\n");

    ASSERT(x == 10);  // Should fail and trigger panic

    kprintf("ERROR: Should not reach here!\n");
}

void test_page_fault_simulation(void) {
    kprintf("\n=== Test 3: Page Fault Simulation ===\n");
    kprintf("Attempting to access unmapped kernel memory...\n");

    // Try to access an unmapped kernel address
    volatile uint64_t* bad_ptr = (uint64_t*)0xFFFF800000001000ULL;
    uint64_t value = *bad_ptr;  // Should trigger page fault

    kprintf("ERROR: Read succeeded (got %llu)!\n", value);
}

void test_null_dereference(void) {
    kprintf("\n=== Test 4: Null Pointer Dereference ===\n");
    kprintf("Attempting null pointer dereference...\n");

    volatile uint64_t* null_ptr = NULL;
    uint64_t value = *null_ptr;  // Should trigger page fault

    kprintf("ERROR: Null dereference succeeded (got %llu)!\n", value);
}

void test_divide_by_zero(void) {
    kprintf("\n=== Test 5: Divide by Zero ===\n");
    kprintf("Attempting division by zero...\n");

    volatile int x = 42;
    volatile int y = 0;
    volatile int z = x / y;  // Should trigger #DE exception

    kprintf("ERROR: Division succeeded (got %d)!\n", z);
}

void test_invalid_opcode(void) {
    kprintf("\n=== Test 6: Invalid Opcode ===\n");
    kprintf("Executing invalid instruction...\n");

    // Use inline assembly to execute an undefined opcode
    asm volatile("ud2");  // Undefined Instruction

    kprintf("ERROR: Invalid opcode did not fault!\n");
}

void test_stack_overflow(void) {
    kprintf("\n=== Test 7: Stack Overflow ===\n");
    kprintf("Triggering stack overflow via deep recursion...\n");

    // Recursive function to overflow stack
    test_stack_overflow();

    kprintf("ERROR: Recursion did not overflow!\n");
}

// Main test runner
void run_panic_handler_tests(void) {
    kprintf("\n");
    kprintf("================================================================================\n");
    kprintf("                    PANIC HANDLER TEST SUITE                                    \n");
    kprintf("================================================================================\n");
    kprintf("\n");
    kprintf("WARNING: These tests will intentionally trigger kernel panics!\n");
    kprintf("         Each test should display detailed diagnostic information.\n");
    kprintf("\n");

    // Uncomment ONE test at a time to run it
    // (Since panics halt the system, we can only run one per boot)

    // test_panic_handler_basic();
    // test_assertion_failure();
    // test_page_fault_simulation();
    // test_null_dereference();
    // test_divide_by_zero();
    // test_invalid_opcode();
    // test_stack_overflow();

    kprintf("\nAll tests require manual execution (uncomment one at a time).\n");
    kprintf("No test is currently active.\n");
}
