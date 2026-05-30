#include "../../kernel/include/capability.h"
#include "../../kernel/include/kernel.h"
#include "../../kernel/include/sched.h"
#include "../../kernel/include/mem.h"

// Test framework macros
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            kprintf("[TEST FAIL] %s: %s\n", __func__, message); \
            return false; \
        } \
    } while(0)

#define TEST_PASS() \
    do { \
        kprintf("[TEST PASS] %s\n", __func__); \
        return true; \
    } while(0)

// External string functions
extern int strcmp(const char* s1, const char* s2);
extern char* strcpy(char* dest, const char* src);

// Test: Create and destroy capability set
bool test_capability_set_create_destroy(void) {
    capability_set_t* set = capability_set_create();
    TEST_ASSERT(set != NULL, "Failed to create capability set");
    TEST_ASSERT(set->count == 0, "New set should have 0 capabilities");
    TEST_ASSERT(set->head == NULL, "New set should have null head");
    TEST_ASSERT(set->bitmask == 0, "New set should have 0 bitmask");

    capability_set_destroy(set);
    TEST_PASS();
}

// Test: Add and remove capabilities
bool test_capability_add_remove(void) {
    capability_set_t* set = capability_set_create();
    TEST_ASSERT(set != NULL, "Failed to create capability set");

    // Create a simple capability
    capability_t* cap = capability_create_simple(CAP_FILE_READ, CAP_FLAG_INHERITABLE);
    TEST_ASSERT(cap != NULL, "Failed to create capability");

    // Add capability
    int result = capability_add(set, cap);
    TEST_ASSERT(result == CAP_SUCCESS, "Failed to add capability");
    TEST_ASSERT(set->count == 1, "Set should have 1 capability");
    TEST_ASSERT(capability_has(set, CAP_FILE_READ), "Set should have CAP_FILE_READ");

    // Try to add duplicate
    capability_t* dup_cap = capability_create_simple(CAP_FILE_READ, CAP_FLAG_INHERITABLE);
    result = capability_add(set, dup_cap);
    TEST_ASSERT(result == CAP_EDUP, "Should reject duplicate capability");
    kfree(dup_cap);

    // Remove capability
    result = capability_remove(set, CAP_FILE_READ);
    TEST_ASSERT(result == CAP_SUCCESS, "Failed to remove capability");
    TEST_ASSERT(set->count == 0, "Set should have 0 capabilities after removal");
    TEST_ASSERT(!capability_has(set, CAP_FILE_READ), "Set should not have CAP_FILE_READ");

    capability_set_destroy(set);
    TEST_PASS();
}

// Test: File path pattern matching
bool test_capability_file_pattern(void) {
    capability_set_t* set = capability_set_create();
    TEST_ASSERT(set != NULL, "Failed to create capability set");

    // Add file read capability with pattern
    capability_t* cap = capability_create_file(CAP_FILE_READ, "/home/user/*",
                                              CAP_FLAG_INHERITABLE);
    TEST_ASSERT(cap != NULL, "Failed to create file capability");

    int result = capability_add(set, cap);
    TEST_ASSERT(result == CAP_SUCCESS, "Failed to add file capability");

    // Test matching paths
    TEST_ASSERT(capability_check_file(set, "/home/user/file.txt", CAP_FILE_READ),
               "Should match /home/user/file.txt");
    TEST_ASSERT(capability_check_file(set, "/home/user/dir/file.txt", CAP_FILE_READ),
               "Should match /home/user/dir/file.txt");

    // Test non-matching paths
    TEST_ASSERT(!capability_check_file(set, "/etc/passwd", CAP_FILE_READ),
               "Should not match /etc/passwd");
    TEST_ASSERT(!capability_check_file(set, "/home/other/file.txt", CAP_FILE_READ),
               "Should not match /home/other/file.txt");

    capability_set_destroy(set);
    TEST_PASS();
}

// Test: Network host/port matching
bool test_capability_network(void) {
    capability_set_t* set = capability_set_create();
    TEST_ASSERT(set != NULL, "Failed to create capability set");

    // Add network capability
    capability_t* cap = capability_create_net(CAP_NET_CONNECT, "*.example.com",
                                             80, 443, CAP_FLAG_INHERITABLE);
    TEST_ASSERT(cap != NULL, "Failed to create network capability");

    int result = capability_add(set, cap);
    TEST_ASSERT(result == CAP_SUCCESS, "Failed to add network capability");

    // Test matching hosts/ports
    TEST_ASSERT(capability_check_net(set, "www.example.com", 80, CAP_NET_CONNECT),
               "Should match www.example.com:80");
    TEST_ASSERT(capability_check_net(set, "api.example.com", 443, CAP_NET_CONNECT),
               "Should match api.example.com:443");

    // Test non-matching hosts/ports
    TEST_ASSERT(!capability_check_net(set, "www.example.com", 8080, CAP_NET_CONNECT),
               "Should not match port 8080");
    TEST_ASSERT(!capability_check_net(set, "www.other.com", 80, CAP_NET_CONNECT),
               "Should not match www.other.com");

    capability_set_destroy(set);
    TEST_PASS();
}

