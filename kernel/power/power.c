/*
 * Power Management Core
 * System sleep states, hibernation, power profiles
 */

#include "../include/power.h"
#include "../include/acpi.h"
#include "../include/kernel.h"
#include "../include/mem.h"
#include "../include/device.h"
#include "../include/string.h"

// Global power state
power_global_state_t power_global = {0};

// Default power profile configurations
static power_profile_config_t power_profiles[3] = {
    // POWER_PROFILE_PERFORMANCE
    {
        .profile = POWER_PROFILE_PERFORMANCE,
        .cpu_governor = CPUFREQ_GOVERNOR_PERFORMANCE,
        .cpu_max_freq_percent = 100,
        .display_blank_sec = 0,         // Never blank
        .display_poweroff_sec = 0,      // Never power off
        .backlight_percent = 100,
        .sleep_timeout_sec = 0,         // Never sleep
        .enable_bluetooth = true,
        .enable_wifi = true,
    },
    // POWER_PROFILE_BALANCED
    {
        .profile = POWER_PROFILE_BALANCED,
        .cpu_governor = CPUFREQ_GOVERNOR_ONDEMAND,
        .cpu_max_freq_percent = 100,
        .display_blank_sec = 300,       // 5 minutes
        .display_poweroff_sec = 600,    // 10 minutes
        .backlight_percent = 100,
        .sleep_timeout_sec = 900,       // 15 minutes
        .enable_bluetooth = true,
        .enable_wifi = true,
    },
    // POWER_PROFILE_POWER_SAVER
    {
        .profile = POWER_PROFILE_POWER_SAVER,
        .cpu_governor = CPUFREQ_GOVERNOR_CONSERVATIVE,
        .cpu_max_freq_percent = 80,
        .display_blank_sec = 120,       // 2 minutes
        .display_poweroff_sec = 300,    // 5 minutes
        .backlight_percent = 80,
        .sleep_timeout_sec = 300,       // 5 minutes
        .enable_bluetooth = false,
        .enable_wifi = true,
    },
};

/**
 * Initialize power management
 */
int power_init(void) {
    kprintf("[POWER] Initializing power management...\n");

    memset(&power_global, 0, sizeof(power_global));

    // Initialize subsystems
    if (battery_init() < 0) {
        kprintf("[POWER] WARNING: Battery init failed\n");
    }

    if (ac_adapter_init() < 0) {
        kprintf("[POWER] WARNING: AC adapter init failed\n");
    }

    if (cpufreq_init() < 0) {
        kprintf("[POWER] WARNING: CPU frequency scaling init failed\n");
    }

    if (cpuidle_init() < 0) {
        kprintf("[POWER] WARNING: CPU idle init failed\n");
    }

    if (thermal_init() < 0) {
        kprintf("[POWER] WARNING: Thermal management init failed\n");
    }

    if (display_power_init() < 0) {
        kprintf("[POWER] WARNING: Display power init failed\n");
    }

    if (backlight_init() < 0) {
        kprintf("[POWER] WARNING: Backlight init failed\n");
    }

    // Set default power profile (balanced)
    power_global.current_state = POWER_STATE_S0;
    power_global.current_profile = POWER_PROFILE_BALANCED;
    power_profile_apply(POWER_PROFILE_BALANCED);

    // Setup default wake sources
    power_global.wake_sources.power_button = true;
    power_global.wake_sources.lid = true;
    power_global.wake_sources.keyboard = true;
    power_global.wake_sources.rtc_alarm = true;

    power_global.initialized = true;

    kprintf("[POWER] Power management initialized\n");
    power_print_info();

    return 0;
}

/**
 * Shutdown power management
 */
void power_shutdown(void) {
    kprintf("[POWER] Shutting down power management...\n");
    memset(&power_global, 0, sizeof(power_global));
}

/**
 * Prepare system for suspend
 */
int power_prepare_suspend(void) {
    kprintf("[POWER] Preparing system for suspend...\n");

    // Sync filesystems
    // TODO: Call filesystem sync

    // Freeze userspace
    // TODO: Freeze all user processes

    // Prepare devices
    device_pm_prepare_all();

    return 0;
}

/**
 * Suspend all devices
 */
int power_suspend_devices(void) {
    kprintf("[POWER] Suspending all devices...\n");
    return device_pm_suspend_all();
}

/**
 * Resume all devices
 */
int power_resume_devices(void) {
    kprintf("[POWER] Resuming all devices...\n");
    return device_pm_resume_all();
}

/**
 * Complete resume
 */
void power_complete_resume(void) {
    kprintf("[POWER] Completing resume...\n");

    device_pm_complete_all();

    // Thaw userspace
    // TODO: Unfreeze all user processes

    kprintf("[POWER] Resume complete\n");
}

