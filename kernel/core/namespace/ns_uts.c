#include "../../include/namespace.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"

// External namespace ID counter
extern uint32_t next_namespace_id;

/**
 * Create a new UTS namespace
 *
 * UTS namespaces isolate system identification:
 * - Hostname (uname -n, gethostname, sethostname)
 * - Domain name (getdomainname, setdomainname)
 *
 * This allows each container to have its own hostname without affecting
 * other containers or the host system.
 *
 * @return New UTS namespace or NULL on error
 */
uts_namespace_t* uts_namespace_create(void) {
    uts_namespace_t* ns = (uts_namespace_t*)kmalloc(sizeof(uts_namespace_t));
    if (!ns) {
        kprintf("[UTS_NS] Failed to allocate UTS namespace\n");
        return NULL;
    }

    // Initialize fields
    ns->id = __atomic_fetch_add(&next_namespace_id, 1, __ATOMIC_SEQ_CST);
    ns->hostname[0] = '\0';
    ns->domainname[0] = '\0';
    ns->ref_count = 1;

    kprintf("[UTS_NS] Created UTS namespace %d\n", ns->id);

    return ns;
}

/**
 * Destroy a UTS namespace
 * Should only be called when ref_count reaches 0
 */
void uts_namespace_destroy(uts_namespace_t* ns) {
    if (!ns) {
        return;
    }

    kprintf("[UTS_NS] Destroying UTS namespace %d (hostname: %s)\n",
            ns->id, ns->hostname[0] ? ns->hostname : "(none)");

    kfree(ns);
}

/**
 * Set hostname for a UTS namespace
 *
 * @param ns UTS namespace
 * @param hostname New hostname (max 255 chars)
 * @return 0 on success, -1 on error
 */
int uts_namespace_set_hostname(uts_namespace_t* ns, const char* hostname) {
    if (!ns || !hostname) {
        return -1;
    }

    // Copy hostname (max 255 chars)
    size_t i;
    for (i = 0; i < 255 && hostname[i]; i++) {
        ns->hostname[i] = hostname[i];
    }
    ns->hostname[i] = '\0';

    kprintf("[UTS_NS] Set hostname for namespace %d: %s\n", ns->id, ns->hostname);

    return 0;
}

/**
 * Set domain name for a UTS namespace
 *
 * @param ns UTS namespace
 * @param domainname New domain name (max 255 chars)
 * @return 0 on success, -1 on error
 */
int uts_namespace_set_domainname(uts_namespace_t* ns, const char* domainname) {
    if (!ns || !domainname) {
        return -1;
    }

    // Copy domain name (max 255 chars)
    size_t i;
    for (i = 0; i < 255 && domainname[i]; i++) {
        ns->domainname[i] = domainname[i];
    }
    ns->domainname[i] = '\0';

    kprintf("[UTS_NS] Set domain name for namespace %d: %s\n", ns->id, ns->domainname);

    return 0;
}
