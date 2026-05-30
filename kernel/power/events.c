/*
 * Power Event Handling
 * Power button, lid, battery events
 */

#include "../include/power.h"
#include "../include/kernel.h"

extern power_global_state_t power_global;

/**
 * Initialize power event system
 */
int power_event_init(void) {
    kprintf("[POWER] Initializing power event system...\n");

    // Clear event handlers
    for (int i = 0; i < 16; i++) {
        power_global.event_handlers[i] = NULL;
    }

    return 0;
}

/**
 * Register power event handler
 */
int power_register_event_handler(power_event_t event, power_event_handler_t handler) {
    if (event >= 16 || !handler) {
        return -1;
    }

    power_global.event_handlers[event] = handler;
    kprintf("[POWER] Registered handler for event %u\n", event);
    return 0;
}

/**
 * Unregister power event handler
 */
int power_unregister_event_handler(power_event_t event) {
    if (event >= 16) {
        return -1;
    }

    power_global.event_handlers[event] = NULL;
    return 0;
}

/**
 * Notify power event
 */
void power_notify_event(power_event_t event, void* data) {
    const char* event_name = "Unknown";

    switch (event) {
        case POWER_EVENT_POWER_BUTTON:
            event_name = "Power Button";
            break;
        case POWER_EVENT_SLEEP_BUTTON:
            event_name = "Sleep Button";
            break;
        case POWER_EVENT_LID_CLOSED:
            event_name = "Lid Closed";
            break;
        case POWER_EVENT_LID_OPENED:
            event_name = "Lid Opened";
            break;
        case POWER_EVENT_AC_PLUGGED:
            event_name = "AC Plugged";
            break;
        case POWER_EVENT_AC_UNPLUGGED:
            event_name = "AC Unplugged";
            break;
        case POWER_EVENT_BATTERY_LOW:
            event_name = "Battery Low";
            break;
        case POWER_EVENT_BATTERY_CRITICAL:
            event_name = "Battery Critical";
            break;
        case POWER_EVENT_THERMAL_WARNING:
            event_name = "Thermal Warning";
            break;
        case POWER_EVENT_THERMAL_CRITICAL:
            event_name = "Thermal Critical";
            break;
        default:
            break;
    }

    kprintf("[POWER] Event: %s\n", event_name);

    // Call registered handler
    if (event < 16 && power_global.event_handlers[event]) {
        power_global.event_handlers[event](event, data);
    }

    // TODO: Send event to userspace (via netlink or similar)
}
