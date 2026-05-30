/*
 * Input Event System
 * Manages input devices and events from keyboards, mice, and gamepads
 */

#include "../../include/input.h"
#include "../../include/kernel.h"
#include "../../include/types.h"
#include "../../include/mem.h"
#include "../../include/x86_64.h"
#include "../../include/string.h"

#define MAX_INPUT_DEVICES 16
#define GLOBAL_EVENT_QUEUE_SIZE 512

// Global input device registry
static input_device_t* input_devices[MAX_INPUT_DEVICES];
static uint32_t num_input_devices = 0;
static uint32_t next_device_id = 1;

// Global event queue (for userspace consumption)
static input_event_t global_event_queue[GLOBAL_EVENT_QUEUE_SIZE];
static volatile uint32_t global_queue_head = 0;
static volatile uint32_t global_queue_tail = 0;

// Timer function to get current timestamp (microseconds since boot)
extern uint64_t timer_get_ticks(void);
extern uint32_t timer_get_frequency(void);

static uint64_t input_get_timestamp(void) {
    uint64_t ticks = timer_get_ticks();
    uint32_t freq = timer_get_frequency();
    if (freq == 0) freq = 1000;  // Default to 1000 Hz if not initialized
    return (ticks * 1000000ULL) / freq;
}

// Initialize input subsystem
void input_init(void) {
    kprintf("[INPUT] Initializing input subsystem\n");
    memset(input_devices, 0, sizeof(input_devices));
    memset(global_event_queue, 0, sizeof(global_event_queue));
    num_input_devices = 0;
    global_queue_head = 0;
    global_queue_tail = 0;
}

// Allocate input device
input_device_t* input_allocate_device(const char* name) {
    input_device_t* dev = (input_device_t*)kmalloc(sizeof(input_device_t));
    if (!dev) {
        kprintf("[INPUT] Failed to allocate device\n");
        return NULL;
    }

    memset(dev, 0, sizeof(input_device_t));

    // Copy name
    size_t name_len = 0;
    while (name[name_len] && name_len < 63) name_len++;
    memcpy(dev->name, name, name_len);
    dev->name[name_len] = '\0';

    // Allocate event queue
    dev->queue_size = 128;
    dev->event_queue = (input_event_t*)kmalloc(sizeof(input_event_t) * dev->queue_size);
    if (!dev->event_queue) {
        kprintf("[INPUT] Failed to allocate event queue\n");
        kfree(dev);
        return NULL;
    }

    memset(dev->event_queue, 0, sizeof(input_event_t) * dev->queue_size);
    dev->queue_head = 0;
    dev->queue_tail = 0;

    return dev;
}

// Free input device
void input_free_device(input_device_t* dev) {
    if (!dev) return;

    if (dev->event_queue) {
        kfree(dev->event_queue);
    }

    kfree(dev);
}

// Register input device
int input_register_device(input_device_t* dev) {
    if (!dev) return -1;

    if (num_input_devices >= MAX_INPUT_DEVICES) {
        kprintf("[INPUT] Maximum number of devices reached\n");
        return -1;
    }

    dev->id = next_device_id++;
    input_devices[num_input_devices++] = dev;

    kprintf("[INPUT] Registered device #%u: %s\n", dev->id, dev->name);
    kprintf("[INPUT]   Key: %s, Rel: %s, Abs: %s, LED: %s\n",
            dev->supports_key ? "yes" : "no",
            dev->supports_rel ? "yes" : "no",
            dev->supports_abs ? "yes" : "no",
            dev->supports_led ? "yes" : "no");

    // Create the /dev/input/eventN VFS node so userspace can open the device.
    // (Requires /dev/input to already exist, i.e. dev_input_init() ran first.)
    extern int evdev_register_device(input_device_t* dev);
    if (evdev_register_device(dev) < 0) {
        kprintf("[INPUT] WARN: evdev node creation failed for %s\n", dev->name);
    }

    return 0;
}

// Unregister input device
void input_unregister_device(input_device_t* dev) {
    if (!dev) return;

    for (uint32_t i = 0; i < num_input_devices; i++) {
        if (input_devices[i] == dev) {
            // Shift remaining devices
            for (uint32_t j = i; j < num_input_devices - 1; j++) {
                input_devices[j] = input_devices[j + 1];
            }
            num_input_devices--;
            input_devices[num_input_devices] = NULL;

            kprintf("[INPUT] Unregistered device #%u: %s\n", dev->id, dev->name);
            return;
        }
    }
}

