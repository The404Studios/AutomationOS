/*
 * IRQ Framework - Enhanced interrupt handling
 * Supports MSI/MSI-X, threaded IRQs, affinity, and softirqs
 */

#include "../../include/irq.h"
#include "../../include/device.h"
#include "../../include/kernel.h"
#include "../../include/mem.h"

#define MAX_IRQS 256

// IRQ descriptor table
static irq_desc_t irq_descriptors[MAX_IRQS];

// Softirq handlers
static softirq_fn_t softirq_handlers[NR_SOFTIRQS];
static void* softirq_data[NR_SOFTIRQS];
static uint32_t softirq_pending = 0;

// Simple spinlock
typedef struct { uint32_t locked; } spinlock_t;
static void spin_lock(spinlock_t* lock) { while (__sync_lock_test_and_set(&lock->locked, 1)); }
static void spin_unlock(spinlock_t* lock) { __sync_lock_release(&lock->locked); }
static void spinlock_init(spinlock_t* lock) { lock->locked = 0; }

/**
 * Initialize IRQ subsystem
 */
int irq_init(void) {
    // Initialize all IRQ descriptors
    for (uint32_t i = 0; i < MAX_IRQS; i++) {
        irq_desc_t* desc = &irq_descriptors[i];
        memset(desc, 0, sizeof(irq_desc_t));
        desc->irq_num = i;
        desc->lock = kmalloc(sizeof(spinlock_t));
        if (desc->lock) {
            spinlock_init((spinlock_t*)desc->lock);
        }
    }

    softirq_init();

    kprintf("[IRQ] IRQ subsystem initialized (%u IRQs)\n", MAX_IRQS);
    return 0;
}

/**
 * Request IRQ handler
 */
int request_irq(uint32_t irq, irq_handler_t handler, uint32_t flags,
               const char* name, void* dev_id) {
    return request_threaded_irq(irq, handler, NULL, flags, name, dev_id);
}

/**
 * Request threaded IRQ handler
 */
int request_threaded_irq(uint32_t irq, irq_handler_t handler,
                        irq_thread_fn_t thread_fn, uint32_t flags,
                        const char* name, void* dev_id) {
    if (irq >= MAX_IRQS || !handler) {
        return -1;
    }

    irq_desc_t* desc = &irq_descriptors[irq];

    spinlock_t* lock = (spinlock_t*)desc->lock;
    if (lock) {
        spin_lock(lock);
    }

    // Check if IRQ is already in use (and not shareable)
    if (desc->action && !(flags & IRQ_FLAG_SHARED)) {
        if (lock) {
            spin_unlock(lock);
        }
        return -1;
    }

    // Allocate action structure
    irq_action_t* action = (irq_action_t*)kmalloc(sizeof(irq_action_t));
    if (!action) {
        if (lock) {
            spin_unlock(lock);
        }
        return -1;
    }

    memset(action, 0, sizeof(irq_action_t));
    action->handler = handler;
    action->thread_fn = thread_fn;
    action->dev_id = dev_id;
    action->name = name;
    action->flags = flags;

    // Add to action list
    action->next = desc->action;
    desc->action = action;

    // Enable IRQ if it has a chip
    if (desc->chip && desc->chip->enable) {
        desc->chip->enable(irq);
        desc->status |= IRQ_STATUS_ENABLED;
    }

    if (lock) {
        spin_unlock(lock);
    }

    kprintf("[IRQ] Requested IRQ %u: %s (flags=0x%x)\n", irq, name, flags);
    return 0;
}

/**
 * Free IRQ handler
 */
void free_irq(uint32_t irq, void* dev_id) {
    if (irq >= MAX_IRQS) {
        return;
    }

    irq_desc_t* desc = &irq_descriptors[irq];

    spinlock_t* lock = (spinlock_t*)desc->lock;
    if (lock) {
        spin_lock(lock);
    }

    // Find and remove action
    irq_action_t** prev = &desc->action;
    irq_action_t* action = desc->action;

    while (action) {
        if (action->dev_id == dev_id) {
            *prev = action->next;
            kfree(action);
            break;
        }
        prev = &action->next;
        action = action->next;
    }

    // Disable IRQ if no more handlers
    if (!desc->action) {
        if (desc->chip && desc->chip->disable) {
            desc->chip->disable(irq);
            desc->status &= ~IRQ_STATUS_ENABLED;
        }
    }

    if (lock) {
        spin_unlock(lock);
    }

    kprintf("[IRQ] Freed IRQ %u\n", irq);
}

/**
 * Disable IRQ (wait for handler to complete)
 * RACE-004 fix: Protect status/depth updates with lock
 */