/**
 * Suspend to RAM (S3 sleep)
 */
int power_suspend_to_ram(void) {
    kprintf("[POWER] Suspending to RAM (S3)...\n");

    // Check if S3 is supported
    if (!acpi_is_enabled()) {
        kprintf("[POWER] ERROR: ACPI not enabled\n");
        return -1;
    }

    // Prepare suspend
    if (power_prepare_suspend() < 0) {
        kprintf("[POWER] ERROR: Failed to prepare suspend\n");
        return -1;
    }

    // Suspend devices
    if (power_suspend_devices() < 0) {
        kprintf("[POWER] ERROR: Failed to suspend devices\n");
        power_complete_resume();
        return -1;
    }

    // Save CPU state
    // TODO: Save CPU registers, page tables, etc.

    // Notify event handlers
    power_notify_event(POWER_EVENT_NONE, NULL);

    // Enter S3 via ACPI
    acpi_prepare_sleep(ACPI_STATE_S3);
    power_global.current_state = POWER_STATE_S3;

    // This will suspend the system
    acpi_enter_sleep_state(ACPI_STATE_S3);

    // ===== System suspended =====
    // ===== Wake from S3 =====

    // Restore CPU state
    // TODO: Restore CPU registers, page tables, etc.

    power_global.current_state = POWER_STATE_S0;

    // Wake from sleep
    acpi_wake_from_sleep();

    // Resume devices
    power_resume_devices();

    // Complete resume
    power_complete_resume();

    kprintf("[POWER] Resumed from S3\n");
    return 0;
}

/**
 * Suspend to idle (freeze, no ACPI)
 */
int power_suspend_to_idle(void) {
    kprintf("[POWER] Suspending to idle (freeze)...\n");

    // Prepare suspend
    if (power_prepare_suspend() < 0) {
        return -1;
    }

    // Suspend devices
    if (power_suspend_devices() < 0) {
        power_complete_resume();
        return -1;
    }

    power_global.current_state = POWER_STATE_S1;

    // Enter idle state (CPU halt)
    // Wait for interrupt to wake
    cli();
    halt();
    sti();

    power_global.current_state = POWER_STATE_S0;

    // Resume devices
    power_resume_devices();
    power_complete_resume();

    kprintf("[POWER] Resumed from idle\n");
    return 0;
}

/**
 * Create hibernation image
 */
int hibernation_create_image(const char* device) {
    kprintf("[POWER] Creating hibernation image...\n");

    hibernation_header_t header = {0};
    header.magic = HIBERNATION_MAGIC;
    header.version = HIBERNATION_VERSION;
    header.timestamp = timer_get_ticks();

    // Get memory size
    // TODO: Get actual memory size
    header.memory_size = 0;
    header.num_pages = 0;

    // Save CPU state
    // TODO: Save registers

    // Write image to disk
    return hibernation_write_image(&header, device);
}

/**
 * Write hibernation image to disk
 */
int hibernation_write_image(hibernation_header_t* header, const char* device) {
    kprintf("[POWER] Writing hibernation image to %s...\n", device);

    // TODO: Write header and memory pages to disk
    // For now, just return success

    kprintf("[POWER] Hibernation image written\n");
    return 0;
}

/**
 * Read hibernation image from disk
 */
int hibernation_read_image(const char* device) {
    kprintf("[POWER] Reading hibernation image from %s...\n", device);

    // TODO: Read header and verify magic/version
    // TODO: Read memory pages from disk

    kprintf("[POWER] Hibernation image read\n");
    return 0;
}

/**
 * Restore from hibernation image
 */
int hibernation_restore_image(const char* device) {
    kprintf("[POWER] Restoring from hibernation image...\n");

    // Read image
    if (hibernation_read_image(device) < 0) {
        return -1;
    }

    // TODO: Restore memory pages
    // TODO: Restore CPU state
    // TODO: Resume devices

    kprintf("[POWER] Restored from hibernation\n");
    return 0;
}

/**
 * Hibernate (S4 - suspend to disk)
 */
int power_hibernate(void) {
    kprintf("[POWER] Hibernating (S4)...\n");

    // Check if hibernation is supported
    if (!acpi_is_enabled()) {
        kprintf("[POWER] ERROR: ACPI not enabled\n");
        return -1;
    }

    // Prepare suspend
    if (power_prepare_suspend() < 0) {
        kprintf("[POWER] ERROR: Failed to prepare suspend\n");
        return -1;
    }

    // Suspend devices
    if (power_suspend_devices() < 0) {
        kprintf("[POWER] ERROR: Failed to suspend devices\n");
        power_complete_resume();
        return -1;
    }

    // Create hibernation image
    if (hibernation_create_image("/dev/sda2") < 0) {
        kprintf("[POWER] ERROR: Failed to create hibernation image\n");
        power_resume_devices();
        power_complete_resume();
        return -1;
    }

    power_global.current_state = POWER_STATE_S4;

    // Power off
    acpi_prepare_sleep(ACPI_STATE_S5);
    acpi_enter_sleep_state(ACPI_STATE_S5);

    // Should not return
    return 0;
}

