#include "../include/namespace.h"
#include "../include/kernel.h"
#include "../include/mem.h"
#include "../include/sched.h"

// Global namespace ID counter
static uint32_t next_namespace_id = 1;

// Global root namespaces (shared by all processes by default)
static namespace_container_t* root_container = NULL;

/**
 * Initialize the namespace system
 * Creates root namespaces that are shared by all processes initially
 */
void namespace_init(void) {
    kprintf("[NS] Initializing namespace system...\n");

    // Allocate root container
    root_container = (namespace_container_t*)kmalloc(sizeof(namespace_container_t));
    if (!root_container) {
        kernel_panic("Failed to allocate root namespace container");
    }

    // Create root namespaces
    root_container->pid_ns = pid_namespace_create(NULL);
    root_container->mount_ns = mount_namespace_create();
    root_container->net_ns = net_namespace_create();
    root_container->ipc_ns = ipc_namespace_create();
    root_container->uts_ns = uts_namespace_create();

    if (!root_container->pid_ns || !root_container->mount_ns ||
        !root_container->net_ns || !root_container->ipc_ns ||
        !root_container->uts_ns) {
        kernel_panic("Failed to create root namespaces");
    }

    // Set default hostname and domain
    uts_namespace_set_hostname(root_container->uts_ns, "automationos");
    uts_namespace_set_domainname(root_container->uts_ns, "local");

    kprintf("[NS] Root namespaces created (PID: %d, Mount: %d, Net: %d, IPC: %d, UTS: %d)\n",
            root_container->pid_ns->id, root_container->mount_ns->id,
            root_container->net_ns->id, root_container->ipc_ns->id,
            root_container->uts_ns->id);
}

/**
 * Get the root namespace container
 */
namespace_container_t* namespace_get_root(void) {
    return root_container;
}

/**
 * Create a new namespace container
 * All namespaces initially point to root namespaces (shared)
 */
namespace_container_t* namespace_create_container(uint32_t flags) {
    // Auto-initialize namespace system if not done yet
    if (!root_container) {
        namespace_init();
    }

    namespace_container_t* ns = (namespace_container_t*)kmalloc(sizeof(namespace_container_t));
    if (!ns) {
        return NULL;
    }

    // Start with root namespaces (shared by default)
    ns->pid_ns = root_container->pid_ns;
    ns->mount_ns = root_container->mount_ns;
    ns->net_ns = root_container->net_ns;
    ns->ipc_ns = root_container->ipc_ns;
    ns->uts_ns = root_container->uts_ns;

    // Increment reference counts atomically (LEAK-006 fix)
    __atomic_add_fetch(&ns->pid_ns->ref_count, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&ns->mount_ns->ref_count, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&ns->net_ns->ref_count, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&ns->ipc_ns->ref_count, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&ns->uts_ns->ref_count, 1, __ATOMIC_SEQ_CST);

    return ns;
}

/**
 * Destroy a namespace container
 * Decrements reference counts and destroys namespaces when ref_count reaches 0
 */
void namespace_destroy_container(namespace_container_t* ns) {
    if (!ns) return;

    // Decrement ref counts atomically and destroy if zero (LEAK-006 fix)
    // Use atomic decrement to prevent race condition where two CPUs
    // simultaneously see ref_count=2, both decrement to 1, and namespace never freed
    if (ns->pid_ns) {
        uint32_t old = __atomic_sub_fetch(&ns->pid_ns->ref_count, 1, __ATOMIC_SEQ_CST);
        if (old == 0) {
            pid_namespace_destroy(ns->pid_ns);
        }
    }
    if (ns->mount_ns) {
        uint32_t old = __atomic_sub_fetch(&ns->mount_ns->ref_count, 1, __ATOMIC_SEQ_CST);
        if (old == 0) {
            mount_namespace_destroy(ns->mount_ns);
        }
    }
    if (ns->net_ns) {
        uint32_t old = __atomic_sub_fetch(&ns->net_ns->ref_count, 1, __ATOMIC_SEQ_CST);
        if (old == 0) {
            net_namespace_destroy(ns->net_ns);
        }
    }
    if (ns->ipc_ns) {
        uint32_t old = __atomic_sub_fetch(&ns->ipc_ns->ref_count, 1, __ATOMIC_SEQ_CST);
        if (old == 0) {
            ipc_namespace_destroy(ns->ipc_ns);
        }
    }
    if (ns->uts_ns) {
        uint32_t old = __atomic_sub_fetch(&ns->uts_ns->ref_count, 1, __ATOMIC_SEQ_CST);
        if (old == 0) {
            uts_namespace_destroy(ns->uts_ns);
        }
    }

    kfree(ns);
}

/**
 * Clone a namespace container (for fork/clone syscall)
 * Based on flags, either shares parent namespaces or creates new ones
 *
 * @param parent Parent namespace container
 * @param flags CLONE_NEW* flags indicating which namespaces to create
 * @return New namespace container
 */