// Test: Capability inheritance
bool test_capability_inheritance(void) {
    capability_set_t* parent = capability_set_create();
    TEST_ASSERT(parent != NULL, "Failed to create parent capability set");

    // Add inheritable and non-inheritable capabilities
    capability_t* cap1 = capability_create_simple(CAP_FILE_READ, CAP_FLAG_INHERITABLE);
    capability_t* cap2 = capability_create_simple(CAP_NET_CONNECT, CAP_FLAG_INHERITABLE);
    capability_t* cap3 = capability_create_simple(CAP_SYS_ADMIN, 0);  // Not inheritable

    capability_add(parent, cap1);
    capability_add(parent, cap2);
    capability_add(parent, cap3);

    TEST_ASSERT(parent->count == 3, "Parent should have 3 capabilities");

    // Inherit capabilities
    uint64_t inherit_mask = (1ULL << CAP_FILE_READ) | (1ULL << CAP_NET_CONNECT);
    capability_set_t* child = capability_inherit(parent, inherit_mask);
    TEST_ASSERT(child != NULL, "Failed to inherit capabilities");

    // Child should have only inheritable capabilities
    TEST_ASSERT(capability_has(child, CAP_FILE_READ), "Child should have CAP_FILE_READ");
    TEST_ASSERT(capability_has(child, CAP_NET_CONNECT), "Child should have CAP_NET_CONNECT");
    TEST_ASSERT(!capability_has(child, CAP_SYS_ADMIN), "Child should not have CAP_SYS_ADMIN");

    capability_set_destroy(parent);
    capability_set_destroy(child);
    TEST_PASS();
}

// Test: Capability delegation
bool test_capability_delegation(void) {
    // Create two mock processes
    process_t* proc1 = (process_t*)kmalloc(sizeof(process_t));
    process_t* proc2 = (process_t*)kmalloc(sizeof(process_t));
    TEST_ASSERT(proc1 != NULL && proc2 != NULL, "Failed to allocate processes");

    proc1->pid = 100;
    proc2->pid = 200;
    proc1->capabilities = capability_set_create();
    proc2->capabilities = capability_set_create();

    // Give proc1 a delegatable capability
    capability_t* cap = capability_create_simple(CAP_FILE_READ,
                                                CAP_FLAG_DELEGATABLE);
    capability_add(proc1->capabilities, cap);

    // Delegate to proc2
    int result = capability_grant(proc1, proc2, cap);
    TEST_ASSERT(result == CAP_SUCCESS, "Failed to delegate capability");
    TEST_ASSERT(capability_has(proc2->capabilities, CAP_FILE_READ),
               "Proc2 should have received capability");

    // Try to delegate non-delegatable capability
    capability_t* cap2 = capability_create_simple(CAP_SYS_ADMIN, 0);  // Not delegatable
    capability_add(proc1->capabilities, cap2);

    result = capability_grant(proc1, proc2, cap2);
    TEST_ASSERT(result == CAP_EPERM, "Should fail to delegate non-delegatable capability");

    capability_set_destroy(proc1->capabilities);
    capability_set_destroy(proc2->capabilities);
    kfree(proc1);
    kfree(proc2);
    TEST_PASS();
}

// Test: Capability revocation
bool test_capability_revocation(void) {
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    TEST_ASSERT(proc != NULL, "Failed to allocate process");

    proc->pid = 100;
    proc->capabilities = capability_set_create();

    // Add capabilities
    capability_t* cap1 = capability_create_simple(CAP_FILE_READ, CAP_FLAG_INHERITABLE);
    capability_t* cap2 = capability_create_simple(CAP_NET_CONNECT, CAP_FLAG_INHERITABLE);
    capability_add(proc->capabilities, cap1);
    capability_add(proc->capabilities, cap2);

    TEST_ASSERT(proc->capabilities->count == 2, "Should have 2 capabilities");

    // Revoke one capability
    uint32_t old_gen = global_capability_generation;
    int result = capability_revoke(proc, CAP_FILE_READ);
    TEST_ASSERT(result == CAP_SUCCESS, "Failed to revoke capability");
    TEST_ASSERT(!capability_has(proc->capabilities, CAP_FILE_READ),
               "Should not have CAP_FILE_READ after revocation");
    TEST_ASSERT(capability_has(proc->capabilities, CAP_NET_CONNECT),
               "Should still have CAP_NET_CONNECT");
    TEST_ASSERT(global_capability_generation > old_gen,
               "Global generation should increment on revocation");

    // Revoke all capabilities
    result = capability_revoke_all(proc);
    TEST_ASSERT(result == CAP_SUCCESS, "Failed to revoke all capabilities");
    TEST_ASSERT(proc->capabilities->count == 0, "Should have 0 capabilities");

    capability_set_destroy(proc->capabilities);
    kfree(proc);
    TEST_PASS();
}