void disable_irq(uint32_t irq) {
    if (irq >= MAX_IRQS) {
        return;
    }

    irq_desc_t* desc = &irq_descriptors[irq];
    spinlock_t* lock = (spinlock_t*)desc->lock;

    if (lock) spin_lock(lock);

    if (desc->chip && desc->chip->disable) {
        desc->chip->disable(irq);
    }

    desc->depth++;
    desc->status &= ~IRQ_STATUS_ENABLED;
    desc->status |= IRQ_STATUS_DISABLED;

    if (lock) spin_unlock(lock);

    // TODO: Wait for handler to complete
}

/**
 * Disable IRQ (don't wait)
 * RACE-004 fix: Protect status/depth updates with lock
 */
void disable_irq_nosync(uint32_t irq) {
    if (irq >= MAX_IRQS) {
        return;
    }

    irq_desc_t* desc = &irq_descriptors[irq];
    spinlock_t* lock = (spinlock_t*)desc->lock;

    if (lock) spin_lock(lock);

    if (desc->chip && desc->chip->disable) {
        desc->chip->disable(irq);
    }

    desc->depth++;
    desc->status &= ~IRQ_STATUS_ENABLED;
    desc->status |= IRQ_STATUS_DISABLED;

    if (lock) spin_unlock(lock);
}

/**
 * Enable IRQ
 * RACE-004 fix: Protect status/depth updates with lock
 */
void enable_irq(uint32_t irq) {
    if (irq >= MAX_IRQS) {
        return;
    }

    irq_desc_t* desc = &irq_descriptors[irq];
    spinlock_t* lock = (spinlock_t*)desc->lock;

    if (lock) spin_lock(lock);

    if (desc->depth > 0) {
        desc->depth--;
    }

    if (desc->depth == 0) {
        if (desc->chip && desc->chip->enable) {
            desc->chip->enable(irq);
        }
        desc->status |= IRQ_STATUS_ENABLED;
        desc->status &= ~IRQ_STATUS_DISABLED;
    }

    if (lock) spin_unlock(lock);
}

/**
 * Wait for IRQ handler to complete
 */
void synchronize_irq(uint32_t irq) {
    // TODO: Wait for all running handlers
}

/**
 * Set IRQ affinity (CPU mask)
 */
int irq_set_affinity(uint32_t irq, uint32_t cpu_mask) {
    if (irq >= MAX_IRQS) {
        return -1;
    }

    irq_desc_t* desc = &irq_descriptors[irq];

    if (desc->chip && desc->chip->set_affinity) {
        int ret = desc->chip->set_affinity(irq, cpu_mask);
        if (ret == 0) {
            desc->cpu_affinity = cpu_mask;
        }
        return ret;
    }

    return -1;
}

/**
 * Get IRQ affinity
 */
uint32_t irq_get_affinity(uint32_t irq) {
    if (irq >= MAX_IRQS) {
        return 0;
    }

    return irq_descriptors[irq].cpu_affinity;
}

/**
 * Set IRQ chip
 */
int irq_set_chip(uint32_t irq, irq_chip_t* chip) {
    if (irq >= MAX_IRQS || !chip) {
        return -1;
    }

    irq_descriptors[irq].chip = chip;
    return 0;
}

/**
 * Get IRQ chip
 */
irq_chip_t* irq_get_chip(uint32_t irq) {
    if (irq >= MAX_IRQS) {
        return NULL;
    }

    return irq_descriptors[irq].chip;
}

/**
 * Get IRQ descriptor
 */
irq_desc_t* irq_to_desc(uint32_t irq) {
    if (irq >= MAX_IRQS) {
        return NULL;
    }

    return &irq_descriptors[irq];
}

/**
 * Check if IRQ has any actions
 */
bool irq_has_action(uint32_t irq) {
    if (irq >= MAX_IRQS) {
        return false;
    }

    return irq_descriptors[irq].action != NULL;
}

/**
 * Generic IRQ handler (called from low-level ISR)
 */
void generic_handle_irq(uint32_t irq) {
    if (irq >= MAX_IRQS) {
        return;
    }

    irq_desc_t* desc = &irq_descriptors[irq];

    desc->count++;
    desc->status |= IRQ_STATUS_INPROGRESS;

    // Call all registered handlers
    irq_action_t* action = desc->action;
    bool handled = false;

    while (action) {
        irq_return_t ret = action->handler(irq, action->dev_id);

        if (ret == IRQ_HANDLED) {
            handled = true;
            action->count++;
        } else if (ret == IRQ_WAKE_THREAD) {
            handled = true;
            action->count++;
            // TODO: Wake threaded handler
        } else {
            action->unhandled++;
        }

        action = action->next;
    }

    if (!handled) {
        desc->unhandled++;
    }

    // EOI (End of Interrupt)
    if (desc->chip && desc->chip->eoi) {
        desc->chip->eoi(irq);
    }

    desc->status &= ~IRQ_STATUS_INPROGRESS;
}

