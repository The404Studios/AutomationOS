/**
 * Filesystem Type Registry
 *
 * Allows registration of filesystem drivers with the VFS layer
 */

#ifndef FS_REGISTRY_H
#define FS_REGISTRY_H

#include "vfs.h"

// Maximum number of registered filesystem types
#define FS_MAX_TYPES 16

// Filesystem type operations
typedef struct fs_type_operations {
    // Mount a filesystem and return its superblock
    vfs_superblock_t* (*mount)(const char* source, uint32_t flags);

    // Unmount a filesystem
    void (*unmount)(vfs_superblock_t* sb);

    // Detect if a block device contains this filesystem type
    int (*detect)(const char* source);
} fs_type_ops_t;

// Filesystem type descriptor
typedef struct fs_type {
    const char* name;
    fs_type_ops_t* ops;
} fs_type_t;

// Filesystem registry functions
void fs_registry_init(void);
int fs_register_type(const char* name, fs_type_ops_t* ops);
fs_type_t* fs_lookup_type(const char* name);
fs_type_t* fs_detect_type(const char* source);

#endif // FS_REGISTRY_H
