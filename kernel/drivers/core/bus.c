/*
 * Bus Type Management
 * Handles bus registration and device-driver lists per bus
 */

#include "../../include/device.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"

// Simple spinlock placeholder
typedef struct { uint32_t locked; } spinlock_t;
static void spin_lock(spinlock_t* lock) { while (__sync_lock_test_and_set(&lock->locked, 1)); }
static void spin_unlock(spinlock_t* lock) { __sync_lock_release(&lock->locked); }
static void spinlock_init(spinlock_t* lock) { lock->locked = 0; }

// Global bus list
static bus_type_t* bus_list = NULL;

/**
 * Register a bus type
 */
int bus_register(bus_type_t* bus) {
    if (!bus || !bus->name) {
        return -1;
    }

    // Allocate lock
    bus->lock = kmalloc(sizeof(spinlock_t));
    if (!bus->lock) {
        return -1;
    }
    spinlock_init((spinlock_t*)bus->lock);

    // Initialize device and driver lists
    bus->devices = NULL;
    bus->drivers = NULL;

    // Add to global bus list
    // TODO: Add proper linked list support
    if (!bus_list) {
        bus_list = bus;
    }

    kprintf("[BUS] Registered bus: %s\n", bus->name);
    return 0;
}

/**
 * Unregister a bus type
 */
void bus_unregister(bus_type_t* bus) {
    if (!bus) {
        return;
    }

    // TODO: Remove all devices and drivers

    if (bus->lock) {
        kfree(bus->lock);
        bus->lock = NULL;
    }

    kprintf("[BUS] Unregistered bus: %s\n", bus->name);
}

/**
 * Add device to bus
 */
int bus_add_device(bus_type_t* bus, device_t* dev) {
    if (!bus || !dev) {
        return -1;
    }

    spinlock_t* lock = (spinlock_t*)bus->lock;
    if (lock) {
        spin_lock(lock);
    }

    // Add to front of device list
    dev->bus_next = bus->devices;
    bus->devices = dev;
    dev->bus = bus;

    if (lock) {
        spin_unlock(lock);
    }

    kprintf("[BUS] Added device %s to bus %s\n", dev->name, bus->name);
    return 0;
}

/**
 * Remove device from bus
 */
void bus_remove_device(bus_type_t* bus, device_t* dev) {
    if (!bus || !dev) {
        return;
    }

    spinlock_t* lock = (spinlock_t*)bus->lock;
    if (lock) {
        spin_lock(lock);
    }

    // Find and remove from device list
    device_t** prev = &bus->devices;
    device_t* curr = bus->devices;

    while (curr) {
        if (curr == dev) {
            *prev = curr->bus_next;
            curr->bus_next = NULL;
            curr->bus = NULL;
            break;
        }
        prev = &curr->bus_next;
        curr = curr->bus_next;
    }

    if (lock) {
        spin_unlock(lock);
    }

    kprintf("[BUS] Removed device %s from bus %s\n", dev->name, bus->name);
}

/**
 * Add driver to bus
 */
int bus_add_driver(bus_type_t* bus, driver_t* drv) {
    if (!bus || !drv) {
        return -1;
    }

    spinlock_t* lock = (spinlock_t*)bus->lock;
    if (lock) {
        spin_lock(lock);
    }

    // Add to front of driver list
    drv->bus_next = bus->drivers;
    bus->drivers = drv;
    drv->bus = bus;

    if (lock) {
        spin_unlock(lock);
    }

    kprintf("[BUS] Added driver %s to bus %s\n", drv->name, bus->name);
    return 0;
}

/**
 * Remove driver from bus
 */
void bus_remove_driver(bus_type_t* bus, driver_t* drv) {
    if (!bus || !drv) {
        return;
    }

    spinlock_t* lock = (spinlock_t*)bus->lock;
    if (lock) {
        spin_lock(lock);
    }

    // Find and remove from driver list
    driver_t** prev = &bus->drivers;
    driver_t* curr = bus->drivers;

    while (curr) {
        if (curr == drv) {
            *prev = curr->bus_next;
            curr->bus_next = NULL;
            curr->bus = NULL;
            break;
        }
        prev = &curr->bus_next;
        curr = curr->bus_next;
    }

    if (lock) {
        spin_unlock(lock);
    }

    kprintf("[BUS] Removed driver %s from bus %s\n", drv->name, bus->name);
}

/**
 * Find device on bus by name
 */
device_t* bus_find_device(bus_type_t* bus, const char* name) {
    if (!bus || !name) {
        return NULL;
    }

    spinlock_t* lock = (spinlock_t*)bus->lock;
    if (lock) {
        spin_lock(lock);
    }

    device_t* dev = bus->devices;
    while (dev) {
        if (strcmp(dev->name, name) == 0) {
            if (lock) {
                spin_unlock(lock);
            }
            return dev;
        }
        dev = dev->bus_next;
    }

    if (lock) {
        spin_unlock(lock);
    }

    return NULL;
}

/**
 * Find driver on bus by name
 */
driver_t* bus_find_driver(bus_type_t* bus, const char* name) {
    if (!bus || !name) {
        return NULL;
    }

    spinlock_t* lock = (spinlock_t*)bus->lock;
    if (lock) {
        spin_lock(lock);
    }

    driver_t* drv = bus->drivers;
    while (drv) {
        if (strcmp(drv->name, name) == 0) {
            if (lock) {
                spin_unlock(lock);
            }
            return drv;
        }
        drv = drv->bus_next;
    }

    if (lock) {
        spin_unlock(lock);
    }

    return NULL;
}
