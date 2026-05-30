/*
 * Test case for automationos-syscall-validation check
 *
 * This file demonstrates good and bad patterns for syscall
 * user pointer validation.
 */

// Simulated kernel functions
int copy_from_user(void *to, const void *from, unsigned long n);
int copy_to_user(void *to, const void *from, unsigned long n);

// GOOD: Syscall with proper validation
long sys_read_good(int fd, void *buf, unsigned long count) {
    char kernel_buf[256];

    // ✅ Proper validation using copy_to_user
    if (copy_to_user(buf, kernel_buf, count))
        return -14;  // -EFAULT

    return count;
}

// GOOD: Syscall with copy_from_user validation
long sys_write_good(int fd, const void *buf, unsigned long count) {
    char kernel_buf[256];

    // ✅ Proper validation using copy_from_user
    if (copy_from_user(kernel_buf, buf, count))
        return -14;  // -EFAULT

    // Process kernel_buf safely
    return count;
}

// BAD: Syscall without validation
// CHECK-MESSAGES: [[@LINE+1]]:1: warning: syscall parameter 'buf' appears to be a user pointer but no copy_from_user/copy_to_user validation found [automationos-syscall-validation]
long sys_read_bad(int fd, void *buf, unsigned long count) {
    char *p = (char *)buf;
    *p = 'A';  // ❌ Direct dereference without validation!
    return count;
}

// BAD: Syscall dereferencing user pointer directly
// CHECK-MESSAGES: [[@LINE+1]]:1: warning: syscall parameter 'buf' appears to be a user pointer but no copy_from_user/copy_to_user validation found [automationos-syscall-validation]
long sys_write_bad(int fd, const void *buf, unsigned long count) {
    const char *p = (const char *)buf;
    char c = *p;  // ❌ Direct read without validation!
    return count;
}

// GOOD: Syscall with no user pointers (no warning expected)
long sys_getpid_good(void) {
    return 1234;  // ✅ No user pointers, no validation needed
}

// GOOD: Syscall with only scalar parameters
long sys_close_good(int fd) {
    return 0;  // ✅ No pointers, safe
}
