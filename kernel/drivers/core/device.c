/*
 * Device Model Core - Unified device representation
 * Handles device lifecycle, registration, and tree management
 */

#include "../../include/device.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"
#include "../../include/spinlock.h"
#include <stdarg.h>

// Global device tree root
static device_t* root_device = NULL;

// Event callback list
#define MAX_EVENT_CALLBACKS 32
static device_event_callback_t event_callbacks[MAX_EVENT_CALLBACKS];
static uint32_t num_event_callbacks = 0;

// RACE-005 fix: Global callback lock protects event_callbacks[] array
static spinlock_t callback_lock;

// Simple spinlock placeholder (to be replaced with actual spinlock)
typedef struct { uint32_t locked; } spinlock_t;
static void spin_lock(spinlock_t* lock) { while (__sync_lock_test_and_set(&lock->locked, 1)); }
static void spin_unlock(spinlock_t* lock) { __sync_lock_release(&lock->locked); }
static void spinlock_init(spinlock_t* lock) { lock->locked = 0; }

/**
 * Initialize device subsystem
 * RACE-005 fix: Initialize callback lock
 */
int device_init(void) {
    // Initialize callback lock
    spinlock_init(&callback_lock);

    // Create root device
    root_device = device_alloc("root");
    if (!root_device) {
        return -1;
    }

    root_device->state = DEVICE_STATE_ACTIVE;
    root_device->parent = NULL;

    kprintf("[DEVICE] Device subsystem initialized (SMP-safe)\n");
    return 0;
}

/**
 * Allocate a new device structure
 */
device_t* device_alloc(const char* name) {
    device_t* dev = (device_t*)kmalloc(sizeof(device_t));
    if (!dev) {
        return NULL;
    }

    memset(dev, 0, sizeof(device_t));

    // Copy name
    size_t len = strlen(name);
    if (len >= sizeof(dev->name)) {
        len = sizeof(dev->name) - 1;
    }
    memcpy(dev->name, name, len);
    dev->name[len] = '\0';

    // Initialize fields
    dev->state = DEVICE_STATE_UNINITIALIZED;
    dev->power_state = DEVICE_POWER_D0;
    dev->refcount = 1;

    // Allocate lock
    dev->lock = kmalloc(sizeof(spinlock_t));
    if (dev->lock) {
        spinlock_init((spinlock_t*)dev->lock);
    }

    return dev;
}

/**
 * Free device structure
 */
void device_free(device_t* dev) {
    if (!dev) {
        return;
    }

    // Free path if allocated
    if (dev->path) {
        kfree(dev->path);
    }

    // Free lock
    if (dev->lock) {
        kfree(dev->lock);
    }

    kfree(dev);
}

/**
 * Register a device with the system
 */
int device_register(device_t* dev) {
    if (!dev) {
        return -1;
    }

    // Mark as registered
    dev->flags |= DEVICE_FLAG_REGISTERED;

    // Add to bus if specified
    if (dev->bus) {
        int ret = bus_add_device(dev->bus, dev);
        if (ret < 0) {
            dev->flags &= ~DEVICE_FLAG_REGISTERED;
            return ret;
        }
    }

    // Notify event callbacks
    device_notify_event(dev, DEVICE_EVENT_ADD);

    // Try to bind a driver
    driver_attach(dev);

    kprintf("[DEVICE] Registered device: %s\n", dev->name);
    return 0;
}

/**
 * Unregister a device from the system
 */
void device_unregister(device_t* dev) {
    if (!dev) {
        return;
    }

    // Unbind driver if attached
    if (dev->driver) {
        driver_unbind(dev);
    }

    // Remove from bus
    if (dev->bus) {
        bus_remove_device(dev->bus, dev);
    }

    // Remove from device tree
    if (dev->parent) {
        device_remove_child(dev->parent, dev);
    }

    // Notify event callbacks
    device_notify_event(dev, DEVICE_EVENT_REMOVE);

    // Mark as removed
    dev->flags &= ~DEVICE_FLAG_REGISTERED;
    dev->state = DEVICE_STATE_REMOVED;

    kprintf("[DEVICE] Unregistered device: %s\n", dev->name);
}

/**
 * Delete device (called after unregister)
 */
void device_del(device_t* dev) {
    device_unregister(dev);
}

/**
 * Add device to system (variant of register)
 */
int device_add(device_t* dev) {
    return device_register(dev);
}

/**
 * Set parent device
 */
void device_set_parent(device_t* dev, device_t* parent) {
    if (!dev) {
        return;
    }

    dev->parent = parent;

    if (parent) {
        device_add_child(parent, dev);
    }
}

/**
 * Get parent device
 */
device_t* device_get_parent(device_t* dev) {
    return dev ? dev->parent : NULL;
}

/**
 * Add child device to parent
 */
void device_add_child(device_t* parent, device_t* child) {
    if (!parent || !child) {
        return;
    }

    spinlock_t* lock = (spinlock_t*)parent->lock;
    if (lock) {
        spin_lock(lock);
    }

    // Add to front of children list
    child->sibling = parent->children;
    parent->children = child;
    child->parent = parent;

    if (lock) {
        spin_unlock(lock);
    }
}

/**
 * Remove child device from parent
 */
void device_remove_child(device_t* parent, device_t* child) {
    if (!parent || !child) {
        return;
    }

    spinlock_t* lock = (spinlock_t*)parent->lock;
    if (lock) {
        spin_lock(lock);
    }

    // Find and remove from children list
    device_t** prev = &parent->children;
    device_t* curr = parent->children;

    while (curr) {
        if (curr == child) {
            *prev = curr->sibling;
            curr->sibling = NULL;
            curr->parent = NULL;
            break;
        }
        prev = &curr->sibling;
        curr = curr->sibling;
    }

    if (lock) {
        spin_unlock(lock);
    }
}