/**
 * Print IRQ statistics
 */
void irq_print_stats(void) {
    kprintf("[IRQ] IRQ Statistics:\n");
    for (uint32_t i = 0; i < MAX_IRQS; i++) {
        irq_desc_t* desc = &irq_descriptors[i];
        if (desc->action || desc->count > 0) {
            kprintf("  IRQ %3u: count=%llu unhandled=%llu\n",
                    i, desc->count, desc->unhandled);

            irq_action_t* action = desc->action;
            while (action) {
                kprintf("    Handler: %s (count=%llu unhandled=%llu)\n",
                        action->name, action->count, action->unhandled);
                action = action->next;
            }
        }
    }
}

/**
 * Get IRQ count
 */
uint64_t irq_get_count(uint32_t irq) {
    if (irq >= MAX_IRQS) {
        return 0;
    }

    return irq_descriptors[irq].count;
}

/**
 * Get unhandled IRQ count
 */
uint64_t irq_get_unhandled_count(uint32_t irq) {
    if (irq >= MAX_IRQS) {
        return 0;
    }

    return irq_descriptors[irq].unhandled;
}

/**
 * Check if IRQ is spurious
 */
bool irq_is_spurious(uint32_t irq) {
    if (irq >= MAX_IRQS) {
        return false;
    }

    irq_desc_t* desc = &irq_descriptors[irq];
    return (desc->status & IRQ_STATUS_SPURIOUS) != 0;
}

// Softirq implementation

/**
 * Initialize softirq subsystem
 */
void softirq_init(void) {
    memset(softirq_handlers, 0, sizeof(softirq_handlers));
    memset(softirq_data, 0, sizeof(softirq_data));
    softirq_pending = 0;
}

/**
 * Raise softirq
 */
void raise_softirq(uint32_t nr) {
    if (nr >= NR_SOFTIRQS) {
        return;
    }

    __sync_fetch_and_or(&softirq_pending, 1U << nr);
}

/**
 * Register softirq handler
 */
void open_softirq(uint32_t nr, softirq_fn_t func, void* data) {
    if (nr >= NR_SOFTIRQS || !func) {
        return;
    }

    softirq_handlers[nr] = func;
    softirq_data[nr] = data;
}

/**
 * Process pending softirqs
 */
void do_softirq(void) {
    uint32_t pending = __sync_fetch_and_and(&softirq_pending, 0);

    for (uint32_t i = 0; i < NR_SOFTIRQS; i++) {
        if (pending & (1U << i)) {
            if (softirq_handlers[i]) {
                softirq_handlers[i](softirq_data[i]);
            }
        }
    }
}

// Tasklet implementation

/**
 * Initialize tasklet
 */
void tasklet_init(tasklet_t* t, void (*func)(uint64_t), uint64_t data) {
    if (!t || !func) {
        return;
    }

    memset(t, 0, sizeof(tasklet_t));
    t->func = func;
    t->data = data;
}

/**
 * Schedule tasklet
 */
void tasklet_schedule(tasklet_t* t) {
    if (!t) {
        return;
    }

    // TODO: Add to tasklet queue and raise softirq
    raise_softirq(SOFTIRQ_TASKLET);
}

/**
 * Schedule high-priority tasklet
 */
void tasklet_hi_schedule(tasklet_t* t) {
    tasklet_schedule(t);  // For now, same as normal priority
}

/**
 * Kill tasklet
 */
void tasklet_kill(tasklet_t* t) {
    // TODO: Remove from queue and wait for completion
}

// MSI/MSI-X support (placeholders)

int msi_enable(device_t* dev) {
    kprintf("[IRQ] MSI enabled for device %s\n", dev->name);
    return 0;
}

void msi_disable(device_t* dev) {
    kprintf("[IRQ] MSI disabled for device %s\n", dev->name);
}

int msix_enable(device_t* dev, uint32_t num_vectors) {
    kprintf("[IRQ] MSI-X enabled for device %s (%u vectors)\n", dev->name, num_vectors);
    return 0;
}

void msix_disable(device_t* dev) {
    kprintf("[IRQ] MSI-X disabled for device %s\n", dev->name);
}