namespace_container_t* namespace_clone_container(namespace_container_t* parent, uint32_t flags) {
    if (!parent) {
        return NULL;
    }

    namespace_container_t* ns = (namespace_container_t*)kmalloc(sizeof(namespace_container_t));
    if (!ns) {
        return NULL;
    }

    // Clone or share PID namespace
    if (flags & CLONE_NEWPID) {
        ns->pid_ns = pid_namespace_create(parent->pid_ns);
        if (!ns->pid_ns) {
            goto error_cleanup;
        }
    } else {
        ns->pid_ns = parent->pid_ns;
        __atomic_add_fetch(&ns->pid_ns->ref_count, 1, __ATOMIC_SEQ_CST);  // LEAK-006 fix
    }

    // Clone or share mount namespace
    if (flags & CLONE_NEWMOUNT) {
        ns->mount_ns = mount_namespace_clone(parent->mount_ns);
        if (!ns->mount_ns) {
            goto error_cleanup;
        }
    } else {
        ns->mount_ns = parent->mount_ns;
        __atomic_add_fetch(&ns->mount_ns->ref_count, 1, __ATOMIC_SEQ_CST);  // LEAK-006 fix
    }

    // Clone or share network namespace
    if (flags & CLONE_NEWNET) {
        ns->net_ns = net_namespace_create();
        if (!ns->net_ns) {
            goto error_cleanup;
        }
    } else {
        ns->net_ns = parent->net_ns;
        __atomic_add_fetch(&ns->net_ns->ref_count, 1, __ATOMIC_SEQ_CST);  // LEAK-006 fix
    }

    // Clone or share IPC namespace
    if (flags & CLONE_NEWIPC) {
        ns->ipc_ns = ipc_namespace_create();
        if (!ns->ipc_ns) {
            goto error_cleanup;
        }
    } else {
        ns->ipc_ns = parent->ipc_ns;
        __atomic_add_fetch(&ns->ipc_ns->ref_count, 1, __ATOMIC_SEQ_CST);  // LEAK-006 fix
    }

    // Clone or share UTS namespace
    if (flags & CLONE_NEWUTS) {
        ns->uts_ns = uts_namespace_create();
        if (!ns->uts_ns) {
            goto error_cleanup;
        }
        // Copy hostname and domainname from parent
        uts_namespace_set_hostname(ns->uts_ns, parent->uts_ns->hostname);
        uts_namespace_set_domainname(ns->uts_ns, parent->uts_ns->domainname);
    } else {
        ns->uts_ns = parent->uts_ns;
        __atomic_add_fetch(&ns->uts_ns->ref_count, 1, __ATOMIC_SEQ_CST);  // LEAK-006 fix
    }

    return ns;

error_cleanup:
    // Clean up partially created namespaces (LEAK-006 fix: use atomic decrements)
    if (ns->pid_ns && (flags & CLONE_NEWPID)) {
        pid_namespace_destroy(ns->pid_ns);
    } else if (ns->pid_ns) {
        __atomic_sub_fetch(&ns->pid_ns->ref_count, 1, __ATOMIC_SEQ_CST);
    }

    if (ns->mount_ns && (flags & CLONE_NEWMOUNT)) {
        mount_namespace_destroy(ns->mount_ns);
    } else if (ns->mount_ns) {
        __atomic_sub_fetch(&ns->mount_ns->ref_count, 1, __ATOMIC_SEQ_CST);
    }

    if (ns->net_ns && (flags & CLONE_NEWNET)) {
        net_namespace_destroy(ns->net_ns);
    } else if (ns->net_ns) {
        __atomic_sub_fetch(&ns->net_ns->ref_count, 1, __ATOMIC_SEQ_CST);
    }

    if (ns->ipc_ns && (flags & CLONE_NEWIPC)) {
        ipc_namespace_destroy(ns->ipc_ns);
    } else if (ns->ipc_ns) {
        __atomic_sub_fetch(&ns->ipc_ns->ref_count, 1, __ATOMIC_SEQ_CST);
    }

    if (ns->uts_ns && (flags & CLONE_NEWUTS)) {
        uts_namespace_destroy(ns->uts_ns);
    } else if (ns->uts_ns) {
        __atomic_sub_fetch(&ns->uts_ns->ref_count, 1, __ATOMIC_SEQ_CST);
    }

    kfree(ns);
    return NULL;
}

/**
 * Unshare namespaces for current process
 * Creates new namespaces based on flags, moving process to new namespaces
 *
 * @param proc Current process
 * @param flags CLONE_NEW* flags indicating which namespaces to unshare
 * @return 0 on success, -1 on error
 */
int namespace_unshare(struct process* proc, uint32_t flags) {
    if (!proc || !proc->namespaces) {
        return -1;
    }

    // Clone container with specified flags
    namespace_container_t* new_ns = namespace_clone_container(proc->namespaces, flags);
    if (!new_ns) {
        return -1;
    }

    // Destroy old container and switch to new one
    namespace_destroy_container(proc->namespaces);
    proc->namespaces = new_ns;

    return 0;
}

/**
 * Enter an existing namespace (setns syscall)
 *
 * @param proc Process to move to new namespace
 * @param type Type of namespace to change
 * @param ns_id Namespace ID to enter
 * @return 0 on success, -1 on error
 */
int namespace_enter(struct process* proc, namespace_type_t type, uint32_t ns_id) {
    // TODO: Implement namespace lookup by ID
    // For now, this is a placeholder
    (void)proc;
    (void)type;
    (void)ns_id;
    return -1;
}
