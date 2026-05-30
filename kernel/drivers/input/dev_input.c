/*
 * /dev/input/* device node management
 * Creates and manages character device nodes for input events
 */

#include "../../include/input.h"
#include "../../include/kernel.h"
#include "../../include/types.h"
#include "../../include/vfs.h"
#include "../../include/mem.h"
#include "../../include/string.h"

// Forward declarations from evdev.c
extern void evdev_init(void);
extern int evdev_register_device(input_device_t* input_dev);
extern void evdev_unregister_device(input_device_t* input_dev);
extern vfs_file_ops_t* evdev_get_fops(void);

// /dev/input directory inode
static vfs_inode_t* dev_input_dir = NULL;

// Directory entry for simple directory listing
typedef struct dev_input_entry {
    char name[64];
    vfs_inode_t* inode;
    struct dev_input_entry* next;
} dev_input_entry_t;

static dev_input_entry_t* dev_input_entries = NULL;

/*
 * Lookup entry in /dev/input
 */
static vfs_dentry_t* dev_input_lookup(vfs_inode_t* dir, const char* name) {
    (void)dir;

    // Search for entry
    dev_input_entry_t* entry = dev_input_entries;
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            vfs_dentry_t* dentry = vfs_dentry_alloc(name);
            if (dentry) {
                dentry->inode = entry->inode;
                vfs_inode_get(entry->inode);
            }
            return dentry;
        }
        entry = entry->next;
    }

    return NULL;
}

static vfs_inode_ops_t dev_input_dir_ops = {
    .lookup = dev_input_lookup,
    .create = NULL,
    .mkdir = NULL,
    .unlink = NULL,
    .rmdir = NULL,
};

/*
 * Create /dev/input directory.
 *
 * NOTE: This must be called AFTER the root filesystem is mounted and the /dev
 * directory exists. It creates /dev/input as a real ramfs directory and then
 * caches that directory's inode so device nodes can be linked into it.
 */
int dev_input_create_dir(void) {
    kprintf("[DEV_INPUT] Creating /dev/input directory\n");

    // Create /dev/input as a real ramfs directory. ramfs_mkdir() gives it a
    // proper dentry-array private_data and ramfs_inode_ops (so its ->lookup
    // works through vfs_path_lookup()).
    int ret = vfs_mkdir("/dev/input", 0755);
    if (ret < 0) {
        kprintf("[DEV_INPUT] Warning: vfs_mkdir(/dev/input) failed (%d); "
                "/dev may be missing or it already exists\n", ret);
        // Continue: it may already exist from a previous call.
    }

    // Resolve the real directory inode so we can link device nodes into it.
    dev_input_dir = vfs_path_lookup("/dev/input");
    if (!dev_input_dir) {
        kprintf("[DEV_INPUT] ERROR: /dev/input not resolvable after mkdir\n");
        return -1;
    }
    if (!(dev_input_dir->type & VFS_TYPE_DIR)) {
        kprintf("[DEV_INPUT] ERROR: /dev/input is not a directory\n");
        dev_input_dir = NULL;
        return -1;
    }

    kprintf("[DEV_INPUT] /dev/input directory ready (inode %p)\n", dev_input_dir);
    return 0;
}

/*
 * Link a device inode into the /dev/input ramfs directory so that
 * vfs_path_lookup("/dev/input/<name>") resolves it. We insert a dentry into
 * the directory inode's private_data dentry-array, matching the layout that
 * ramfs_lookup() / vfs_readdir() expect.
 */
int dev_input_link_node(const char* name, vfs_inode_t* inode) {
    if (!name || !inode) return -1;

    if (!dev_input_dir) {
        // /dev/input not created yet; caller will retry later.
        return -1;
    }

    // Ensure the directory has a dentry-array (ramfs_mkdir allocates one with
    // capacity 16; guard in case it is empty).
    if (!dev_input_dir->private_data) {
        dev_input_dir->private_data = kmalloc(sizeof(vfs_dentry_t*) * 16);
        if (!dev_input_dir->private_data) {
            return -1;
        }
        memset(dev_input_dir->private_data, 0, sizeof(vfs_dentry_t*) * 16);
        dev_input_dir->data_capacity = 16;
    }

    vfs_dentry_t* dentry = vfs_dentry_alloc(name);
    if (!dentry) {
        return -1;
    }
    dentry->inode = inode;
    vfs_inode_get(inode);  // dentry now holds a reference

    vfs_dentry_t** entries = (vfs_dentry_t**)dev_input_dir->private_data;
    for (uint64_t i = 0; i < dev_input_dir->data_capacity; i++) {
        if (!entries[i]) {
            entries[i] = dentry;
            dev_input_dir->size++;
            kprintf("[DEV_INPUT] Linked /dev/input/%s\n", name);
            return 0;
        }
    }

    // Directory array full; drop the reference and fail.
    kprintf("[DEV_INPUT] /dev/input directory full, cannot link %s\n", name);
    dentry->inode = NULL;
    vfs_inode_put(inode);
    vfs_dentry_free(dentry);
    return -1;
}

