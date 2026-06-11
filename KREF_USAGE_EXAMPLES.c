/*
 * Example usage of kref API
 * =========================
 */

#include "kernel/include/kref.h"
#include "kernel/include/kernel.h"

// Example 1: Simple allocation and cleanup
void example_basic(void) {
    // Allocate 1KB with refcount=1
    void* obj = kmalloc_ref(1024);
    if (!obj) {
        kprintf("Allocation failed\n");
        return;
    }

    // Use the object...
    // ((uint8_t*)obj)[0] = 42;

    // Release the reference (refcount: 1->0, frees)
    kput(obj);
}

// Example 2: Sharing across multiple owners
void example_sharing(void) {
    void* obj = kmalloc_ref(256);
    if (!obj) return;

    // Share with another owner (refcount: 1->2)
    void* shared1 = kget(obj);

    // Share with yet another owner (refcount: 2->3)
    void* shared2 = kget(obj);

    // First owner releases (refcount: 3->2)
    kput(obj);

    // Second owner releases (refcount: 2->1)
    kput(shared1);

    // Last owner releases (refcount: 1->0, frees)
    kput(shared2);
}

// Example 3: With destructor
void my_destructor(void* payload) {
    kprintf("Cleaning up object at %p\n", payload);
    // Perform cleanup (e.g., close file descriptors, release locks)
}

void example_destructor(void) {
    void* obj = kmalloc_ref_dtor(512, my_destructor);
    if (!obj) return;

    // my_destructor will be called when refcount reaches 0
    kput(obj);
}

// Example 4: Edge cases handled
void example_edge_cases(void) {
    void* obj = kmalloc_ref(128);

    // NULL-safe: these are all no-ops
    kput(NULL);
    kget(NULL);

    // Double-put detection (will log error, won't free)
    kput(obj);
    // kput(obj);  // <- Would log "[KREF] ERROR: bad magic"

    // Use-after-free detection
    // kget(obj);  // <- Would log "[KREF] ERROR: bad magic"
}
