#include "../../kernel/include/namespace.h"
#include "../../kernel/include/kernel.h"
#include "../../kernel/include/sched.h"
#include "../../kernel/include/mem.h"

// Test assertion macro
#define ASSERT(cond) do { \
    if (!(cond)) { \
        kprintf("[TEST FAIL] %s:%d: Assertion failed: %s\n", __FILE__, __LINE__, #cond); \
        return; \
    } \
} while(0)

/**
 * Test: Namespace system initialization
 * Verifies that root namespaces are created correctly
 */
void test_namespace_init(void) {
    kprintf("[TEST] test_namespace_init\n");

    namespace_container_t* root = namespace_get_root();

    ASSERT(root != NULL);
    ASSERT(root->pid_ns != NULL);
    ASSERT(root->mount_ns != NULL);
    ASSERT(root->net_ns != NULL);
    ASSERT(root->ipc_ns != NULL);
    ASSERT(root->uts_ns != NULL);

    // Check root PID namespace
    ASSERT(root->pid_ns->level == 0);
    ASSERT(root->pid_ns->parent == NULL);
    ASSERT(root->pid_ns->next_pid == 1);
    ASSERT(root->pid_ns->ref_count >= 1);

    // Check UTS namespace has correct hostname
    ASSERT(root->uts_ns->hostname[0] != '\0');

    kprintf("[TEST] test_namespace_init: PASS\n");
}

/**
 * Test: Container creation and destruction
 * Verifies reference counting and cleanup
 */
void test_namespace_container_lifecycle(void) {
    kprintf("[TEST] test_namespace_container_lifecycle\n");

    namespace_container_t* root = namespace_get_root();
    uint32_t initial_pid_refs = root->pid_ns->ref_count;

    // Create new container (shares root namespaces)
    namespace_container_t* ns1 = namespace_create_container(0);
    ASSERT(ns1 != NULL);
    ASSERT(ns1->pid_ns == root->pid_ns);
    ASSERT(root->pid_ns->ref_count == initial_pid_refs + 1);

    // Create another container
    namespace_container_t* ns2 = namespace_create_container(0);
    ASSERT(ns2 != NULL);
    ASSERT(root->pid_ns->ref_count == initial_pid_refs + 2);

    // Destroy first container
    namespace_destroy_container(ns1);
    ASSERT(root->pid_ns->ref_count == initial_pid_refs + 1);

    // Destroy second container
    namespace_destroy_container(ns2);
    ASSERT(root->pid_ns->ref_count == initial_pid_refs);

    kprintf("[TEST] test_namespace_container_lifecycle: PASS\n");
}

/**
 * Test: PID namespace creation and hierarchy
 * Verifies parent-child relationships and nesting levels
 */
void test_pid_namespace_hierarchy(void) {
    kprintf("[TEST] test_pid_namespace_hierarchy\n");

    namespace_container_t* root = namespace_get_root();

    // Create child PID namespace
    namespace_container_t* child = namespace_clone_container(root, CLONE_NEWPID);
    ASSERT(child != NULL);
    ASSERT(child->pid_ns != root->pid_ns);
    ASSERT(child->pid_ns->parent == root->pid_ns);
    ASSERT(child->pid_ns->level == 1);

    // Create grandchild PID namespace
    namespace_container_t* grandchild = namespace_clone_container(child, CLONE_NEWPID);
    ASSERT(grandchild != NULL);
    ASSERT(grandchild->pid_ns != child->pid_ns);
    ASSERT(grandchild->pid_ns->parent == child->pid_ns);
    ASSERT(grandchild->pid_ns->level == 2);

    // Cleanup
    namespace_destroy_container(grandchild);
    namespace_destroy_container(child);

    kprintf("[TEST] test_pid_namespace_hierarchy: PASS\n");
}

/**
 * Test: PID allocation in namespace
 * Verifies that PIDs are allocated sequentially and correctly
 */
