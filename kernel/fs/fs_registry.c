/**
 * Filesystem Type Registry Implementation
 */

#include "../include/fs_registry.h"
#include "../include/kernel.h"
#include "../include/mem.h"
#include "../include/string.h"

// Registry of filesystem types
static struct {
    fs_type_t types[FS_MAX_TYPES];
    uint32_t count;
} fs_registry;

/**
 * Initialize the filesystem registry
 */
void fs_registry_init(void) {
    memset(&fs_registry, 0, sizeof(fs_registry));
    kprintf("[FS_REGISTRY] Initialized\n");
}

/**
 * Register a filesystem type
 */
int fs_register_type(const char* name, fs_type_ops_t* ops) {
    if (!name || !ops) {
        return -1;
    }

    if (fs_registry.count >= FS_MAX_TYPES) {
        kprintf("[FS_REGISTRY] Error: Maximum filesystem types reached\n");
        return -1;
    }

    // Check for duplicate registration
    for (uint32_t i = 0; i < fs_registry.count; i++) {
        if (strcmp(fs_registry.types[i].name, name) == 0) {
            kprintf("[FS_REGISTRY] Warning: Filesystem '%s' already registered\n", name);
            return -1;
        }
    }

    fs_registry.types[fs_registry.count].name = name;
    fs_registry.types[fs_registry.count].ops = ops;
    fs_registry.count++;

    kprintf("[FS_REGISTRY] Registered filesystem: %s\n", name);
    return 0;
}

/**
 * Look up a filesystem type by name
 */
fs_type_t* fs_lookup_type(const char* name) {
    if (!name) {
        return NULL;
    }

    for (uint32_t i = 0; i < fs_registry.count; i++) {
        if (strcmp(fs_registry.types[i].name, name) == 0) {
            return &fs_registry.types[i];
        }
    }

    return NULL;
}

/**
 * Detect filesystem type from a block device
 */
fs_type_t* fs_detect_type(const char* source) {
    if (!source) {
        return NULL;
    }

    for (uint32_t i = 0; i < fs_registry.count; i++) {
        if (fs_registry.types[i].ops->detect &&
            fs_registry.types[i].ops->detect(source) == 0) {
            kprintf("[FS_REGISTRY] Detected filesystem: %s\n", fs_registry.types[i].name);
            return &fs_registry.types[i];
        }
    }

    return NULL;
}
