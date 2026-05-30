#include "../../include/namespace.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"

// External namespace ID counter
extern uint32_t next_namespace_id;

/**
 * Create a new mount namespace
 *
 * Mount namespaces provide isolated filesystem views.
 * Each namespace has its own set of mount points.
 *
 * @return New mount namespace or NULL on error
 */
mount_namespace_t* mount_namespace_create(void) {
    mount_namespace_t* ns = (mount_namespace_t*)kmalloc(sizeof(mount_namespace_t));
    if (!ns) {
        kprintf("[MOUNT_NS] Failed to allocate mount namespace\n");
        return NULL;
    }

    // Initialize fields
    ns->id = __atomic_fetch_add(&next_namespace_id, 1, __ATOMIC_SEQ_CST);
    ns->mounts = NULL;  // Will be initialized by VFS subsystem
    ns->flags = MNT_NS_PRIVATE;  // Private by default
    ns->ref_count = 1;

    // Set default root path
    ns->root_path[0] = '/';
    ns->root_path[1] = '\0';

    kprintf("[MOUNT_NS] Created mount namespace %d\n", ns->id);

    return ns;
}

/**
 * Destroy a mount namespace
 * Should only be called when ref_count reaches 0
 */
void mount_namespace_destroy(mount_namespace_t* ns) {
    if (!ns) {
        return;
    }

    kprintf("[MOUNT_NS] Destroying mount namespace %d\n", ns->id);

    // TODO: Unmount all filesystems in this namespace
    // This will be implemented when VFS mount support is added
    // Should iterate through mount table and unmount each filesystem

    kfree(ns);
}

/**
 * Clone a mount namespace (copy-on-write)
 *
 * Creates a new mount namespace with a copy of the parent's mount table.
 * This implements copy-on-write semantics - the child starts with the same
 * mounts as the parent, but future mount/unmount operations are isolated.
 *
 * @param parent Parent mount namespace
 * @return New mount namespace or NULL on error
 */
mount_namespace_t* mount_namespace_clone(mount_namespace_t* parent) {
    if (!parent) {
        return mount_namespace_create();
    }

    mount_namespace_t* ns = (mount_namespace_t*)kmalloc(sizeof(mount_namespace_t));
    if (!ns) {
        kprintf("[MOUNT_NS] Failed to allocate mount namespace\n");
        return NULL;
    }

    // Initialize fields
    ns->id = __atomic_fetch_add(&next_namespace_id, 1, __ATOMIC_SEQ_CST);
    ns->mounts = NULL;  // Will be copied from parent by VFS
    ns->flags = parent->flags;
    ns->ref_count = 1;

    // Copy root path from parent
    size_t i;
    for (i = 0; i < 255 && parent->root_path[i]; i++) {
        ns->root_path[i] = parent->root_path[i];
    }
    ns->root_path[i] = '\0';

    // TODO: Clone mount table from parent
    // This will be implemented when VFS mount support is added
    // Should create a copy of the parent's mount table
    // Mounts can be:
    // - MS_SHARED: Changes propagate between namespaces
    // - MS_PRIVATE: Changes are isolated (default)
    // - MS_SLAVE: Receives propagation from master but doesn't send
    // - MS_UNBINDABLE: Cannot be bind-mounted

    kprintf("[MOUNT_NS] Cloned mount namespace %d from parent %d\n",
            ns->id, parent->id);

    return ns;
}
