/**
 * Namespace Demonstration
 *
 * This example demonstrates how to use namespaces to create isolated containers
 * in AutomationOS. It shows:
 * 1. Creating a simple container with isolated namespaces
 * 2. Running processes inside the container
 * 3. Verifying isolation between containers
 */

#include "../kernel/include/namespace.h"
#include "../kernel/include/sched.h"
#include "../kernel/include/kernel.h"

/**
 * Container 1 main function
 * This runs as PID 1 inside the container
 */
void container1_main(void) {
    process_t* current = process_get_current();

    kprintf("[CONTAINER1] Starting container (host PID: %d, container PID: %d)\n",
            current->pid, 1);

    // Get our UTS namespace and check hostname
    uts_namespace_t* uts = current->namespaces->uts_ns;
    kprintf("[CONTAINER1] Hostname: %s\n", uts->hostname);

    // Get our PID namespace info
    pid_namespace_t* pid_ns = current->namespaces->pid_ns;
    kprintf("[CONTAINER1] PID namespace ID: %d, level: %d\n",
            pid_ns->id, pid_ns->level);

    // Simulate some work
    kprintf("[CONTAINER1] Doing work...\n");

    // Try to look up processes in our namespace
    process_t* proc = pid_namespace_find_process(pid_ns, 1);
    if (proc) {
        kprintf("[CONTAINER1] Found myself at PID 1: %s\n", proc->name);
    }

    kprintf("[CONTAINER1] Container 1 finished\n");
}

/**
 * Container 2 main function
 * This runs as PID 1 in a separate container
 */
void container2_main(void) {
    process_t* current = process_get_current();

    kprintf("[CONTAINER2] Starting container (host PID: %d, container PID: %d)\n",
            current->pid, 1);

    // Get our UTS namespace and check hostname
    uts_namespace_t* uts = current->namespaces->uts_ns;
    kprintf("[CONTAINER2] Hostname: %s\n", uts->hostname);

    // Get our PID namespace info
    pid_namespace_t* pid_ns = current->namespaces->pid_ns;
    kprintf("[CONTAINER2] PID namespace ID: %d, level: %d\n",
            pid_ns->id, pid_ns->level);

    kprintf("[CONTAINER2] Doing different work...\n");

    kprintf("[CONTAINER2] Container 2 finished\n");
}

/**
 * Demonstrate basic namespace isolation
 */
void demo_basic_isolation(void) {
    kprintf("\n=== Demonstrating Basic Namespace Isolation ===\n\n");

    // Get root container for reference
    namespace_container_t* root = namespace_get_root();
    kprintf("[DEMO] Root PID namespace ID: %d\n", root->pid_ns->id);
    kprintf("[DEMO] Root hostname: %s\n\n", root->uts_ns->hostname);

    // Create first container with isolated PID and UTS namespaces
    kprintf("[DEMO] Creating Container 1...\n");
    uint32_t flags1 = CLONE_NEWPID | CLONE_NEWUTS;
    namespace_container_t* container1 = namespace_clone_container(root, flags1);

    if (!container1) {
        kprintf("[DEMO] Failed to create Container 1\n");
        return;
    }

    // Set hostname for container 1
    uts_namespace_set_hostname(container1->uts_ns, "web-server");
    kprintf("[DEMO] Container 1 hostname set to: %s\n", container1->uts_ns->hostname);

    // Create process in container 1
    process_t* proc1 = process_create("container1", (void*)container1_main);
    if (proc1) {
        // Replace default namespace with container1
        namespace_destroy_container(proc1->namespaces);
        proc1->namespaces = container1;
        container1->pid_ns->ref_count++;  // Increment since we're using it

        kprintf("[DEMO] Process 1 created with host PID %d\n\n", proc1->pid);

        // Add to scheduler
        scheduler_add_process(proc1);
    }

    // Create second container with different namespaces
    kprintf("[DEMO] Creating Container 2...\n");
    uint32_t flags2 = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNET;
    namespace_container_t* container2 = namespace_clone_container(root, flags2);

    if (!container2) {
        kprintf("[DEMO] Failed to create Container 2\n");
        return;
    }

    // Set hostname for container 2
    uts_namespace_set_hostname(container2->uts_ns, "database");
    kprintf("[DEMO] Container 2 hostname set to: %s\n", container2->uts_ns->hostname);

    // Create process in container 2
    process_t* proc2 = process_create("container2", (void*)container2_main);
    if (proc2) {
        // Replace default namespace with container2
        namespace_destroy_container(proc2->namespaces);
        proc2->namespaces = container2;
        container2->pid_ns->ref_count++;

        kprintf("[DEMO] Process 2 created with host PID %d\n\n", proc2->pid);

        // Add to scheduler
        scheduler_add_process(proc2);
    }

    // Verify isolation
    kprintf("[DEMO] Verifying isolation:\n");
    kprintf("[DEMO] - Container 1 PID namespace ID: %d\n", container1->pid_ns->id);
    kprintf("[DEMO] - Container 2 PID namespace ID: %d\n", container2->pid_ns->id);
    kprintf("[DEMO] - Container 1 hostname: %s\n", container1->uts_ns->hostname);
    kprintf("[DEMO] - Container 2 hostname: %s\n", container2->uts_ns->hostname);
    kprintf("[DEMO] - Root hostname unchanged: %s\n\n", root->uts_ns->hostname);

    // Note: In a full implementation, we would:
    // 1. Let the scheduler run these processes
    // 2. Verify they can only see their own PID namespace
    // 3. Verify network isolation (container 2 has CLONE_NEWNET)
    // 4. Clean up when processes exit
}