// Test: Device capability
bool test_capability_device(void) {
    capability_set_t* set = capability_set_create();
    TEST_ASSERT(set != NULL, "Failed to create capability set");

    // Add device capability for specific device
    capability_t* cap1 = capability_create_device(0x1234, "gpu", CAP_FLAG_INHERITABLE);
    capability_add(set, cap1);

    TEST_ASSERT(capability_check_device(set, 0x1234), "Should have access to device 0x1234");
    TEST_ASSERT(!capability_check_device(set, 0x5678), "Should not have access to device 0x5678");

    // Add capability for all devices
    capability_t* cap2 = capability_create_device(0xFFFFFFFF, "all", CAP_FLAG_INHERITABLE);
    capability_add(set, cap2);

    TEST_ASSERT(capability_check_device(set, 0x5678), "Should have access to all devices");

    capability_set_destroy(set);
    TEST_PASS();
}

// Test: IPC capability
bool test_capability_ipc(void) {
    capability_set_t* set = capability_set_create();
    TEST_ASSERT(set != NULL, "Failed to create capability set");

    // Add IPC capability for specific process
    capability_t* cap1 = capability_create_ipc(42, CAP_FLAG_INHERITABLE);
    capability_add(set, cap1);

    TEST_ASSERT(capability_check_ipc(set, 42), "Should have IPC access to PID 42");
    TEST_ASSERT(!capability_check_ipc(set, 100), "Should not have IPC access to PID 100");

    // Add broadcast capability
    capability_t* cap2 = capability_create_simple(CAP_IPC_BROADCAST, CAP_FLAG_INHERITABLE);
    capability_add(set, cap2);

    TEST_ASSERT(capability_check_ipc(set, 100), "Should have IPC access to all processes");

    capability_set_destroy(set);
    TEST_PASS();
}

// Test: Capability cloning
bool test_capability_clone(void) {
    capability_set_t* set1 = capability_set_create();
    TEST_ASSERT(set1 != NULL, "Failed to create capability set");

    // Add some capabilities
    capability_t* cap1 = capability_create_simple(CAP_FILE_READ, CAP_FLAG_INHERITABLE);
    capability_t* cap2 = capability_create_simple(CAP_NET_CONNECT, CAP_FLAG_INHERITABLE);
    capability_add(set1, cap1);
    capability_add(set1, cap2);

    // Clone the set
    capability_set_t* set2 = capability_set_clone(set1);
    TEST_ASSERT(set2 != NULL, "Failed to clone capability set");
    TEST_ASSERT(set2->count == set1->count, "Cloned set should have same count");
    TEST_ASSERT(capability_has(set2, CAP_FILE_READ), "Clone should have CAP_FILE_READ");
    TEST_ASSERT(capability_has(set2, CAP_NET_CONNECT), "Clone should have CAP_NET_CONNECT");

    // Modify original - clone should be unaffected
    capability_remove(set1, CAP_FILE_READ);
    TEST_ASSERT(!capability_has(set1, CAP_FILE_READ), "Original should not have CAP_FILE_READ");
    TEST_ASSERT(capability_has(set2, CAP_FILE_READ), "Clone should still have CAP_FILE_READ");

    capability_set_destroy(set1);
    capability_set_destroy(set2);
    TEST_PASS();
}

// Test runner
void run_capability_tests(void) {
    kprintf("\n");
    kprintf("==============================================\n");
    kprintf("  Running Capability System Unit Tests\n");
    kprintf("==============================================\n");
    kprintf("\n");

    int passed = 0;
    int total = 0;

    #define RUN_TEST(test_func) \
        do { \
            total++; \
            if (test_func()) passed++; \
        } while(0)

    RUN_TEST(test_capability_set_create_destroy);
    RUN_TEST(test_capability_add_remove);
    RUN_TEST(test_capability_file_pattern);
    RUN_TEST(test_capability_network);
    RUN_TEST(test_capability_inheritance);
    RUN_TEST(test_capability_delegation);
    RUN_TEST(test_capability_revocation);
    RUN_TEST(test_capability_device);
    RUN_TEST(test_capability_ipc);
    RUN_TEST(test_capability_clone);

    kprintf("\n");
    kprintf("==============================================\n");
    kprintf("  Test Results: %d/%d passed\n", passed, total);
    kprintf("==============================================\n");
    kprintf("\n");

    if (passed == total) {
        kprintf("[SUCCESS] All capability tests passed!\n");
    } else {
        kprintf("[FAILURE] Some capability tests failed!\n");
    }
}
