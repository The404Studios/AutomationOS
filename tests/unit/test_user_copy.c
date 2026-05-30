#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/include/mem.h"
#include "../../kernel/include/kernel.h"

// Mock kprintf for testing
int kprintf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    int ret = vprintf(format, args);
    va_end(args);
    return ret;
}

// Test copy_from_user with valid user space address
void test_copy_from_user_valid(void) {
    printf("Test: copy_from_user() with valid user address\n");

    // User space buffer (simulated - in real kernel this would be < 0x0000800000000000)
    char user_buf[] = "Hello from user space!";
    char kernel_buf[64];

    // This should succeed (addresses below USER_SPACE_END)
    int result = copy_from_user(kernel_buf, user_buf, sizeof(user_buf));

    assert(result == COPY_SUCCESS);
    assert(strcmp(kernel_buf, user_buf) == 0);

    printf("  PASS: copy_from_user() succeeded with valid user address\n");
}

// Test copy_from_user with kernel space address (should fail)
void test_copy_from_user_kernel_address(void) {
    printf("Test: copy_from_user() with kernel address\n");

    // Kernel space address (>= 0xFFFF800000000000)
    void* kernel_addr = (void*)0xFFFFFFFF80000000ULL;
    char kernel_buf[64];

    // This should fail - source is in kernel space
    int result = copy_from_user(kernel_buf, kernel_addr, 10);

    assert(result == COPY_EFAULT);

    printf("  PASS: copy_from_user() rejected kernel address\n");
}

// Test copy_from_user with NULL pointer
void test_copy_from_user_null(void) {
    printf("Test: copy_from_user() with NULL pointer\n");

    char kernel_buf[64];

    // NULL source
    int result = copy_from_user(kernel_buf, NULL, 10);
    assert(result == COPY_EFAULT);

    // NULL destination
    char user_buf[] = "test";
    result = copy_from_user(NULL, user_buf, 10);
    assert(result == COPY_EFAULT);

    printf("  PASS: copy_from_user() rejected NULL pointers\n");
}

// Test copy_from_user with zero size
void test_copy_from_user_zero_size(void) {
    printf("Test: copy_from_user() with zero size\n");

    char user_buf[] = "test";
    char kernel_buf[64];

    int result = copy_from_user(kernel_buf, user_buf, 0);
    assert(result == COPY_EFAULT);

    printf("  PASS: copy_from_user() rejected zero size\n");
}

// Test copy_from_user with address overflow
void test_copy_from_user_overflow(void) {
    printf("Test: copy_from_user() with address overflow\n");

    char kernel_buf[64];
    // Address that would overflow when adding size
    void* user_addr = (void*)(0xFFFFFFFFFFFFFFFFULL - 5);

    int result = copy_from_user(kernel_buf, user_addr, 10);
    assert(result == COPY_EFAULT);

    printf("  PASS: copy_from_user() detected address overflow\n");
}

// Test copy_from_user with address crossing user/kernel boundary
void test_copy_from_user_boundary_cross(void) {
    printf("Test: copy_from_user() crossing user/kernel boundary\n");

    char kernel_buf[64];
    // Start just before user space end, but extend into kernel space
    void* user_addr = (void*)(USER_SPACE_END - 5);

    int result = copy_from_user(kernel_buf, user_addr, 10);
    assert(result == COPY_EFAULT);

    printf("  PASS: copy_from_user() rejected boundary-crossing address\n");
}

// Test copy_to_user with valid user space address
void test_copy_to_user_valid(void) {
    printf("Test: copy_to_user() with valid user address\n");

    char kernel_buf[] = "Hello from kernel!";
    char user_buf[64];

    // This should succeed (addresses below USER_SPACE_END)
    int result = copy_to_user(user_buf, kernel_buf, sizeof(kernel_buf));

    assert(result == COPY_SUCCESS);
    assert(strcmp(user_buf, kernel_buf) == 0);

    printf("  PASS: copy_to_user() succeeded with valid user address\n");
}

// Test copy_to_user with kernel space address (should fail)
void test_copy_to_user_kernel_address(void) {
    printf("Test: copy_to_user() with kernel address\n");

    char kernel_buf[] = "test";
    // Kernel space address
    void* kernel_addr = (void*)0xFFFFFFFF80000000ULL;

    // This should fail - destination is in kernel space
    int result = copy_to_user(kernel_addr, kernel_buf, 10);

    assert(result == COPY_EFAULT);

    printf("  PASS: copy_to_user() rejected kernel address\n");
}

// Test copy_to_user with NULL pointer
void test_copy_to_user_null(void) {
    printf("Test: copy_to_user() with NULL pointer\n");

    char kernel_buf[] = "test";

    // NULL destination
    int result = copy_to_user(NULL, kernel_buf, 10);
    assert(result == COPY_EFAULT);

    char user_buf[64];
    // NULL source
    result = copy_to_user(user_buf, NULL, 10);
    assert(result == COPY_EFAULT);

    printf("  PASS: copy_to_user() rejected NULL pointers\n");
}

// Test copy_to_user with zero size
void test_copy_to_user_zero_size(void) {
    printf("Test: copy_to_user() with zero size\n");

    char kernel_buf[] = "test";
    char user_buf[64];

    int result = copy_to_user(user_buf, kernel_buf, 0);
    assert(result == COPY_EFAULT);

    printf("  PASS: copy_to_user() rejected zero size\n");
}

// Test copy_to_user with address overflow
void test_copy_to_user_overflow(void) {
    printf("Test: copy_to_user() with address overflow\n");

    char kernel_buf[] = "test";
    // Address that would overflow when adding size
    void* user_addr = (void*)(0xFFFFFFFFFFFFFFFFULL - 5);

    int result = copy_to_user(user_addr, kernel_buf, 10);
    assert(result == COPY_EFAULT);

    printf("  PASS: copy_to_user() detected address overflow\n");
}

// Test copy_to_user with address crossing user/kernel boundary
void test_copy_to_user_boundary_cross(void) {
    printf("Test: copy_to_user() crossing user/kernel boundary\n");

    char kernel_buf[] = "test";
    // Start just before user space end, but extend into kernel space
    void* user_addr = (void*)(USER_SPACE_END - 5);

    int result = copy_to_user(user_addr, kernel_buf, 10);
    assert(result == COPY_EFAULT);

    printf("  PASS: copy_to_user() rejected boundary-crossing address\n");
}

int main(void) {
    printf("======================================\n");
    printf("User-Kernel Memory Copy Tests\n");
    printf("======================================\n\n");

    // copy_from_user tests
    test_copy_from_user_valid();
    test_copy_from_user_kernel_address();
    test_copy_from_user_null();
    test_copy_from_user_zero_size();
    test_copy_from_user_overflow();
    test_copy_from_user_boundary_cross();

    printf("\n");

    // copy_to_user tests
    test_copy_to_user_valid();
    test_copy_to_user_kernel_address();
    test_copy_to_user_null();
    test_copy_to_user_zero_size();
    test_copy_to_user_overflow();
    test_copy_to_user_boundary_cross();

    printf("\n======================================\n");
    printf("All tests passed!\n");
    printf("======================================\n");

    return 0;
}
