/*
 * Test case for automationos-null-check check
 *
 * This file demonstrates good and bad patterns for NULL checking
 * after memory allocations.
 */

// Simulated allocation functions
void *kmalloc(unsigned long size);
void *kzalloc(unsigned long size);
void kfree(void *ptr);

// GOOD: NULL check immediately after allocation
void alloc_good_1(void) {
    void *ptr = kmalloc(100);

    // ✅ Check for NULL before use
    if (!ptr) {
        return;
    }

    // Safe to use ptr here
    kfree(ptr);
}

// GOOD: NULL check with early return
void *alloc_good_2(unsigned long size) {
    void *ptr = kmalloc(size);

    // ✅ Check and return NULL
    if (ptr == NULL)
        return NULL;

    return ptr;
}

// GOOD: NULL check in conditional
void alloc_good_3(void) {
    void *ptr = kzalloc(200);

    // ✅ Check in if statement
    if (ptr) {
        kfree(ptr);
    }
}

// BAD: No NULL check before use
// CHECK-MESSAGES: [[@LINE+1]]:17: warning: memory allocation result not checked for NULL [automationos-null-check]
void alloc_bad_1(void) {
    void *ptr = kmalloc(100);

    // ❌ No NULL check - immediate use
    char *p = (char *)ptr;
    *p = 'A';  // Potential NULL dereference!

    kfree(ptr);
}

// BAD: NULL check too late (after dereference)
// CHECK-MESSAGES: [[@LINE+1]]:17: warning: memory allocation result not checked for NULL [automationos-null-check]
void alloc_bad_2(void) {
    void *ptr = kmalloc(100);

    // ❌ Used before checking
    int *ip = (int *)ptr;
    *ip = 42;  // Potential NULL dereference!

    // Too late
    if (!ptr)
        return;
}

// BAD: Missing NULL check entirely
// CHECK-MESSAGES: [[@LINE+1]]:17: warning: memory allocation result not checked for NULL [automationos-null-check]
void alloc_bad_3(void) {
    char *buffer = (char *)kzalloc(512);

    // ❌ Direct use without any check
    buffer[0] = 'A';
    buffer[1] = 'B';

    kfree(buffer);
}

// GOOD: Check in loop condition
void alloc_good_loop(void) {
    void *ptr = kmalloc(100);

    // ✅ Check in while condition
    while (ptr) {
        kfree(ptr);
        break;
    }
}

// Structure example
struct data {
    int value;
    char name[32];
};

// BAD: Structure allocation without NULL check
// CHECK-MESSAGES: [[@LINE+1]]:24: warning: memory allocation result not checked for NULL [automationos-null-check]
void alloc_struct_bad(void) {
    struct data *d = (struct data *)kmalloc(sizeof(struct data));

    // ❌ Field access without NULL check
    d->value = 100;
}

// GOOD: Structure allocation with NULL check
void alloc_struct_good(void) {
    struct data *d = (struct data *)kmalloc(sizeof(struct data));

    // ✅ Check before field access
    if (!d)
        return;

    d->value = 100;
    kfree(d);
}
