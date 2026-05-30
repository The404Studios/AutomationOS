#include "../../include/namespace.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"

// External namespace ID counter
extern uint32_t next_namespace_id;

/**
 * Create a new network namespace
 *
 * Network namespaces provide isolated network stacks.
 * Each namespace has its own:
 * - Network devices (loopback, ethernet interfaces)
 * - IP addresses and routing tables
 * - Netfilter rules (firewall)
 * - Network sockets
 *
 * @return New network namespace or NULL on error
 */
net_namespace_t* net_namespace_create(void) {
    net_namespace_t* ns = (net_namespace_t*)kmalloc(sizeof(net_namespace_t));
    if (!ns) {
        kprintf("[NET_NS] Failed to allocate network namespace\n");
        return NULL;
    }

    // Initialize fields
    ns->id = __atomic_fetch_add(&next_namespace_id, 1, __ATOMIC_SEQ_CST);
    ns->stack = NULL;  // Will be initialized by network subsystem
    ns->ref_count = 1;

    kprintf("[NET_NS] Created network namespace %d\n", ns->id);

    // TODO: Initialize network stack for this namespace
    // When network stack is implemented, this should:
    // 1. Create a loopback device (lo) for this namespace
    // 2. Initialize routing tables
    // 3. Initialize socket tables
    // 4. Setup netfilter/firewall rules
    //
    // Each namespace gets its own isolated network stack:
    // - Root namespace: Has physical network devices
    // - Child namespaces: Start with only loopback, need veth pairs to communicate

    return ns;
}

/**
 * Destroy a network namespace
 * Should only be called when ref_count reaches 0
 */
void net_namespace_destroy(net_namespace_t* ns) {
    if (!ns) {
        return;
    }

    kprintf("[NET_NS] Destroying network namespace %d\n", ns->id);

    // TODO: Cleanup network stack for this namespace
    // When network stack is implemented, this should:
    // 1. Close all sockets in this namespace
    // 2. Remove all network devices
    // 3. Free routing tables
    // 4. Cleanup netfilter rules

    kfree(ns);
}
