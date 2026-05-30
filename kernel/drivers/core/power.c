/*
 * Device Power Management
 * Handles suspend/resume, runtime PM, and system sleep states
 */

#include "../../include/device.h"
#include "../../include/kernel.h"

// Autosuspend delay (milliseconds)
#define DEFAULT_AUTOSUSPEND_DELAY 2000

// Runtime PM state
typedef struct {
    bool enabled;
    uint32_t usage_count;
    uint32_t autosuspend_delay;
    uint64_t last_busy_time;
    bool autosuspend_pending;
} runtime_pm_state_t;

/**
 * Suspend device
 */
int device_pm_suspend(device_t* dev) {
    if (!dev) {
        return -1;
    }

    if (dev->state == DEVICE_STATE_SUSPENDED) {
        return 0;  // Already suspended
    }

    // Call driver suspend callback
    int ret = 0;
    if (dev->pm_ops && dev->pm_ops->suspend) {
        ret = dev->pm_ops->suspend(dev);
    } else if (dev->driver && dev->driver->pm_ops && dev->driver->pm_ops->suspend) {
        ret = dev->driver->pm_ops->suspend(dev);
    }

    if (ret == 0) {
        dev->state = DEVICE_STATE_SUSPENDED;
        dev->power_state = DEVICE_POWER_D3_HOT;
        dev->flags |= DEVICE_FLAG_SUSPENDED;
        kprintf("[PM] Suspended device: %s\n", dev->name);
    } else {
        kprintf("[PM] Failed to suspend device: %s (ret=%d)\n", dev->name, ret);
    }

    return ret;
}

/**
 * Resume device
 */
int device_pm_resume(device_t* dev) {
    if (!dev) {
        return -1;
    }

    if (dev->state != DEVICE_STATE_SUSPENDED) {
        return 0;  // Not suspended
    }

    // Call driver resume callback
    int ret = 0;
    if (dev->pm_ops && dev->pm_ops->resume) {
        ret = dev->pm_ops->resume(dev);
    } else if (dev->driver && dev->driver->pm_ops && dev->driver->pm_ops->resume) {
        ret = dev->driver->pm_ops->resume(dev);
    }

    if (ret == 0) {
        dev->state = DEVICE_STATE_ACTIVE;
        dev->power_state = DEVICE_POWER_D0;
        dev->flags &= ~DEVICE_FLAG_SUSPENDED;
        kprintf("[PM] Resumed device: %s\n", dev->name);
    } else {
        kprintf("[PM] Failed to resume device: %s (ret=%d)\n", dev->name, ret);
    }

    return ret;
}

/**
 * Runtime suspend device
 */
int device_pm_runtime_suspend(device_t* dev) {
    if (!dev) {
        return -1;
    }

    // Check if runtime PM is enabled
    if (!(dev->flags & DEVICE_FLAG_RUNTIME_PM)) {
        return -1;
    }

    // Call driver runtime suspend callback
    int ret = 0;
    if (dev->pm_ops && dev->pm_ops->runtime_suspend) {
        ret = dev->pm_ops->runtime_suspend(dev);
    } else if (dev->driver && dev->driver->pm_ops && dev->driver->pm_ops->runtime_suspend) {
        ret = dev->driver->pm_ops->runtime_suspend(dev);
    }

    if (ret == 0) {
        dev->power_state = DEVICE_POWER_D3_HOT;
        dev->runtime_suspend_time = timer_get_ticks();
        kprintf("[PM] Runtime suspended: %s\n", dev->name);
    }

    return ret;
}

/**
 * Runtime resume device
 */
int device_pm_runtime_resume(device_t* dev) {
    if (!dev) {
        return -1;
    }

    // Check if runtime PM is enabled
    if (!(dev->flags & DEVICE_FLAG_RUNTIME_PM)) {
        return -1;
    }

    // Already active
    if (dev->power_state == DEVICE_POWER_D0) {
        return 0;
    }

    // Call driver runtime resume callback
    int ret = 0;
    if (dev->pm_ops && dev->pm_ops->runtime_resume) {
        ret = dev->pm_ops->runtime_resume(dev);
    } else if (dev->driver && dev->driver->pm_ops && dev->driver->pm_ops->runtime_resume) {
        ret = dev->driver->pm_ops->runtime_resume(dev);
    }

    if (ret == 0) {
        dev->power_state = DEVICE_POWER_D0;
        kprintf("[PM] Runtime resumed: %s\n", dev->name);
    }

    return ret;
}

