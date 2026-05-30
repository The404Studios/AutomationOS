/*
 * Thermal Management
 * Temperature monitoring, cooling, and throttling
 */

#include "../include/power.h"
#include "../include/kernel.h"
#include "../include/io.h"
#include "../include/string.h"

extern power_global_state_t power_global;

// Thermal zone names
static const char* thermal_zone_names[] = {
    "CPU",
    "GPU",
    "ACPI",
    "System",
};

#define NUM_THERMAL_ZONES (sizeof(thermal_zone_names) / sizeof(const char*))

// Temperature thresholds (Celsius * 1000)
#define TEMP_ACTIVE     60000   // 60°C - Turn on fan
#define TEMP_PASSIVE    75000   // 75°C - Start throttling
#define TEMP_HOT        85000   // 85°C - Urgent
#define TEMP_CRITICAL   95000   // 95°C - Emergency shutdown

/**
 * Read CPU temperature from MSR (Model Specific Register)
 */
static uint32_t read_cpu_temperature(void) {
    // TODO: Read from IA32_THERM_STATUS MSR
    // For now, return simulated temperature
    return 45000;  // 45°C
}

/**
 * Read GPU temperature
 */
static uint32_t read_gpu_temperature(void) {
    // TODO: Read from GPU registers
    return 50000;  // 50°C
}

/**
 * Read ACPI thermal zone temperature
 */
static uint32_t read_acpi_temperature(void) {
    // TODO: Read from ACPI thermal zone
    return 40000;  // 40°C
}

/**
 * Set fan speed (0-100%)
 */
static int set_fan_speed(uint32_t percentage) {
    // TODO: Control fan via ACPI or EC (Embedded Controller)
    kprintf("[THERMAL] Setting fan speed to %u%%\n", percentage);
    return 0;
}

/**
 * Initialize thermal management
 */
int thermal_init(void) {
    kprintf("[THERMAL] Initializing thermal management...\n");

    // Allocate thermal zones
    power_global.num_thermal_zones = NUM_THERMAL_ZONES;
    power_global.thermal_zones = kmalloc(sizeof(thermal_zone_t) * NUM_THERMAL_ZONES);
    if (!power_global.thermal_zones) {
        kprintf("[THERMAL] ERROR: Failed to allocate memory\n");
        return -1;
    }

    // Initialize thermal zones
    for (uint32_t i = 0; i < NUM_THERMAL_ZONES; i++) {
        thermal_zone_t* zone = &power_global.thermal_zones[i];

        strcpy(zone->name, thermal_zone_names[i]);
        zone->temperature = 0;

        // Set trip points
        zone->trip_point_active = TEMP_ACTIVE;
        zone->trip_point_passive = TEMP_PASSIVE;
        zone->trip_point_hot = TEMP_HOT;
        zone->trip_point_critical = TEMP_CRITICAL;

        zone->cooling_state = 0;
        zone->max_cooling_state = 10;

        kprintf("[THERMAL] Zone '%s': Trip points: %u/%u/%u/%u°C\n",
                zone->name,
                zone->trip_point_active / 1000,
                zone->trip_point_passive / 1000,
                zone->trip_point_hot / 1000,
                zone->trip_point_critical / 1000);
    }

    // Read initial temperatures
    power_global.thermal_zones[0].temperature = read_cpu_temperature();
    power_global.thermal_zones[1].temperature = read_gpu_temperature();
    power_global.thermal_zones[2].temperature = read_acpi_temperature();
    power_global.thermal_zones[3].temperature = (read_cpu_temperature() + read_gpu_temperature()) / 2;

    return 0;
}

/**
 * Get thermal zone by name
 */
thermal_zone_t* thermal_get_zone(const char* name) {
    for (uint32_t i = 0; i < power_global.num_thermal_zones; i++) {
        if (strcmp(power_global.thermal_zones[i].name, name) == 0) {
            return &power_global.thermal_zones[i];
        }
    }
    return NULL;
}

/**
 * Get temperature of thermal zone
 */
uint32_t thermal_get_temperature(const char* name) {
    thermal_zone_t* zone = thermal_get_zone(name);
    if (!zone) {
        return 0;
    }

    // Update temperature
    if (strcmp(name, "CPU") == 0) {
        zone->temperature = read_cpu_temperature();
    } else if (strcmp(name, "GPU") == 0) {
        zone->temperature = read_gpu_temperature();
    } else if (strcmp(name, "ACPI") == 0) {
        zone->temperature = read_acpi_temperature();
    }

    return zone->temperature;
}

/**
 * Set cooling state
 */