/**
 * Resume from sleep/hibernation
 */
int power_resume(void) {
    kprintf("[POWER] Resuming...\n");

    power_global.current_state = POWER_STATE_S0;

    // Wake from ACPI sleep
    acpi_wake_from_sleep();

    // Resume devices
    power_resume_devices();

    // Complete resume
    power_complete_resume();

    return 0;
}

/**
 * Handle power button press
 */
void power_handle_power_button(void) {
    kprintf("[POWER] Power button pressed\n");

    // Default action: suspend to RAM
    power_notify_event(POWER_EVENT_POWER_BUTTON, NULL);

    // Give userspace a chance to handle it
    timer_sleep(2000);  // 2 seconds

    // If not handled, suspend
    power_suspend_to_ram();
}

/**
 * Handle sleep button press
 */
void power_handle_sleep_button(void) {
    kprintf("[POWER] Sleep button pressed\n");

    power_notify_event(POWER_EVENT_SLEEP_BUTTON, NULL);

    // Suspend to RAM
    power_suspend_to_ram();
}

/**
 * Handle lid event
 */
void power_handle_lid_event(bool closed) {
    kprintf("[POWER] Lid %s\n", closed ? "closed" : "opened");

    if (closed) {
        power_notify_event(POWER_EVENT_LID_CLOSED, NULL);

        // If on battery, suspend when lid closes
        if (!ac_adapter_is_online()) {
            timer_sleep(1000);  // 1 second delay
            power_suspend_to_ram();
        }
    } else {
        power_notify_event(POWER_EVENT_LID_OPENED, NULL);
    }
}

/**
 * Get power statistics
 */
power_stats_t* power_get_stats(void) {
    return &power_global.stats;
}

/**
 * Update power statistics
 */
void power_update_stats(void) {
    power_stats_t* stats = &power_global.stats;

    stats->current_state = power_global.current_state;
    stats->uptime_sec = timer_get_ticks() / 1000;

    // Battery info
    stats->on_battery = !ac_adapter_is_online();
    stats->battery_percent = battery_get_percentage();
    stats->time_remaining_min = battery_get_time_remaining();

    // Power draw estimation
    stats->power_draw_watts = power_estimate_draw();

    // CPU info
    if (power_global.num_cpus > 0) {
        stats->cpu_freq_avg_mhz = cpufreq_get_frequency(0);
    }

    // Display info
    stats->display_on = !power_global.display.powered_off;
    stats->backlight_percent = power_global.display.backlight_percentage;
}

/**
 * Reset power statistics
 */
void power_reset_stats(void) {
    memset(&power_global.stats, 0, sizeof(power_stats_t));
}

/**
 * Estimate current power draw
 */
uint32_t power_estimate_draw(void) {
    uint32_t watts = 0;

    // Base system power
    watts += 5;  // 5W base

    // CPU power (depends on frequency and load)
    if (power_global.num_cpus > 0) {
        uint32_t freq = cpufreq_get_frequency(0);
        // Rough estimate: 0.5W per GHz per core
        watts += (freq * power_global.num_cpus) / 2000;
    }

    // Display power
    if (!power_global.display.powered_off) {
        // Rough estimate: up to 3W for backlight
        watts += (power_global.display.backlight_percentage * 3) / 100;
    }

    // Battery charging
    if (power_global.battery.state == BATTERY_STATE_CHARGING) {
        watts += 10;  // 10W for charging
    }

    return watts;
}

/**
 * Estimate battery life remaining
 */
uint32_t power_estimate_battery_life(void) {
    if (!power_global.battery.present || power_global.battery.current_ma >= 0) {
        return 0;  // No battery or charging
    }

    // remaining_mah / current_ma = hours
    // * 60 = minutes
    uint32_t current_ma = -power_global.battery.current_ma;
    if (current_ma == 0) {
        return 0;
    }

    return (power_global.battery.remaining_mah * 60) / current_ma;
}

/**
 * Estimate time to full charge
 */
uint32_t power_estimate_time_to_full(void) {
    if (!power_global.battery.present || power_global.battery.current_ma <= 0) {
        return 0;  // No battery or not charging
    }

    uint32_t remaining = power_global.battery.capacity_mah - power_global.battery.remaining_mah;
    uint32_t current_ma = power_global.battery.current_ma;

    if (current_ma == 0) {
        return 0;
    }

    return (remaining * 60) / current_ma;
}

/**
 * Set wake sources
 */