/*
 * Create device node in /dev/input
 */
int dev_input_create_node(const char* name, vfs_inode_t* inode) {
    if (!name || !inode) return -1;

    // Add to entries list
    dev_input_entry_t* entry = (dev_input_entry_t*)kmalloc(sizeof(dev_input_entry_t));
    if (!entry) {
        kprintf("[DEV_INPUT] Failed to allocate entry\n");
        return -1;
    }

    memset(entry, 0, sizeof(dev_input_entry_t));
    // Copy name manually (strncpy may not be available)
    size_t i;
    for (i = 0; i < sizeof(entry->name) - 1 && name[i]; i++) {
        entry->name[i] = name[i];
    }
    entry->name[i] = '\0';
    entry->inode = inode;
    vfs_inode_get(inode);

    // Add to list
    entry->next = dev_input_entries;
    dev_input_entries = entry;

    // Actually create the VFS node by linking the inode into /dev/input so
    // it is reachable via vfs_path_lookup()/vfs_open(). If /dev/input is not
    // ready yet this is a soft failure; the legacy entries list above still
    // tracks it for dev_input_open()/dev_input_list().
    if (dev_input_link_node(name, inode) < 0) {
        kprintf("[DEV_INPUT] Note: %s tracked but not yet linked in VFS\n", name);
    }

    return 0;
}

/*
 * Register input device and create /dev/input/eventX node.
 *
 * The heavy lifting is done by evdev_register_device(): it creates the ONE
 * canonical VFS_TYPE_DEVICE inode (whose private_data carries evdev_fops via
 * a vfs_devnode_t) and links it into /dev/input. We deliberately do NOT
 * create a second stray inode here -- that previously produced a node with no
 * working file operations.
 */
int dev_input_register_device(input_device_t* input_dev) {
    if (!input_dev) return -1;

    // Register with evdev (creates canonical inode + links it into /dev/input)
    int minor = evdev_register_device(input_dev);
    if (minor < 0) {
        kprintf("[DEV_INPUT] Failed to register with evdev\n");
        return -1;
    }

    kprintf("[DEV_INPUT] Registered input device as /dev/input/event%d\n", minor);
    return 0;
}

/*
 * Initialize /dev/input subsystem
 */
void dev_input_init(void) {
    kprintf("[DEV_INPUT] Initializing /dev/input subsystem\n");

    // Initialize evdev first
    evdev_init();

    // Create /dev/input directory
    dev_input_create_dir();

    kprintf("[DEV_INPUT] /dev/input subsystem ready\n");
}

/*
 * Open a /dev/input device node
 */
int dev_input_open(const char* path) {
    if (!path) return -1;

    // Strip /dev/input/ prefix
    const char* name = path;
    const char* prefix = "/dev/input/";
    size_t prefix_len = strlen(prefix);

    if (strncmp(path, prefix, prefix_len) == 0) {
        name = path + prefix_len;
    }

    // Find entry
    dev_input_entry_t* entry = dev_input_entries;
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            // Found - use VFS open
            return vfs_open(path, O_RDONLY, 0);
        }
        entry = entry->next;
    }

    kprintf("[DEV_INPUT] Device not found: %s\n", path);
    return -1;
}

/*
 * List all /dev/input devices (for debugging)
 */
void dev_input_list(void) {
    kprintf("[DEV_INPUT] Devices:\n");

    dev_input_entry_t* entry = dev_input_entries;
    while (entry) {
        kprintf("[DEV_INPUT]   /dev/input/%s\n", entry->name);
        entry = entry->next;
    }
}
