/*
 * Driver Registration and Management
 * Handles driver registration, device-driver binding, and auto-loading
 */

#include "../../include/device.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"

// Simple spinlock placeholder
typedef struct { uint32_t locked; } spinlock_t;
static void spin_lock(spinlock_t* lock) { while (__sync_lock_test_and_set(&lock->locked, 1)); }
static void spin_unlock(spinlock_t* lock) { __sync_lock_release(&lock->locked); }

/**
 * Register a driver with the system
 */
int driver_register(driver_t* drv) {
    if (!drv || !drv->bus) {
        return -1;
    }

    // Add to bus driver list
    int ret = bus_add_driver(drv->bus, drv);
    if (ret < 0) {
        return ret;
    }

    kprintf("[DRIVER] Registered driver: %s on bus %s\n", drv->name, drv->bus->name);

    // Try to bind to existing devices
    device_t* dev = drv->bus->devices;
    while (dev) {
        if (!dev->driver && device_match_driver(dev, drv)) {
            driver_bind(dev, drv);
        }
        dev = dev->bus_next;
    }

    return 0;
}

/**
 * Unregister a driver from the system
 */
void driver_unregister(driver_t* drv) {
    if (!drv || !drv->bus) {
        return;
    }

    // Unbind from all devices
    device_t* dev = drv->bus->devices;
    while (dev) {
        if (dev->driver == drv) {
            driver_unbind(dev);
        }
        dev = dev->bus_next;
    }

    // Remove from bus
    bus_remove_driver(drv->bus, drv);

    kprintf("[DRIVER] Unregistered driver: %s\n", drv->name);
}

/**
 * Attach a driver to a device (find matching driver)
 */
int driver_attach(device_t* dev) {
    if (!dev || !dev->bus) {
        return -1;
    }

    // Device already has a driver
    if (dev->driver) {
        return 0;
    }

    // Find matching driver with highest priority
    driver_t* best_drv = NULL;
    uint32_t best_priority = 0;

    driver_t* drv = dev->bus->drivers;
    while (drv) {
        if (device_match_driver(dev, drv)) {
            if (!best_drv || drv->priority > best_priority) {
                best_drv = drv;
                best_priority = drv->priority;
            }
        }
        drv = drv->bus_next;
    }

    if (best_drv) {
        return driver_bind(dev, best_drv);
    }

    return -1;  // No matching driver found
}

/**
 * Detach driver from device
 */
void driver_detach(device_t* dev) {
    driver_unbind(dev);
}

/**
 * Bind driver to device
 */
int driver_bind(device_t* dev, driver_t* drv) {
    if (!dev || !drv) {
        return -1;
    }

    // Check if already bound
    if (dev->driver) {
        return -1;
    }

    // Check if driver matches
    if (!device_match_driver(dev, drv)) {
        return -1;
    }

    // Bind driver to device
    dev->driver = drv;

    // Probe device
    int ret = device_probe(dev);
    if (ret < 0) {
        dev->driver = NULL;
        return ret;
    }

    kprintf("[DRIVER] Bound device %s to driver %s\n", dev->name, drv->name);
    return 0;
}

/**
 * Unbind driver from device
 */
void driver_unbind(device_t* dev) {
    if (!dev || !dev->driver) {
        return;
    }

    driver_t* drv = dev->driver;

    // Set state to removing
    dev->state = DEVICE_STATE_REMOVING;

    // Call driver remove
    if (drv->remove) {
        drv->remove(dev);
    }

    // Clear driver pointer
    dev->driver = NULL;
    dev->flags &= ~DEVICE_FLAG_PROBED;
    dev->state = DEVICE_STATE_ACTIVE;

    kprintf("[DRIVER] Unbound device %s from driver %s\n", dev->name, drv->name);
}