// Add event to device queue
static void input_add_to_device_queue(input_device_t* dev, input_event_t* event) {
    uint32_t next_tail = (dev->queue_tail + 1) % dev->queue_size;

    if (next_tail == dev->queue_head) {
        // Queue full, drop oldest event
        kprintf("[INPUT] Device queue full, dropping event\n");
        dev->queue_head = (dev->queue_head + 1) % dev->queue_size;
    }

    dev->event_queue[dev->queue_tail] = *event;
    dev->queue_tail = next_tail;
}

// Add event to global queue
static void input_add_to_global_queue(input_event_t* event) {
    uint32_t next_tail = (global_queue_tail + 1) % GLOBAL_EVENT_QUEUE_SIZE;

    if (next_tail == global_queue_head) {
        // Queue full, drop oldest event
        global_queue_head = (global_queue_head + 1) % GLOBAL_EVENT_QUEUE_SIZE;
    }

    global_event_queue[global_queue_tail] = *event;
    global_queue_tail = next_tail;
}

// Report key event
void input_report_key(input_device_t* dev, uint16_t keycode, int32_t value) {
    if (!dev || !dev->supports_key) return;

    input_event_t event;
    event.timestamp = input_get_timestamp();
    event.type = INPUT_EVENT_KEY;
    event.code = keycode;
    event.value = value;

    input_add_to_device_queue(dev, &event);
    input_add_to_global_queue(&event);

    // Forward to evdev
    evdev_handle_event(dev, &event);
}

// Report relative movement
void input_report_rel(input_device_t* dev, uint16_t axis, int32_t value) {
    if (!dev || !dev->supports_rel) return;

    input_event_t event;
    event.timestamp = input_get_timestamp();
    event.type = INPUT_EVENT_REL;
    event.code = axis;
    event.value = value;

    input_add_to_device_queue(dev, &event);
    input_add_to_global_queue(&event);

    // Forward to evdev
    evdev_handle_event(dev, &event);
}

// Report absolute position
void input_report_abs(input_device_t* dev, uint16_t axis, int32_t value) {
    if (!dev || !dev->supports_abs) return;

    input_event_t event;
    event.timestamp = input_get_timestamp();
    event.type = INPUT_EVENT_ABS;
    event.code = axis;
    event.value = value;

    input_add_to_device_queue(dev, &event);
    input_add_to_global_queue(&event);

    // Forward to evdev
    evdev_handle_event(dev, &event);
}

// Synchronize events (marks end of event batch)
void input_sync(input_device_t* dev) {
    if (!dev) return;

    // In a more complete implementation, we would send a SYN event here
    // For now, this is just a marker that the driver has finished sending
    // a batch of events
}

// Get event from global queue (for userspace)
int input_get_event(input_event_t* event) {
    if (!event) return -1;

    if (global_queue_head == global_queue_tail) {
        // Queue empty
        return 0;
    }

    *event = global_event_queue[global_queue_head];
    global_queue_head = (global_queue_head + 1) % GLOBAL_EVENT_QUEUE_SIZE;

    return 1;
}

// Get device by ID
input_device_t* input_get_device(uint32_t id) {
    for (uint32_t i = 0; i < num_input_devices; i++) {
        if (input_devices[i]->id == id) {
            return input_devices[i];
        }
    }
    return NULL;
}

// Get number of registered devices
uint32_t input_get_device_count(void) {
    return num_input_devices;
}

// List all devices (for debugging)
void input_list_devices(void) {
    kprintf("[INPUT] Registered devices (%u):\n", num_input_devices);
    for (uint32_t i = 0; i < num_input_devices; i++) {
        input_device_t* dev = input_devices[i];
        kprintf("[INPUT]   #%u: %s (caps: %c%c%c%c)\n",
                dev->id, dev->name,
                dev->supports_key ? 'K' : '-',
                dev->supports_rel ? 'R' : '-',
                dev->supports_abs ? 'A' : '-',
                dev->supports_led ? 'L' : '-');
    }
}

// Debug: print event details
void input_debug_event(input_event_t* event) {
    const char* type_names[] = {
        "KEY", "REL", "ABS", "MSC", "SW", "LED", "SND", "REP"
    };

    const char* type_name = (event->type < 8) ? type_names[event->type] : "UNK";

    kprintf("[INPUT] Event: type=%s code=%u value=%d timestamp=%llu\n",
            type_name, event->code, event->value, event->timestamp);
}