int power_set_wake_sources(wake_sources_t* sources) {
    if (!sources) {
        return -1;
    }

    memcpy(&power_global.wake_sources, sources, sizeof(wake_sources_t));
    kprintf("[POWER] Wake sources updated\n");
    return 0;
}

/**
 * Get wake sources
 */
wake_sources_t* power_get_wake_sources(void) {
    return &power_global.wake_sources;
}

/**
 * Print power information
 */
void power_print_info(void) {
    kprintf("\n[POWER] ===== Power Management Info =====\n");
    kprintf("[POWER] State: S%d\n", power_global.current_state);
    kprintf("[POWER] Profile: %s\n",
            power_global.current_profile == POWER_PROFILE_PERFORMANCE ? "Performance" :
            power_global.current_profile == POWER_PROFILE_BALANCED ? "Balanced" :
            "Power Saver");

    if (power_global.battery.present) {
        kprintf("[POWER] Battery: %u%% (%s)\n",
                power_global.battery.percentage,
                power_global.battery.state == BATTERY_STATE_CHARGING ? "Charging" :
                power_global.battery.state == BATTERY_STATE_DISCHARGING ? "Discharging" :
                power_global.battery.state == BATTERY_STATE_FULL ? "Full" :
                "Unknown");
    }

    kprintf("[POWER] AC Adapter: %s\n",
            power_global.ac_adapter.online ? "Online" : "Offline");

    kprintf("[POWER] ==============================\n\n");
}

/**
 * Dump battery information
 */
void power_dump_battery_info(void) {
    if (!power_global.battery.present) {
        kprintf("[POWER] No battery present\n");
        return;
    }

    battery_info_t* bat = &power_global.battery;

    kprintf("\n[POWER] ===== Battery Information =====\n");
    kprintf("[POWER] Manufacturer: %s\n", bat->manufacturer);
    kprintf("[POWER] Model: %s\n", bat->model);
    kprintf("[POWER] Technology: %s\n", bat->technology);
    kprintf("[POWER] Capacity: %u mAh\n", bat->capacity_mah);
    kprintf("[POWER] Remaining: %u mAh (%u%%)\n", bat->remaining_mah, bat->percentage);
    kprintf("[POWER] Voltage: %u mV\n", bat->voltage_mv);
    kprintf("[POWER] Current: %d mA\n", bat->current_ma);
    kprintf("[POWER] Temperature: %u.%u°C\n", bat->temperature / 10, bat->temperature % 10);
    kprintf("[POWER] Health: %u%%\n", bat->health_percentage);
    kprintf("[POWER] Cycle Count: %u\n", bat->cycle_count);

    if (bat->state == BATTERY_STATE_DISCHARGING && bat->time_to_empty_min > 0) {
        kprintf("[POWER] Time to empty: %u:%02u\n",
                bat->time_to_empty_min / 60, bat->time_to_empty_min % 60);
    } else if (bat->state == BATTERY_STATE_CHARGING && bat->time_to_full_min > 0) {
        kprintf("[POWER] Time to full: %u:%02u\n",
                bat->time_to_full_min / 60, bat->time_to_full_min % 60);
    }

    kprintf("[POWER] ===========================\n\n");
}

/**
 * Dump thermal zones
 */
void power_dump_thermal_zones(void) {
    kprintf("\n[POWER] ===== Thermal Zones =====\n");

    for (uint32_t i = 0; i < power_global.num_thermal_zones; i++) {
        thermal_zone_t* zone = &power_global.thermal_zones[i];
        kprintf("[POWER] %s: %u.%03u°C (cooling state %u/%u)\n",
                zone->name,
                zone->temperature / 1000,
                zone->temperature % 1000,
                zone->cooling_state,
                zone->max_cooling_state);
    }

    kprintf("[POWER] ===========================\n\n");
}

/**
 * Dump CPU information
 */
void power_dump_cpu_info(void) {
    kprintf("\n[POWER] ===== CPU Information =====\n");

    for (uint32_t i = 0; i < power_global.num_cpus; i++) {
        cpufreq_policy_t* policy = &power_global.cpu_policies[i];
        kprintf("[POWER] CPU %u: %u MHz (min %u, max %u)\n",
                i, policy->current_freq_mhz,
                policy->min_freq_mhz, policy->max_freq_mhz);
        kprintf("           Governor: %s\n",
                policy->governor == CPUFREQ_GOVERNOR_PERFORMANCE ? "performance" :
                policy->governor == CPUFREQ_GOVERNOR_POWERSAVE ? "powersave" :
                policy->governor == CPUFREQ_GOVERNOR_ONDEMAND ? "ondemand" :
                policy->governor == CPUFREQ_GOVERNOR_CONSERVATIVE ? "conservative" :
                "userspace");
    }

    kprintf("[POWER] ===========================\n\n");
}
