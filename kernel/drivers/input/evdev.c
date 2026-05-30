/*
 * Event Device (evdev) - Character device interface for input events
 * Provides /dev/input/event0, /dev/input/event1, etc.
 * Compatible with Linux evdev protocol
 */

#include "../../include/input.h"
#include "../../include/kernel.h"
#include "../../include/types.h"
#include "../../include/vfs.h"
#include "../../include/mem.h"
#include "../../include/string.h"

// O_NONBLOCK if not defined
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x0800
#endif

#define EVDEV_MAX_DEVICES 16
#define EVDEV_BUFFER_SIZE 64

// evdev client structure - represents an open /dev/input/eventX file
typedef struct evdev_client {
    input_device_t* input_dev;
    input_event_t buffer[EVDEV_BUFFER_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t packet_count;
    bool nonblock;
    struct evdev_client* next;
} evdev_client_t;

// evdev device structure - represents /dev/input/eventX
typedef struct evdev_device {
    uint32_t minor;
    input_device_t* input_dev;
    evdev_client_t* clients;
    vfs_inode_t* inode;
    char devname[32];
} evdev_device_t;

static evdev_device_t* evdev_devices[EVDEV_MAX_DEVICES];
static uint32_t evdev_count = 0;

// Forward declarations
static ssize_t evdev_read(vfs_file_t* file, void* buf, size_t count);
static ssize_t evdev_write(vfs_file_t* file, const void* buf, size_t count);
static int evdev_open(vfs_inode_t* inode, vfs_file_t* file);
static int evdev_close(vfs_file_t* file);

static vfs_file_ops_t evdev_fops = {
    .read = evdev_read,
    .write = evdev_write,
    .open = evdev_open,
    .close = evdev_close,
    .lseek = NULL,
};

/*
 * Push event to all clients of a device
 */
static void evdev_event(input_device_t* dev, input_event_t* event) {
    // Find evdev device for this input device
    for (uint32_t i = 0; i < evdev_count; i++) {
        evdev_device_t* evdev = evdev_devices[i];
        if (evdev && evdev->input_dev == dev) {
            // Push to all clients
            evdev_client_t* client = evdev->clients;
            while (client) {
                uint32_t next_tail = (client->tail + 1) % EVDEV_BUFFER_SIZE;
                if (next_tail != client->head) {
                    client->buffer[client->tail] = *event;
                    client->tail = next_tail;
                    client->packet_count++;
                } else {
                    // Buffer full, drop oldest event
                    client->head = (client->head + 1) % EVDEV_BUFFER_SIZE;
                    client->buffer[client->tail] = *event;
                    client->tail = next_tail;
                }
                client = client->next;
            }
            break;
        }
    }
}

/*
 * Input event handler - called by input subsystem
 */
static void evdev_input_event(input_device_t* dev, uint16_t type, uint16_t code, int32_t value) {
    input_event_t event;
    event.timestamp = 0; // Will be set by input system
    event.type = type;
    event.code = code;
    event.value = value;

    evdev_event(dev, &event);
}

/*
 * Open evdev device
 */
static int evdev_open(vfs_inode_t* inode, vfs_file_t* file) {
    if (!inode || !file) return -1;

    // Find evdev device by inode
    evdev_device_t* evdev = NULL;
    for (uint32_t i = 0; i < evdev_count; i++) {
        if (evdev_devices[i] && evdev_devices[i]->inode == inode) {
            evdev = evdev_devices[i];
            break;
        }
    }

    if (!evdev) {
        kprintf("[EVDEV] Device not found for inode %p\n", inode);
        return -1;
    }

    // Allocate client
    evdev_client_t* client = (evdev_client_t*)kmalloc(sizeof(evdev_client_t));
    if (!client) {
        kprintf("[EVDEV] Failed to allocate client\n");
        return -1;
    }

    memset(client, 0, sizeof(evdev_client_t));
    client->input_dev = evdev->input_dev;
    client->head = 0;
    client->tail = 0;
    client->packet_count = 0;
    client->nonblock = (file->flags & O_NONBLOCK) ? true : false;

    // Add to client list
    client->next = evdev->clients;
    evdev->clients = client;

    file->private_data = client;

    kprintf("[EVDEV] Opened %s (client %p)\n", evdev->devname, client);
    return 0;
}

/*
 * Close evdev device
 */
static int evdev_close(vfs_file_t* file) {
    if (!file || !file->private_data) return -1;

    evdev_client_t* client = (evdev_client_t*)file->private_data;

    // Find and remove from device's client list
    for (uint32_t i = 0; i < evdev_count; i++) {
        evdev_device_t* evdev = evdev_devices[i];
        if (evdev && evdev->input_dev == client->input_dev) {
            evdev_client_t** prev = &evdev->clients;
            evdev_client_t* curr = evdev->clients;

            while (curr) {
                if (curr == client) {
                    *prev = curr->next;
                    kfree(curr);
                    file->private_data = NULL;
                    kprintf("[EVDEV] Closed client %p\n", client);
                    return 0;
                }
                prev = &curr->next;
                curr = curr->next;
            }
        }
    }

    // Fallback: free client anyway
    kfree(client);
    file->private_data = NULL;
    return 0;
}

/*
 * Read events from evdev device
 */
static ssize_t evdev_read(vfs_file_t* file, void* buf, size_t count) {
    if (!file || !file->private_data || !buf) return -1;

    evdev_client_t* client = (evdev_client_t*)file->private_data;

    // Calculate number of events to read
    size_t event_size = sizeof(input_event_t);
    size_t events_requested = count / event_size;

    if (events_requested == 0) {
        return -1; // Buffer too small
    }

    // Check if events available
    if (client->head == client->tail) {
        if (client->nonblock) {
            return 0; // Would block
        }
        // TODO: Implement blocking wait
        return 0;
    }

    // Copy events to userspace buffer
    size_t events_read = 0;
    input_event_t* event_buf = (input_event_t*)buf;

    while (events_read < events_requested && client->head != client->tail) {
        event_buf[events_read] = client->buffer[client->head];
        client->head = (client->head + 1) % EVDEV_BUFFER_SIZE;
        events_read++;
    }

    return events_read * event_size;
}

/*
 * Write to evdev device (for LED control, force feedback, etc.)
 */
static ssize_t evdev_write(vfs_file_t* file, const void* buf, size_t count) {
    (void)file;
    (void)buf;
    (void)count;

    // TODO: Implement write for LED/FF control
    return -1; // Not implemented
}

/*
 * Register input device with evdev
 */
int evdev_register_device(input_device_t* input_dev) {
    if (!input_dev) return -1;

    if (evdev_count >= EVDEV_MAX_DEVICES) {
        kprintf("[EVDEV] Maximum devices reached\n");
        return -1;
    }

    // Allocate evdev device
    evdev_device_t* evdev = (evdev_device_t*)kmalloc(sizeof(evdev_device_t));
    if (!evdev) {
        kprintf("[EVDEV] Failed to allocate device\n");
        return -1;
    }

    memset(evdev, 0, sizeof(evdev_device_t));
    evdev->minor = evdev_count;
    evdev->input_dev = input_dev;
    evdev->clients = NULL;

    // Create device name (simple formatting without snprintf)
    char* name_ptr = evdev->devname;
    const char* prefix = "event";
    while (*prefix) {
        *name_ptr++ = *prefix++;
    }
    // Convert minor to string
    uint32_t minor = evdev->minor;
    if (minor == 0) {
        *name_ptr++ = '0';
    } else {
        char digits[10];
        int digit_count = 0;
        while (minor > 0) {
            digits[digit_count++] = '0' + (minor % 10);
            minor /= 10;
        }
        while (digit_count > 0) {
            *name_ptr++ = digits[--digit_count];
        }
    }
    *name_ptr = '\0';

    // Create the ONE canonical VFS inode for this device. This same inode is
    // both stored on the evdev_device (so evdev_open()'s pointer-equality
    // match works) and linked into the /dev/input directory (so
    // vfs_path_lookup("/dev/input/eventN") returns it).
    evdev->inode = vfs_inode_alloc(NULL);
    if (!evdev->inode) {
        kprintf("[EVDEV] Failed to create inode\n");
        kfree(evdev);
        return -1;
    }

    // Build the device-node descriptor so vfs_open() can route file ops to
    // evdev_fops instead of the default ramfs ops. (See VFS_DEVNODE_MAGIC
    // and the integration note in input.h.)
    vfs_devnode_t* devnode = (vfs_devnode_t*)kmalloc(sizeof(vfs_devnode_t));
    if (!devnode) {
        kprintf("[EVDEV] Failed to allocate devnode descriptor\n");
        vfs_inode_put(evdev->inode);
        kfree(evdev);
        return -1;
    }
    devnode->magic = VFS_DEVNODE_MAGIC;
    devnode->fops = &evdev_fops;
    devnode->driver_data = evdev;

    evdev->inode->type = VFS_TYPE_DEVICE;
    evdev->inode->mode = 0666; // rw-rw-rw-
    evdev->inode->ops = NULL;             // no inode (lookup/create) ops
    evdev->inode->private_data = devnode; // device-node descriptor

    // Store before linking so evdev_open() can find it during open.
    evdev_devices[evdev_count++] = evdev;

    // Link the inode into /dev/input so it is reachable by path. This is a
    // best-effort step: if /dev/input has not been created yet the device is
    // still tracked in evdev_devices[] and dev_input can link it later.
    if (dev_input_link_node(evdev->devname, evdev->inode) < 0) {
        kprintf("[EVDEV] Warning: could not link %s into /dev/input yet\n",
                evdev->devname);
    }

    kprintf("[EVDEV] Registered %s for device %s (minor %u)\n",
            evdev->devname, input_dev->name, evdev->minor);

    return evdev->minor;
}

/*
 * Unregister input device from evdev
 */
void evdev_unregister_device(input_device_t* input_dev) {
    if (!input_dev) return;

    for (uint32_t i = 0; i < evdev_count; i++) {
        evdev_device_t* evdev = evdev_devices[i];
        if (evdev && evdev->input_dev == input_dev) {
            // Close all clients
            evdev_client_t* client = evdev->clients;
            while (client) {
                evdev_client_t* next = client->next;
                kfree(client);
                client = next;
            }

            // Free the device-node descriptor and inode
            if (evdev->inode) {
                if (evdev->inode->private_data) {
                    kfree(evdev->inode->private_data);
                    evdev->inode->private_data = NULL;
                }
                vfs_inode_put(evdev->inode);
            }

            kprintf("[EVDEV] Unregistered %s\n", evdev->devname);

            kfree(evdev);
            evdev_devices[i] = NULL;
            return;
        }
    }
}

/*
 * Get evdev device by minor number
 */
evdev_device_t* evdev_get_device(uint32_t minor) {
    if (minor < evdev_count) {
        return evdev_devices[minor];
    }
    return NULL;
}

/*
 * Get evdev file operations
 */
vfs_file_ops_t* evdev_get_fops(void) {
    return &evdev_fops;
}

/*
 * Initialize evdev subsystem
 */
void evdev_init(void) {
    kprintf("[EVDEV] Initializing evdev subsystem\n");

    memset(evdev_devices, 0, sizeof(evdev_devices));
    evdev_count = 0;

    kprintf("[EVDEV] Event device interface ready\n");
}

/*
 * Hook input events to evdev (called by input subsystem)
 */
void evdev_handle_event(input_device_t* dev, input_event_t* event) {
    if (!dev || !event) return;
    evdev_event(dev, event);
}