/**
 * Enable wakeup capability
 */
void device_pm_enable_wakeup(device_t* dev) {
    if (dev) {
        dev->can_wakeup = true;
        kprintf("[PM] Enabled wakeup for device: %s\n", dev->name);
    }
}

/**
 * Disable wakeup capability
 */
void device_pm_disable_wakeup(device_t* dev) {
    if (dev) {
        dev->can_wakeup = false;
        dev->should_wakeup = false;
        dev->flags &= ~DEVICE_FLAG_WAKEUP_ARMED;
        kprintf("[PM] Disabled wakeup for device: %s\n", dev->name);
    }
}

/**
 * Check if device can wakeup
 */
bool device_pm_can_wakeup(device_t* dev) {
    return dev ? dev->can_wakeup : false;
}

/**
 * Suspend all devices in the system
 */
int device_pm_suspend_all(void) {
    // TODO: Traverse device tree and suspend in reverse order
    // (children before parents)
    kprintf("[PM] Suspending all devices...\n");
    return 0;
}

/**
 * Resume all devices in the system
 */
int device_pm_resume_all(void) {
    // TODO: Traverse device tree and resume in forward order
    // (parents before children)
    kprintf("[PM] Resuming all devices...\n");
    return 0;
}

/**
 * Prepare all devices for suspend
 */
int device_pm_prepare_all(void) {
    // TODO: Call prepare callbacks
    kprintf("[PM] Preparing all devices for suspend...\n");
    return 0;
}

/**
 * Complete resume of all devices
 */
void device_pm_complete_all(void) {
    // TODO: Call complete callbacks
    kprintf("[PM] Completing resume of all devices...\n");
}

/**
 * Enable runtime PM for device
 */
void device_pm_runtime_enable(device_t* dev) {
    if (dev) {
        dev->flags |= DEVICE_FLAG_RUNTIME_PM;
        kprintf("[PM] Enabled runtime PM for device: %s\n", dev->name);
    }
}

/**
 * Disable runtime PM for device
 */
void device_pm_runtime_disable(device_t* dev) {
    if (dev) {
        dev->flags &= ~DEVICE_FLAG_RUNTIME_PM;
        kprintf("[PM] Disabled runtime PM for device: %s\n", dev->name);
    }
}

/**
 * Increment runtime PM usage count (prevents auto-suspend)
 */
int device_pm_runtime_get(device_t* dev) {
    if (!dev) {
        return -1;
    }

    // Resume if suspended
    if (dev->power_state != DEVICE_POWER_D0) {
        int ret = device_pm_runtime_resume(dev);
        if (ret < 0) {
            return ret;
        }
    }

    return 0;
}

/**
 * Decrement runtime PM usage count
 */
void device_pm_runtime_put(device_t* dev) {
    if (!dev) {
        return;
    }

    // Device becomes idle, can be suspended
    dev->runtime_idle_time = timer_get_ticks();
}

/**
 * Decrement usage count and schedule autosuspend
 */
void device_pm_runtime_put_autosuspend(device_t* dev) {
    if (!dev) {
        return;
    }

    device_pm_runtime_put(dev);

    // TODO: Schedule autosuspend after delay
    // For now, just mark idle time
    dev->runtime_idle_time = timer_get_ticks();
}

/**
 * Set autosuspend delay
 */
void device_pm_runtime_set_autosuspend_delay(device_t* dev, uint32_t delay_ms) {
    if (dev) {
        // TODO: Store delay in runtime PM state
        kprintf("[PM] Set autosuspend delay for %s: %u ms\n", dev->name, delay_ms);
    }
}