/**
 * Increment device reference count
 */
void device_get(device_t* dev) {
    if (dev) {
        __sync_fetch_and_add(&dev->refcount, 1);
    }
}

/**
 * Decrement device reference count and free if zero
 */
void device_put(device_t* dev) {
    if (!dev) {
        return;
    }

    uint32_t old = __sync_fetch_and_sub(&dev->refcount, 1);
    if (old == 1) {
        // Last reference, free device
        device_free(dev);
    }
}

/**
 * Set driver private data
 */
void device_set_driver_data(device_t* dev, void* data) {
    if (dev) {
        dev->driver_data = data;
    }
}

/**
 * Get driver private data
 */
void* device_get_driver_data(device_t* dev) {
    return dev ? dev->driver_data : NULL;
}

/**
 * Set device name (with printf-style formatting)
 */
int device_set_name(device_t* dev, const char* fmt, ...) {
    if (!dev || !fmt) {
        return -1;
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf(dev->name, sizeof(dev->name), fmt, args);
    va_end(args);

    return 0;
}

/**
 * Get device name
 */
const char* device_get_name(device_t* dev) {
    return dev ? dev->name : NULL;
}

/**
 * Register device event callback
 * RACE-005 fix: Protect callback array with lock
 */
int device_register_event_callback(device_event_callback_t callback) {
    if (!callback) {
        return -1;
    }

    spin_lock(&callback_lock);

    if (num_event_callbacks >= MAX_EVENT_CALLBACKS) {
        spin_unlock(&callback_lock);
        return -1;
    }

    event_callbacks[num_event_callbacks++] = callback;

    spin_unlock(&callback_lock);
    return 0;
}

/**
 * Unregister device event callback
 * RACE-005 fix: Protect callback array with lock
 */
void device_unregister_event_callback(device_event_callback_t callback) {
    if (!callback) {
        return;
    }

    spin_lock(&callback_lock);

    for (uint32_t i = 0; i < num_event_callbacks; i++) {
        if (event_callbacks[i] == callback) {
            // Shift remaining callbacks down
            for (uint32_t j = i; j < num_event_callbacks - 1; j++) {
                event_callbacks[j] = event_callbacks[j + 1];
            }
            num_event_callbacks--;
            spin_unlock(&callback_lock);
            return;
        }
    }

    spin_unlock(&callback_lock);
}

/**
 * Notify all registered callbacks of device event
 * RACE-005 fix: Make local copy of callbacks before invoking (prevents deadlock)
 */
void device_notify_event(device_t* dev, device_event_t event) {
    if (!dev) {
        return;
    }

    const char* event_str = "UNKNOWN";
    switch (event) {
        case DEVICE_EVENT_ADD: event_str = "ADD"; break;
        case DEVICE_EVENT_REMOVE: event_str = "REMOVE"; break;
        case DEVICE_EVENT_CHANGE: event_str = "CHANGE"; break;
        case DEVICE_EVENT_ONLINE: event_str = "ONLINE"; break;
        case DEVICE_EVENT_OFFLINE: event_str = "OFFLINE"; break;
    }

    kprintf("[DEVICE] Event %s: %s\n", event_str, dev->name);

    // RACE-005 fix: Make local copy of callback list before invoking
    // This prevents deadlock if callback tries to register/unregister
    spin_lock(&callback_lock);
    uint32_t count = num_event_callbacks;
    device_event_callback_t local_callbacks[MAX_EVENT_CALLBACKS];
    for (uint32_t i = 0; i < count; i++) {
        local_callbacks[i] = event_callbacks[i];
    }
    spin_unlock(&callback_lock);

    // Call callbacks outside lock (prevents deadlock)
    for (uint32_t i = 0; i < count; i++) {
        local_callbacks[i](dev, event);
    }
}

/**
 * Match device with driver
 */
int device_match_driver(device_t* dev, driver_t* drv) {
    if (!dev || !drv) {
        return 0;
    }

    // Check bus type matches
    if (dev->bus != drv->bus) {
        return 0;
    }

    // Use driver's match function if available
    if (drv->match) {
        return drv->match(dev, drv);
    }

    // Use bus's match function if available
    if (dev->bus && dev->bus->match) {
        return dev->bus->match(dev, drv);
    }

    // Default: no match
    return 0;
}

/**
 * Probe device with matched driver
 */
int device_probe(device_t* dev) {
    if (!dev || !dev->driver) {
        return -1;
    }

    driver_t* drv = dev->driver;

    // Set state to probing
    dev->state = DEVICE_STATE_PROBING;

    // Call driver probe
    int ret = 0;
    if (drv->probe) {
        ret = drv->probe(dev);
    }

    if (ret == 0) {
        dev->state = DEVICE_STATE_ACTIVE;
        dev->flags |= DEVICE_FLAG_PROBED;
        kprintf("[DEVICE] Probed %s with driver %s\n", dev->name, drv->name);
    } else {
        dev->state = DEVICE_STATE_ERROR;
        kprintf("[DEVICE] Probe failed for %s with driver %s (ret=%d)\n",
                dev->name, drv->name, ret);
    }

    return ret;
}

/**
 * Reprobe device (unbind and rebind)
 */
void device_reprobe(device_t* dev) {
    if (!dev) {
        return;
    }

    driver_unbind(dev);
    driver_attach(dev);
}
