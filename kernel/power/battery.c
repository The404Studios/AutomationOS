/*
 * Battery Management
 * Battery monitoring, AC adapter detection, power events
 */

#include "../include/power.h"
#include "../include/acpi.h"
#include "../include/kernel.h"
#include "../include/io.h"
#include "../include/string.h"

// ACPI Battery device paths
#define ACPI_BATTERY_PATH       "/sys/class/power_supply/BAT0"
#define ACPI_AC_ADAPTER_PATH    "/sys/class/power_supply/AC"

// Battery update interval (milliseconds)
#define BATTERY_UPDATE_INTERVAL 30000

extern power_global_state_t power_global;

// Last battery check time
static uint64_t last_battery_update = 0;

/**
 * Read ACPI battery information (stub)
 * In a real implementation, this would read from ACPI _BIF/_BST methods
 */
static int read_acpi_battery_info(battery_info_t* info) {
    // TODO: Implement ACPI battery information reading
    // For now, return simulated data

    info->present = true;
    info->state = BATTERY_STATE_DISCHARGING;

    info->capacity_mah = 5000;        // 5000 mAh capacity
    info->remaining_mah = 4250;       // 85% charged
    info->percentage = 85;

    info->voltage_mv = 11400;         // 11.4V
    info->current_ma = -1500;         // Discharging at 1.5A
    info->temperature = 350;          // 35.0°C

    // Calculate time to empty
    if (info->current_ma < 0) {
        info->time_to_empty_min = (info->remaining_mah * 60) / (-info->current_ma);
    } else {
        info->time_to_empty_min = 0;
    }

    info->time_to_full_min = 0;

    info->cycle_count = 123;
    info->health_percentage = 95;

    strcpy(info->manufacturer, "Generic");
    strcpy(info->model, "Li-ion Battery");
    strcpy(info->serial, "12345");
    strcpy(info->technology, "Li-ion");

    return 0;
}

/**
 * Read AC adapter status (stub)
 */
static int read_acpi_ac_adapter(ac_adapter_t* ac) {
    // TODO: Implement ACPI AC adapter status reading
    // For now, return simulated data

    ac->present = true;
    ac->online = false;  // On battery
    strcpy(ac->type, "Mains");

    return 0;
}

/**
 * Initialize battery subsystem
 */
int battery_init(void) {
    kprintf("[BATTERY] Initializing battery subsystem...\n");

    // Try to read battery information
    if (read_acpi_battery_info(&power_global.battery) < 0) {
        kprintf("[BATTERY] No battery found\n");
        power_global.battery.present = false;
        return 0;  // Not an error, just no battery
    }

    kprintf("[BATTERY] Battery found: %s %s\n",
            power_global.battery.manufacturer,
            power_global.battery.model);
    kprintf("[BATTERY] Capacity: %u mAh, Health: %u%%\n",
            power_global.battery.capacity_mah,
            power_global.battery.health_percentage);

    last_battery_update = timer_get_ticks();

    return 0;
}

/**
 * Update battery information
 */
int battery_update(void) {
    uint64_t now = timer_get_ticks();

    // Don't update too frequently
    if (now - last_battery_update < BATTERY_UPDATE_INTERVAL) {
        return 0;
    }

    if (!power_global.battery.present) {
        return -1;
    }

    battery_state_t old_state = power_global.battery.state;
    uint32_t old_percentage = power_global.battery.percentage;

    // Read updated battery information
    if (read_acpi_battery_info(&power_global.battery) < 0) {
        return -1;
    }

    last_battery_update = now;

    // Check for state changes
    if (old_state != power_global.battery.state) {
        if (power_global.battery.state == BATTERY_STATE_CHARGING) {
            kprintf("[BATTERY] Now charging\n");
        } else if (power_global.battery.state == BATTERY_STATE_DISCHARGING) {
            kprintf("[BATTERY] Now discharging\n");
        } else if (power_global.battery.state == BATTERY_STATE_FULL) {
            kprintf("[BATTERY] Fully charged\n");
        }
    }

    // Check for low battery
    if (power_global.battery.percentage <= 5 && old_percentage > 5) {
        kprintf("[BATTERY] CRITICAL: Battery at %u%%\n", power_global.battery.percentage);
        power_notify_event(POWER_EVENT_BATTERY_CRITICAL, &power_global.battery);
    } else if (power_global.battery.percentage <= 10 && old_percentage > 10) {
        kprintf("[BATTERY] WARNING: Battery at %u%%\n", power_global.battery.percentage);
        power_notify_event(POWER_EVENT_BATTERY_LOW, &power_global.battery);
    }

    return 0;
}

/**
 * Get battery information
 */
battery_info_t* battery_get_info(void) {
    // Update if needed
    battery_update();
    return &power_global.battery;
}

/**
 * Check if battery is present
 */
bool battery_is_present(void) {
    return power_global.battery.present;
}

/**
 * Get battery percentage
 */
uint32_t battery_get_percentage(void) {
    battery_update();
    return power_global.battery.percentage;
}

/**
 * Get battery state
 */
battery_state_t battery_get_state(void) {
    battery_update();
    return power_global.battery.state;
}

/**
 * Get time remaining (minutes)
 */
uint32_t battery_get_time_remaining(void) {
    battery_update();

    if (power_global.battery.state == BATTERY_STATE_DISCHARGING) {
        return power_global.battery.time_to_empty_min;
    } else if (power_global.battery.state == BATTERY_STATE_CHARGING) {
        return power_global.battery.time_to_full_min;
    }

    return 0;
}

/**
 * Initialize AC adapter subsystem
 */
int ac_adapter_init(void) {
    kprintf("[AC] Initializing AC adapter subsystem...\n");

    // Read AC adapter status
    if (read_acpi_ac_adapter(&power_global.ac_adapter) < 0) {
        kprintf("[AC] No AC adapter found\n");
        return -1;
    }

    kprintf("[AC] AC adapter: %s (%s)\n",
            power_global.ac_adapter.online ? "Online" : "Offline",
            power_global.ac_adapter.type);

    return 0;
}

/**
 * Get AC adapter information
 */
ac_adapter_t* ac_adapter_get_info(void) {
    // Re-read status
    read_acpi_ac_adapter(&power_global.ac_adapter);
    return &power_global.ac_adapter;
}

/**
 * Check if AC adapter is online
 */
bool ac_adapter_is_online(void) {
    read_acpi_ac_adapter(&power_global.ac_adapter);
    return power_global.ac_adapter.online;
}