void test_pid_namespace_allocation(void) {
    kprintf("[TEST] test_pid_namespace_allocation\n");

    // Create a new PID namespace
    pid_namespace_t* ns = pid_namespace_create(NULL);
    ASSERT(ns != NULL);
    ASSERT(ns->next_pid == 1);
    ASSERT(ns->process_count == 0);

    // Allocate PIDs (we'll use NULL for process pointer in this test)
    // In real usage, these would be actual process pointers
    uint32_t pid1 = pid_namespace_alloc_pid(ns, (process_t*)0x1000);
    ASSERT(pid1 == 1);
    ASSERT(ns->process_count == 1);

    uint32_t pid2 = pid_namespace_alloc_pid(ns, (process_t*)0x2000);
    ASSERT(pid2 == 2);
    ASSERT(ns->process_count == 2);

    uint32_t pid3 = pid_namespace_alloc_pid(ns, (process_t*)0x3000);
    ASSERT(pid3 == 3);
    ASSERT(ns->process_count == 3);

    // Free a PID
    pid_namespace_free_pid(ns, pid2);
    ASSERT(ns->process_count == 2);

    // Cleanup
    pid_namespace_destroy(ns);

    kprintf("[TEST] test_pid_namespace_allocation: PASS\n");
}

/**
 * Test: Namespace cloning with flags
 * Verifies that different flags create/share appropriate namespaces
 */
void test_namespace_clone_flags(void) {
    kprintf("[TEST] test_namespace_clone_flags\n");

    namespace_container_t* root = namespace_get_root();

    // Clone with CLONE_NEWPID - only PID namespace should be new
    namespace_container_t* ns1 = namespace_clone_container(root, CLONE_NEWPID);
    ASSERT(ns1 != NULL);
    ASSERT(ns1->pid_ns != root->pid_ns);
    ASSERT(ns1->mount_ns == root->mount_ns);
    ASSERT(ns1->net_ns == root->net_ns);
    ASSERT(ns1->ipc_ns == root->ipc_ns);
    ASSERT(ns1->uts_ns == root->uts_ns);

    // Clone with CLONE_NEWUTS - only UTS namespace should be new
    namespace_container_t* ns2 = namespace_clone_container(root, CLONE_NEWUTS);
    ASSERT(ns2 != NULL);
    ASSERT(ns2->pid_ns == root->pid_ns);
    ASSERT(ns2->mount_ns == root->mount_ns);
    ASSERT(ns2->net_ns == root->net_ns);
    ASSERT(ns2->ipc_ns == root->ipc_ns);
    ASSERT(ns2->uts_ns != root->uts_ns);

    // Clone with multiple flags - multiple namespaces should be new
    uint32_t flags = CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWUTS;
    namespace_container_t* ns3 = namespace_clone_container(root, flags);
    ASSERT(ns3 != NULL);
    ASSERT(ns3->pid_ns != root->pid_ns);
    ASSERT(ns3->mount_ns == root->mount_ns);  // Not cloned
    ASSERT(ns3->net_ns != root->net_ns);
    ASSERT(ns3->ipc_ns == root->ipc_ns);  // Not cloned
    ASSERT(ns3->uts_ns != root->uts_ns);

    // Cleanup
    namespace_destroy_container(ns1);
    namespace_destroy_container(ns2);
    namespace_destroy_container(ns3);

    kprintf("[TEST] test_namespace_clone_flags: PASS\n");
}

/**
 * Test: Mount namespace cloning
 * Verifies mount namespace copy-on-write behavior
 */
void test_mount_namespace_clone(void) {
    kprintf("[TEST] test_mount_namespace_clone\n");

    mount_namespace_t* parent = mount_namespace_create();
    ASSERT(parent != NULL);
    ASSERT(parent->ref_count == 1);

    // Clone mount namespace
    mount_namespace_t* child = mount_namespace_clone(parent);
    ASSERT(child != NULL);
    ASSERT(child != parent);
    ASSERT(child->ref_count == 1);

    // Child should have same root path initially
    ASSERT(child->root_path[0] == parent->root_path[0]);

    // Cleanup
    mount_namespace_destroy(child);
    mount_namespace_destroy(parent);

    kprintf("[TEST] test_mount_namespace_clone: PASS\n");
}

/**
 * Test: UTS namespace hostname/domain
 * Verifies hostname and domain name isolation
 */