/**
 * Demonstrate nested containers
 */
void demo_nested_containers(void) {
    kprintf("\n=== Demonstrating Nested Containers ===\n\n");

    namespace_container_t* root = namespace_get_root();

    // Create parent container
    kprintf("[DEMO] Creating parent container...\n");
    namespace_container_t* parent = namespace_clone_container(root, CLONE_NEWPID);
    kprintf("[DEMO] Parent PID namespace: ID=%d, level=%d\n",
            parent->pid_ns->id, parent->pid_ns->level);

    // Create child container inside parent
    kprintf("[DEMO] Creating child container inside parent...\n");
    namespace_container_t* child = namespace_clone_container(parent, CLONE_NEWPID);
    kprintf("[DEMO] Child PID namespace: ID=%d, level=%d, parent=%d\n",
            child->pid_ns->id, child->pid_ns->level,
            child->pid_ns->parent ? child->pid_ns->parent->id : 0);

    // Verify hierarchy
    kprintf("[DEMO] Verifying hierarchy:\n");
    kprintf("[DEMO] - Root level: %d\n", root->pid_ns->level);
    kprintf("[DEMO] - Parent level: %d\n", parent->pid_ns->level);
    kprintf("[DEMO] - Child level: %d\n", child->pid_ns->level);
    kprintf("[DEMO] - Child parent matches parent: %s\n",
            child->pid_ns->parent == parent->pid_ns ? "YES" : "NO");

    // Cleanup
    namespace_destroy_container(child);
    namespace_destroy_container(parent);

    kprintf("[DEMO] Nested container demo complete\n");
}

/**
 * Demonstrate namespace sharing
 */
void demo_namespace_sharing(void) {
    kprintf("\n=== Demonstrating Namespace Sharing ===\n\n");

    namespace_container_t* root = namespace_get_root();

    // Create container with only PID isolated (shares everything else)
    kprintf("[DEMO] Creating container with only PID isolation...\n");
    namespace_container_t* container = namespace_clone_container(root, CLONE_NEWPID);

    kprintf("[DEMO] Checking namespace sharing:\n");
    kprintf("[DEMO] - PID namespace is new: %s\n",
            container->pid_ns != root->pid_ns ? "YES" : "NO");
    kprintf("[DEMO] - Mount namespace shared: %s\n",
            container->mount_ns == root->mount_ns ? "YES" : "NO");
    kprintf("[DEMO] - Network namespace shared: %s\n",
            container->net_ns == root->net_ns ? "YES" : "NO");
    kprintf("[DEMO] - IPC namespace shared: %s\n",
            container->ipc_ns == root->ipc_ns ? "YES" : "NO");
    kprintf("[DEMO] - UTS namespace shared: %s\n",
            container->uts_ns == root->uts_ns ? "YES" : "NO");

    kprintf("[DEMO] Root mount namespace ref_count: %d\n",
            root->mount_ns->ref_count);

    // Cleanup
    namespace_destroy_container(container);

    kprintf("[DEMO] After cleanup, root mount namespace ref_count: %d\n",
            root->mount_ns->ref_count);

    kprintf("[DEMO] Namespace sharing demo complete\n");
}

/**
 * Main demo entry point
 */
void namespace_demo(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("   Namespace Isolation Demo\n");
    kprintf("========================================\n");

    // Run demonstrations
    demo_basic_isolation();
    demo_nested_containers();
    demo_namespace_sharing();

    kprintf("\n");
    kprintf("========================================\n");
    kprintf("   Demo Complete\n");
    kprintf("========================================\n");
    kprintf("\n");
}