int thermal_set_cooling_state(const char* name, uint32_t state) {
    thermal_zone_t* zone = thermal_get_zone(name);
    if (!zone) {
        return -1;
    }

    if (state > zone->max_cooling_state) {
        state = zone->max_cooling_state;
    }

    zone->cooling_state = state;

    // Apply cooling
    uint32_t fan_speed = (state * 100) / zone->max_cooling_state;
    set_fan_speed(fan_speed);

    return 0;
}

/**
 * Throttle CPU to reduce temperature
 */
int thermal_throttle_cpu(uint32_t cpu, uint32_t percentage) {
    if (cpu >= power_global.num_cpus || percentage > 100) {
        return -1;
    }

    kprintf("[THERMAL] Throttling CPU %u to %u%%\n", cpu, percentage);

    // Reduce max frequency
    cpufreq_policy_t* policy = cpufreq_get_policy(cpu);
    if (!policy) {
        return -1;
    }

    uint32_t throttled_freq = (policy->max_freq_mhz * percentage) / 100;
    cpufreq_set_frequency(cpu, throttled_freq);

    return 0;
}

/**
 * Check trip points and take action
 */
void thermal_check_trip_points(thermal_zone_t* zone) {
    if (!zone) {
        return;
    }

    uint32_t temp = zone->temperature;

    // Critical: Emergency shutdown
    if (temp >= zone->trip_point_critical) {
        kprintf("[THERMAL] CRITICAL: %s temperature %u°C! Shutting down...\n",
                zone->name, temp / 1000);
        power_notify_event(POWER_EVENT_THERMAL_CRITICAL, zone);

        // Emergency shutdown
        timer_sleep(5000);  // Give 5 seconds warning
        acpi_poweroff();
    }

    // Hot: Urgent cooling
    else if (temp >= zone->trip_point_hot) {
        kprintf("[THERMAL] HOT: %s temperature %u°C\n", zone->name, temp / 1000);
        power_notify_event(POWER_EVENT_THERMAL_WARNING, zone);

        // Max cooling
        thermal_set_cooling_state(zone->name, zone->max_cooling_state);

        // Aggressive throttling
        if (strcmp(zone->name, "CPU") == 0) {
            for (uint32_t i = 0; i < power_global.num_cpus; i++) {
                thermal_throttle_cpu(i, 50);  // Throttle to 50%
            }
        }
    }

    // Passive: Start throttling
    else if (temp >= zone->trip_point_passive) {
        if (zone->cooling_state < zone->max_cooling_state * 7 / 10) {
            kprintf("[THERMAL] Passive cooling for %s (%u°C)\n",
                    zone->name, temp / 1000);

            // Increase cooling
            thermal_set_cooling_state(zone->name, zone->max_cooling_state * 7 / 10);

            // Light throttling
            if (strcmp(zone->name, "CPU") == 0) {
                for (uint32_t i = 0; i < power_global.num_cpus; i++) {
                    thermal_throttle_cpu(i, 80);  // Throttle to 80%
                }
            }
        }
    }

    // Active: Turn on fan
    else if (temp >= zone->trip_point_active) {
        if (zone->cooling_state < zone->max_cooling_state * 4 / 10) {
            kprintf("[THERMAL] Active cooling for %s (%u°C)\n",
                    zone->name, temp / 1000);

            // Moderate cooling
            thermal_set_cooling_state(zone->name, zone->max_cooling_state * 4 / 10);
        }
    }

    // Normal: Reduce cooling
    else if (temp < zone->trip_point_active - 5000) {  // 5°C hysteresis
        if (zone->cooling_state > 0) {
            // Reduce cooling gradually
            if (zone->cooling_state > 2) {
                zone->cooling_state -= 2;
            } else {
                zone->cooling_state = 0;
            }

            uint32_t fan_speed = (zone->cooling_state * 100) / zone->max_cooling_state;
            set_fan_speed(fan_speed);

            // Remove throttling
            if (strcmp(zone->name, "CPU") == 0) {
                for (uint32_t i = 0; i < power_global.num_cpus; i++) {
                    thermal_throttle_cpu(i, 100);  // Full speed
                }
            }
        }
    }
}

/**
 * Thermal monitoring thread (called periodically)
 */
void thermal_monitor(void) {
    // Update all thermal zones
    for (uint32_t i = 0; i < power_global.num_thermal_zones; i++) {
        thermal_zone_t* zone = &power_global.thermal_zones[i];

        // Read temperature
        thermal_get_temperature(zone->name);

        // Check trip points
        thermal_check_trip_points(zone);
    }
}