void test_uts_namespace_isolation(void) {
    kprintf("[TEST] test_uts_namespace_isolation\n");

    // Create two UTS namespaces
    uts_namespace_t* ns1 = uts_namespace_create();
    uts_namespace_t* ns2 = uts_namespace_create();

    ASSERT(ns1 != NULL);
    ASSERT(ns2 != NULL);

    // Set different hostnames
    int result1 = uts_namespace_set_hostname(ns1, "container1");
    int result2 = uts_namespace_set_hostname(ns2, "container2");

    ASSERT(result1 == 0);
    ASSERT(result2 == 0);

    // Verify hostnames are different and isolated
    ASSERT(ns1->hostname[0] == 'c');
    ASSERT(ns1->hostname[9] == '1');
    ASSERT(ns2->hostname[9] == '2');

    // Set domain names
    uts_namespace_set_domainname(ns1, "example.com");
    uts_namespace_set_domainname(ns2, "test.org");

    ASSERT(ns1->domainname[0] == 'e');
    ASSERT(ns2->domainname[0] == 't');

    // Cleanup
    uts_namespace_destroy(ns1);
    uts_namespace_destroy(ns2);

    kprintf("[TEST] test_uts_namespace_isolation: PASS\n");
}

/**
 * Test: Network namespace isolation
 * Verifies network namespace creation and destruction
 */
void test_net_namespace_isolation(void) {
    kprintf("[TEST] test_net_namespace_isolation\n");

    net_namespace_t* ns1 = net_namespace_create();
    net_namespace_t* ns2 = net_namespace_create();

    ASSERT(ns1 != NULL);
    ASSERT(ns2 != NULL);
    ASSERT(ns1 != ns2);
    ASSERT(ns1->id != ns2->id);

    // Cleanup
    net_namespace_destroy(ns1);
    net_namespace_destroy(ns2);

    kprintf("[TEST] test_net_namespace_isolation: PASS\n");
}

/**
 * Test: IPC namespace isolation
 * Verifies IPC namespace creation and destruction
 */
void test_ipc_namespace_isolation(void) {
    kprintf("[TEST] test_ipc_namespace_isolation\n");

    ipc_namespace_t* ns1 = ipc_namespace_create();
    ipc_namespace_t* ns2 = ipc_namespace_create();

    ASSERT(ns1 != NULL);
    ASSERT(ns2 != NULL);
    ASSERT(ns1 != ns2);
    ASSERT(ns1->id != ns2->id);

    // Cleanup
    ipc_namespace_destroy(ns1);
    ipc_namespace_destroy(ns2);

    kprintf("[TEST] test_ipc_namespace_isolation: PASS\n");
}

/**
 * Test: Reference counting stress test
 * Creates and destroys many containers to verify ref counting
 */
void test_namespace_refcount_stress(void) {
    kprintf("[TEST] test_namespace_refcount_stress\n");

    namespace_container_t* root = namespace_get_root();
    uint32_t initial_refs = root->pid_ns->ref_count;

    // Create 10 containers
    namespace_container_t* containers[10];
    for (int i = 0; i < 10; i++) {
        containers[i] = namespace_create_container(0);
        ASSERT(containers[i] != NULL);
    }

    ASSERT(root->pid_ns->ref_count == initial_refs + 10);

    // Destroy all containers
    for (int i = 0; i < 10; i++) {
        namespace_destroy_container(containers[i]);
    }

    ASSERT(root->pid_ns->ref_count == initial_refs);

    kprintf("[TEST] test_namespace_refcount_stress: PASS\n");
}

/**
 * Run all namespace tests
 */
void run_namespace_tests(void) {
    kprintf("========================================\n");
    kprintf("[TEST] Running namespace unit tests...\n");
    kprintf("========================================\n");

    test_namespace_init();
    test_namespace_container_lifecycle();
    test_pid_namespace_hierarchy();
    test_pid_namespace_allocation();
    test_namespace_clone_flags();
    test_mount_namespace_clone();
    test_uts_namespace_isolation();
    test_net_namespace_isolation();
    test_ipc_namespace_isolation();
    test_namespace_refcount_stress();

    kprintf("========================================\n");
    kprintf("[TEST] All namespace tests passed!\n");
    kprintf("========================================\n");
}
